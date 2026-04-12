#pragma once
#include <stdint.h>
#include <stddef.h>

// ─── DVM syscall numbers ──────────────────────────────────────────────────────
// Platform-independent numbers. The dispatch layer translates to native.
// Convention: ax=nr, bx=a1, cx=a2, dx=a3, di=a4, si=a5 → result in ax.

typedef enum {
    DVM_SYS_EXIT    = 0,
    DVM_SYS_READ    = 1,
    DVM_SYS_WRITE   = 2,
    DVM_SYS_OPEN    = 3,
    DVM_SYS_CLOSE   = 4,
    DVM_SYS_MMAP    = 5,
    DVM_SYS_MUNMAP  = 6,
    DVM_SYS_GETPID  = 7,
    DVM_SYS_TIME    = 8,
    DVM_SYS_CLOCK   = 9,    // clock_gettime(CLOCK_MONOTONIC, &ts) → ts.tv_nsec
    DVM_SYS_ISATTY  = 10,

    DVM_SYS_COUNT
} DvmSyscall;

// ─── mmap flags (DVM-portable) ────────────────────────────────────────────────
// Passed as a5 (si) to DVM_SYS_MMAP.
#define DVM_MMAP_NONE    0
#define DVM_MMAP_READ    (1 << 0)
#define DVM_MMAP_WRITE   (1 << 1)
#define DVM_MMAP_EXEC    (1 << 2)
#define DVM_MMAP_ANON    (1 << 3)
#define DVM_MMAP_STACK   (1 << 4)
#define DVM_MMAP_RW      (DVM_MMAP_READ | DVM_MMAP_WRITE)
#define DVM_MMAP_RWX     (DVM_MMAP_READ | DVM_MMAP_WRITE | DVM_MMAP_EXEC)

// ─── mmap wrapper (used internally by vm.c and jit.c) ────────────────────────
// Abstracts mmap/VirtualAlloc. prot = DVM_MMAP_* flags.
void  *dvm_mmap  (size_t len, int prot);
void   dvm_munmap(void *ptr, size_t len);
void   dvm_mprotect(void *ptr, size_t len, int prot);  // change prot after alloc

// ─── Syscall dispatch ─────────────────────────────────────────────────────────
// Called by OP_SYSCALL handler. Fills result into *ret.
// Returns 0 on success, -1 on unknown syscall.
int dvm_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t *ret);
