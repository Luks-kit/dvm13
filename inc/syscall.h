#pragma once
#include "vm.h"
#include <stdint.h>
#include <stddef.h>

// ─── DVM syscall numbers ──────────────────────────────────────────────────────
// Platform-independent numbers. The dispatch layer translates to native.
// Convention: ax=nr, bx=a1, cx=a2, dx=a3, di=a4, si=a5 → result in ax.
typedef enum {
    // ── I/O ──────────────────────────────────────────────────────────────────
    DVM_SYS_EXIT    = 0,
    DVM_SYS_READ    = 1,
    DVM_SYS_WRITE   = 2,
    DVM_SYS_OPEN    = 3,
    DVM_SYS_CLOSE   = 4,

    // ── Memory ───────────────────────────────────────────────────────────────
    DVM_SYS_MMAP    = 5,
    DVM_SYS_MUNMAP  = 6,

    // ── Process / time ───────────────────────────────────────────────────────
    DVM_SYS_GETPID  = 7,
    DVM_SYS_TIME    = 8,
    DVM_SYS_CLOCK   = 9,    // clock_gettime(CLOCK_MONOTONIC) → ns
    DVM_SYS_ISATTY  = 10,

    // ── Threads ──────────────────────────────────────────────────────────────
    // SPAWN  bx=bytecode_ptr  cx=stack_size  → ax=thread_handle (VMThread*)
    //        Spawns a new guest thread starting at bytecode_ptr.
    DVM_SYS_THREAD_SPAWN  = 11,

    // JOIN   bx=thread_handle  → ax=exit_code
    //        Blocks until thread finishes, then frees its resources.
    DVM_SYS_THREAD_JOIN   = 12,

    // DETACH bx=thread_handle  → ax=0
    //        Detaches thread; resources freed automatically on exit.
    DVM_SYS_THREAD_DETACH = 13,

    // SELF   → ax=thread_handle (VMThread*) of calling thread
    //        Returns 0 if called outside a tracked thread (e.g. vm_run wrapper).
    DVM_SYS_THREAD_SELF   = 14,

    // THREAD_ID  → ax=integer id (VMThread.id) of calling thread
    DVM_SYS_THREAD_ID     = 15,

    // ── Mutexes ───────────────────────────────────────────────────────────────
    // All handles are heap-allocated pthread_mutex_t*, returned as uint64_t.

    // CREATE  → ax=mutex_handle   (0 on failure)
    DVM_SYS_MUTEX_CREATE  = 16,

    // LOCK    bx=mutex_handle  → ax=0 on success, -1 on error
    DVM_SYS_MUTEX_LOCK    = 17,

    // UNLOCK  bx=mutex_handle  → ax=0 on success, -1 on error
    DVM_SYS_MUTEX_UNLOCK  = 18,

    // DESTROY bx=mutex_handle  → ax=0
    //         Unlocks if needed, destroys, and frees the handle.
    DVM_SYS_MUTEX_DESTROY = 19,

    // ── Atomics (operate on 64-bit aligned guest memory) ─────────────────────
    // FETCHADD  bx=ptr  cx=delta  → ax=old value
    //           Atomically: old = *ptr; *ptr += delta; return old
    DVM_SYS_ATOMIC_FETCHADD  = 20,

    // CMPXCHG   bx=ptr  cx=expected  dx=desired  → ax=old value
    //           Atomically: if *ptr==expected { *ptr=desired }; return old
    //           Caller checks ax==cx to determine success.
    DVM_SYS_ATOMIC_CMPXCHG   = 21,

    DVM_SYS_COUNT
} DvmSyscall;

// ─── mmap flags (DVM-portable) ────────────────────────────────────────────────
#define DVM_MMAP_NONE    0
#define DVM_MMAP_READ    (1 << 0)
#define DVM_MMAP_WRITE   (1 << 1)
#define DVM_MMAP_EXEC    (1 << 2)
#define DVM_MMAP_ANON    (1 << 3)
#define DVM_MMAP_STACK   (1 << 4)
#define DVM_MMAP_RW      (DVM_MMAP_READ | DVM_MMAP_WRITE)
#define DVM_MMAP_RWX     (DVM_MMAP_READ | DVM_MMAP_WRITE | DVM_MMAP_EXEC)

// ─── mmap wrapper (used internally by vm.c and jit.c) ────────────────────────
void  *dvm_mmap    (size_t len, int prot);
void   dvm_munmap  (void *ptr, size_t len);
void   dvm_mprotect(void *ptr, size_t len, int prot);

// ─── Syscall dispatch ─────────────────────────────────────────────────────────
// Called by OP_SYSCALL handler. Fills result into *ret.
// Returns 0 on success, -1 on unknown syscall.
// The current VMThread* is required for thread-related syscalls (SELF, ID,
// SPAWN). Pass NULL when calling from outside the interpreter (e.g. C tests) —
// SELF and ID will return 0 in that case.
int dvm_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t a4, uint64_t a5, uint64_t *ret);

// Variant used by OP_SYSCALL — passes the calling VMThread for SELF/SPAWN.
// vm.c calls this instead of dvm_syscall directly.
typedef struct VMThread VMThread;
int dvm_syscall_from(VMThread *caller,
                     uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5, uint64_t *ret);
