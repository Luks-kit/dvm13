#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Platform detection ───────────────────────────────────────────────────────

#if defined(_WIN32)
#  define DVM_WINDOWS
#  include <windows.h>
#elif defined(__APPLE__)
#  define DVM_APPLE
#  include <sys/mman.h>
#  include <unistd.h>
#  include <time.h>
#  include <fcntl.h>
#elif defined(__linux__)
#  define DVM_LINUX
#  include <sys/mman.h>
#  include <unistd.h>
#  include <time.h>
#  include <fcntl.h>
#else
#  error "Unsupported platform"
#endif

// ─── dvm_mmap / dvm_munmap / dvm_mprotect ────────────────────────────────────

static int prot_to_native(int prot) {
#if defined(DVM_WINDOWS)
    // Windows uses PAGE_* constants, handled separately
    (void)prot;
    return 0;
#else
    int np = 0;
    if (prot & DVM_MMAP_READ)  np |= PROT_READ;
    if (prot & DVM_MMAP_WRITE) np |= PROT_WRITE;
    if (prot & DVM_MMAP_EXEC)  np |= PROT_EXEC;
    return np;
#endif
}

#if defined(DVM_WINDOWS)
static DWORD prot_to_win(int prot) {
    int rw  = (prot & DVM_MMAP_RW)  == DVM_MMAP_RW;
    int rx  = (prot & DVM_MMAP_READ) && (prot & DVM_MMAP_EXEC);
    int rwx = (prot & DVM_MMAP_RWX) == DVM_MMAP_RWX;
    if (rwx) return PAGE_EXECUTE_READWRITE;
    if (rx)  return PAGE_EXECUTE_READ;
    if (rw)  return PAGE_READWRITE;
    if (prot & DVM_MMAP_READ)  return PAGE_READONLY;
    return PAGE_NOACCESS;
}
#endif

void *dvm_mmap(size_t len, int prot) {
#if defined(DVM_WINDOWS)
    return VirtualAlloc(NULL, len, MEM_COMMIT|MEM_RESERVE, prot_to_win(prot));
#else
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    void *p   = mmap(NULL, len, prot_to_native(prot), flags, -1, 0);
    if (p == MAP_FAILED) { perror("dvm_mmap"); abort(); }
    return p;
#endif
}

void dvm_munmap(void *ptr, size_t len) {
#if defined(DVM_WINDOWS)
    (void)len;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, len);
#endif
}

void dvm_mprotect(void *ptr, size_t len, int prot) {
#if defined(DVM_WINDOWS)
    DWORD old;
    VirtualProtect(ptr, len, prot_to_win(prot), &old);
#else
    mprotect(ptr, len, prot_to_native(prot));
#endif
}

// ─── Syscall dispatch ─────────────────────────────────────────────────────────

int dvm_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t *ret) {
    (void)a4; (void)a5;  // not used by most calls below
    *ret = (uint64_t)-1;

    switch ((DvmSyscall)nr) {

    case DVM_SYS_EXIT:
        exit((int)a1);

    case DVM_SYS_READ: {
#if defined(DVM_WINDOWS)
        HANDLE h  = (HANDLE)(uintptr_t)a1;
        DWORD got = 0;
        BOOL ok   = ReadFile(h, (void*)(uintptr_t)a2, (DWORD)a3, &got, NULL);
        *ret      = ok ? got : (uint64_t)-1;
#else
        *ret = (uint64_t)read((int)a1, (void*)(uintptr_t)a2, (size_t)a3);
#endif
        return 0;
    }

    case DVM_SYS_WRITE: {
#if defined(DVM_WINDOWS)
        HANDLE h    = (HANDLE)(uintptr_t)a1;
        DWORD wrote = 0;
        BOOL ok     = WriteFile(h, (void*)(uintptr_t)a2, (DWORD)a3, &wrote, NULL);
        *ret        = ok ? wrote : (uint64_t)-1;
#else
        *ret = (uint64_t)write((int)a1, (void*)(uintptr_t)a2, (size_t)a3);
#endif
        return 0;
    }

    case DVM_SYS_OPEN: {
#if defined(DVM_WINDOWS)
        // a1=path, a2=flags (O_RDONLY=0 O_WRONLY=1 O_RDWR=2), a3=mode
        DWORD acc   = (a2 == 0) ? GENERIC_READ
                    : (a2 == 1) ? GENERIC_WRITE
                    :              GENERIC_READ|GENERIC_WRITE;
        HANDLE h    = CreateFileA((char*)(uintptr_t)a1, acc,
                                   FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        *ret        = (h == INVALID_HANDLE_VALUE) ? (uint64_t)-1 : (uint64_t)(uintptr_t)h;
#else
        *ret = (uint64_t)open((char*)(uintptr_t)a1, (int)a2, (int)a3);
#endif
        return 0;
    }

    case DVM_SYS_CLOSE: {
#if defined(DVM_WINDOWS)
        *ret = CloseHandle((HANDLE)(uintptr_t)a1) ? 0 : (uint64_t)-1;
#else
        *ret = (uint64_t)close((int)a1);
#endif
        return 0;
    }

    case DVM_SYS_MMAP: {
        // a1=len, a2=prot (DVM_MMAP_* flags)
        void *p = dvm_mmap((size_t)a1, (int)a2);
        *ret    = (uint64_t)(uintptr_t)p;
        return 0;
    }

    case DVM_SYS_MUNMAP: {
        // a1=ptr, a2=len
        dvm_munmap((void*)(uintptr_t)a1, (size_t)a2);
        *ret = 0;
        return 0;
    }

    case DVM_SYS_GETPID: {
#if defined(DVM_WINDOWS)
        *ret = (uint64_t)GetCurrentProcessId();
#else
        *ret = (uint64_t)getpid();
#endif
        return 0;
    }

    case DVM_SYS_TIME: {
        // Returns seconds since epoch
#if defined(DVM_WINDOWS)
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t t100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        *ret = t100ns / 10000000ULL - 11644473600ULL;  // epoch offset
#else
        *ret = (uint64_t)time(NULL);
#endif
        return 0;
    }

    case DVM_SYS_CLOCK: {
        // Returns nanoseconds (monotonic)
#if defined(DVM_WINDOWS)
        LARGE_INTEGER freq, cnt;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&cnt);
        *ret = (uint64_t)(cnt.QuadPart * 1000000000LL / freq.QuadPart);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        *ret = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
        return 0;
    }

    case DVM_SYS_ISATTY: {
#if defined(DVM_WINDOWS)
        *ret = (uint64_t)_isatty((int)a1);
#else
        *ret = (uint64_t)isatty((int)a1);
#endif
        return 0;
    }

    default:
        fprintf(stderr, "dvm: unknown syscall %llu\n", (unsigned long long)nr);
        return -1;
    }
}
