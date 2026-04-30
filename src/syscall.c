#include "syscall.h"
#include "vm.h"       // VMThread, vm_thread_create/join/detach/destroy
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

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
    if (prot & DVM_MMAP_READ) return PAGE_READONLY;
    return PAGE_NOACCESS;
}
#endif

void *dvm_mmap(size_t len, int prot) {
#if defined(DVM_WINDOWS)
    return VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE, prot_to_win(prot));
#else
    void *p = mmap(NULL, len, prot_to_native(prot), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

// ─── Thread-local current VMThread ───────────────────────────────────────────
// Set by vm_run_thread so SELF/ID syscalls can find the caller without an
// explicit parameter threaded all the way down to dvm_syscall.
// vm.c sets this via dvm_set_current_thread before entering the dispatch loop.

static _Thread_local VMThread *_current_thread = NULL;

void dvm_set_current_thread(VMThread *t) {
    _current_thread = t;
}

VMThread *dvm_get_current_thread(void) {
    return _current_thread;
}

// ─── Syscall dispatch (internal) ─────────────────────────────────────────────
static int dispatch(VMThread *caller,
                    uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5, uint64_t *ret) {
    (void)a4; (void)a5;
    *ret = (uint64_t)-1;

    switch ((DvmSyscall)nr) {

    // ── I/O ──────────────────────────────────────────────────────────────────
    case DVM_SYS_EXIT:
        exit((int)a1);

    case DVM_SYS_READ: {
#if defined(DVM_WINDOWS)
        HANDLE h = (HANDLE)(uintptr_t)a1;
        DWORD got = 0;
        BOOL ok = ReadFile(h, (void*)(uintptr_t)a2, (DWORD)a3, &got, NULL);
        *ret = ok ? got : (uint64_t)-1;
#else
        *ret = (uint64_t)read((int)a1, (void*)(uintptr_t)a2, (size_t)a3);
#endif
        return 0;
    }

    case DVM_SYS_WRITE: {
#if defined(DVM_WINDOWS)
        HANDLE h = (HANDLE)(uintptr_t)a1;
        DWORD wrote = 0;
        BOOL ok = WriteFile(h, (void*)(uintptr_t)a2, (DWORD)a3, &wrote, NULL);
        *ret = ok ? wrote : (uint64_t)-1;
#else
        *ret = (uint64_t)write((int)a1, (void*)(uintptr_t)a2, (size_t)a3);
#endif
        return 0;
    }

    case DVM_SYS_OPEN: {
#if defined(DVM_WINDOWS)
        DWORD acc = (a2 == 0) ? GENERIC_READ
                  : (a2 == 1) ? GENERIC_WRITE
                  :              GENERIC_READ | GENERIC_WRITE;
        HANDLE h = CreateFileA((char*)(uintptr_t)a1, acc, FILE_SHARE_READ,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        *ret = (h == INVALID_HANDLE_VALUE) ? (uint64_t)-1 : (uint64_t)(uintptr_t)h;
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

    // ── Memory ───────────────────────────────────────────────────────────────
    case DVM_SYS_MMAP: {
        void *p = dvm_mmap((size_t)a1, (int)a2);
        *ret = (uint64_t)(uintptr_t)p;
        return 0;
    }

    case DVM_SYS_MUNMAP: {
        dvm_munmap((void*)(uintptr_t)a1, (size_t)a2);
        *ret = 0;
        return 0;
    }

    // ── Process / time ───────────────────────────────────────────────────────
    case DVM_SYS_GETPID: {
#if defined(DVM_WINDOWS)
        *ret = (uint64_t)GetCurrentProcessId();
#else
        *ret = (uint64_t)getpid();
#endif
        return 0;
    }

    case DVM_SYS_TIME: {
#if defined(DVM_WINDOWS)
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t t100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        *ret = t100ns / 10000000ULL - 11644473600ULL;
#else
        *ret = (uint64_t)time(NULL);
#endif
        return 0;
    }

    case DVM_SYS_CLOCK: {
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

    // ── Threads ──────────────────────────────────────────────────────────────

    case DVM_SYS_THREAD_SPAWN: {
        // a1 = bytecode ptr (absolute), a2 = stack size
        uint8_t *entry     = (uint8_t *)(uintptr_t)a1;
        size_t   stack_sz  = a2 ? (size_t)a2 : 64 * 1024;
        VMThread *child    = vm_thread_create(entry, stack_sz);
        *ret = (uint64_t)(uintptr_t)child;
        return 0;
    }

    case DVM_SYS_THREAD_JOIN: {
        // a1 = thread handle (VMThread*)
        VMThread *t = (VMThread *)(uintptr_t)a1;
        if (!t) { *ret = (uint64_t)-1; return -1; }
        vm_thread_join(t);
        *ret = (uint64_t)t->exit_code;
        vm_thread_destroy(t);
        return 0;
    }

    case DVM_SYS_THREAD_DETACH: {
        VMThread *t = (VMThread *)(uintptr_t)a1;
        if (!t) { *ret = (uint64_t)-1; return -1; }
        vm_thread_detach(t);
        *ret = 0;
        return 0;
    }

    case DVM_SYS_THREAD_SELF: {
        // Returns the VMThread* of the calling thread, or 0.
        *ret = (uint64_t)(uintptr_t)(caller ? caller : _current_thread);
        return 0;
    }

    case DVM_SYS_THREAD_ID: {
        VMThread *self = caller ? caller : _current_thread;
        *ret = self ? (uint64_t)self->id : 0;
        return 0;
    }

    // ── Mutexes ───────────────────────────────────────────────────────────────
    // Handles are heap-allocated pthread_mutex_t* cast to uint64_t.
    // Guest code treats them as opaque integers.

    case DVM_SYS_MUTEX_CREATE: {
#if defined(DVM_WINDOWS)
        CRITICAL_SECTION *cs = malloc(sizeof(CRITICAL_SECTION));
        if (!cs) { *ret = 0; return 0; }
        InitializeCriticalSection(cs);
        *ret = (uint64_t)(uintptr_t)cs;
#else
        pthread_mutex_t *m = malloc(sizeof(pthread_mutex_t));
        if (!m) { *ret = 0; return 0; }
        pthread_mutex_init(m, NULL);
        *ret = (uint64_t)(uintptr_t)m;
#endif
        return 0;
    }

    case DVM_SYS_MUTEX_LOCK: {
#if defined(DVM_WINDOWS)
        CRITICAL_SECTION *cs = (CRITICAL_SECTION *)(uintptr_t)a1;
        if (!cs) { *ret = (uint64_t)-1; return -1; }
        EnterCriticalSection(cs);
        *ret = 0;
#else
        pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)a1;
        if (!m) { *ret = (uint64_t)-1; return -1; }
        *ret = (uint64_t)pthread_mutex_lock(m);
#endif
        return 0;
    }

    case DVM_SYS_MUTEX_UNLOCK: {
#if defined(DVM_WINDOWS)
        CRITICAL_SECTION *cs = (CRITICAL_SECTION *)(uintptr_t)a1;
        if (!cs) { *ret = (uint64_t)-1; return -1; }
        LeaveCriticalSection(cs);
        *ret = 0;
#else
        pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)a1;
        if (!m) { *ret = (uint64_t)-1; return -1; }
        *ret = (uint64_t)pthread_mutex_unlock(m);
#endif
        return 0;
    }

    case DVM_SYS_MUTEX_DESTROY: {
#if defined(DVM_WINDOWS)
        CRITICAL_SECTION *cs = (CRITICAL_SECTION *)(uintptr_t)a1;
        if (!cs) { *ret = 0; return 0; }
        DeleteCriticalSection(cs);
        free(cs);
#else
        pthread_mutex_t *m = (pthread_mutex_t *)(uintptr_t)a1;
        if (!m) { *ret = 0; return 0; }
        pthread_mutex_destroy(m);
        free(m);
#endif
        *ret = 0;
        return 0;
    }

    // ── Atomics ───────────────────────────────────────────────────────────────
    // Both operate on 64-bit aligned guest memory via C11 _Atomic casts.
    // The guest is responsible for alignment; misaligned access is UB.

    case DVM_SYS_ATOMIC_FETCHADD: {
        // a1=ptr, a2=delta → old value
        _Atomic uint64_t *p = (_Atomic uint64_t *)(uintptr_t)a1;
        *ret = atomic_fetch_add_explicit(p, a2, memory_order_seq_cst);
        return 0;
    }

    case DVM_SYS_ATOMIC_CMPXCHG: {
        // a1=ptr, a2=expected, a3=desired → old value
        // Guest checks if ret==a2 to know whether the swap succeeded.
        _Atomic uint64_t *p = (_Atomic uint64_t *)(uintptr_t)a1;
        uint64_t expected = a2;
        atomic_compare_exchange_strong_explicit(
            p, &expected, a3,
            memory_order_seq_cst, memory_order_seq_cst);
        *ret = expected;   // atomic_compare_exchange writes old value into expected
        return 0;
    }

    default:
        fprintf(stderr, "dvm: unknown syscall %llu\n", (unsigned long long)nr);
        return -1;
    }
}

// ─── Public entry points ──────────────────────────────────────────────────────

// Called from C tests — no caller context.
int dvm_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t *ret) {
    return dispatch(NULL, nr, a1, a2, a3, a4, a5, ret);
}

// Called from OP_SYSCALL in vm.c — passes the current VMThread.
int dvm_syscall_from(VMThread *caller,
                     uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t *ret) {
    return dispatch(caller, nr, a1, a2, a3, a4, a5, ret);
}
