#include "vm.h"
#include "asm.h"
#include "vm_test.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>

// ─── Helpers ──────────────────────────────────────────────────────────────────
static uint64_t R[8];
static AsmCtx _actx;

#define PASS(name)          printf("PASS  %-40s\n", name)
#define FAIL(name, ...)     do { \
    fprintf(stderr, "FAIL  %s: ", name); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); \
} while(0)

// STORE snippet: writes ax into R[0] so the C side can inspect it.
static char STORE[128];
static void init_store(void) {
    snprintf(STORE, sizeof(STORE),
        "    mov  di, %llu\n"
        "    mov  [di], ax\n",
        (unsigned long long)(uint64_t)&R[0]);
}

static uint64_t asm_run(const char *src) {
    memset(&_actx, 0, sizeof(_actx));
    memset(R, 0, sizeof(R));
    assert(asm_compile(src, &_actx));
    vm_run(_actx.text.buf, 64 * 1024);
    return R[0];
}

// ─── dvm_mmap ─────────────────────────────────────────────────────────────────
static void test_mmap_rw(void) {
    void *p = dvm_mmap(4096, DVM_MMAP_RW);
    assert(p != NULL);
    ((uint8_t*)p)[0] = 0xAB;
    assert(((uint8_t*)p)[0] == 0xAB);
    dvm_munmap(p, 4096);
    PASS("mmap_rw");
}

static void test_mmap_rwx(void) {
    void *p = dvm_mmap(4096, DVM_MMAP_RWX);
    assert(p != NULL);
    ((uint8_t*)p)[0] = 0xC3;  // native RET
    void (*fn)(void) = (void(*)(void))p;
    fn();
    dvm_munmap(p, 4096);
    PASS("mmap_rwx");
}

static void test_mmap_multiple(void) {
    void *a = dvm_mmap(4096, DVM_MMAP_RW);
    void *b = dvm_mmap(4096, DVM_MMAP_RW);
    assert(a != b);
    memset(a, 0xAA, 4096);
    memset(b, 0xBB, 4096);
    assert(((uint8_t*)a)[0] == 0xAA);
    assert(((uint8_t*)b)[0] == 0xBB);
    dvm_munmap(a, 4096);
    dvm_munmap(b, 4096);
    PASS("mmap_multiple");
}

static void test_mprotect_rw_to_rx(void) {
    void *p = dvm_mmap(4096, DVM_MMAP_RW);
    ((uint8_t*)p)[0] = 0xC3;
    dvm_mprotect(p, 4096, DVM_MMAP_READ | DVM_MMAP_EXEC);
    void (*fn)(void) = (void(*)(void))p;
    fn();
    dvm_munmap(p, 4096);
    PASS("mprotect_rw_to_rx");
}

// ─── Syscall dispatch (C-level, not via OP_SYSCALL) ──────────────────────────
static void test_syscall_write(void) {
    const char msg[] = ".";
    uint64_t ret = 0;
    int rc = dvm_syscall(DVM_SYS_WRITE, STDOUT_FILENO,
                         (uint64_t)(uintptr_t)msg, 1, 0, 0, &ret);
    assert(rc == 0 && ret == 1);
    PASS("syscall_write");
}

static void test_syscall_getpid(void) {
    uint64_t ret = 0;
    int rc = dvm_syscall(DVM_SYS_GETPID, 0, 0, 0, 0, 0, &ret);
    assert(rc == 0 && ret == (uint64_t)getpid());
    PASS("syscall_getpid");
}

static void test_syscall_clock(void) {
    uint64_t t1 = 0, t2 = 0;
    dvm_syscall(DVM_SYS_CLOCK, 0, 0, 0, 0, 0, &t1);
    volatile int x = 0;
    for (int i = 0; i < 1000000; i++) x++;
    dvm_syscall(DVM_SYS_CLOCK, 0, 0, 0, 0, 0, &t2);
    assert(t1 > 0);
    assert(t2 > t1);
    PASS("syscall_clock");
}

static void test_syscall_mmap_via_dispatch(void) {
    uint64_t ptr = 0;
    int rc = dvm_syscall(DVM_SYS_MMAP, 4096, DVM_MMAP_RW, 0, 0, 0, &ptr);
    assert(rc == 0 && ptr != 0);
    ((uint8_t*)(uintptr_t)ptr)[0] = 0x42;
    assert(((uint8_t*)(uintptr_t)ptr)[0] == 0x42);
    uint64_t r2 = 0;
    dvm_syscall(DVM_SYS_MUNMAP, ptr, 4096, 0, 0, 0, &r2);
    PASS("syscall_mmap_via_dispatch");
}

static void test_syscall_unknown(void) {
    uint64_t ret = 0;
    silence_stderr();
    int rc = dvm_syscall(DVM_SYS_COUNT + 99, 0, 0, 0, 0, 0, &ret);
    restore_stderr();
    assert(rc == -1);
    PASS("syscall_unknown");
}

// ─── OP_SYSCALL via VM ────────────────────────────────────────────────────────
static void test_op_syscall_write(void) {
    static const char dot[] = ".";
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"
        "    mov  bx, 1\n"
        "    mov  cx, %llu\n"
        "    mov  dx, 1\n"
        "    syscall\n"
        "%s"
        "    halt\n",
        (unsigned long long)DVM_SYS_WRITE,
        (unsigned long long)(uint64_t)dot,
        STORE);
    uint64_t r = asm_run(src);
    assert(r == 1);
    PASS("op_syscall_write");
}

static void test_op_syscall_getpid(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"
        "    syscall\n"
        "%s"
        "    halt\n",
        (unsigned long long)DVM_SYS_GETPID,
        STORE);
    uint64_t r = asm_run(src);
    assert(r == (uint64_t)getpid());
    PASS("op_syscall_getpid");
}

// ─── Edge cases ───────────────────────────────────────────────────────────────
static void test_shadow_depth(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 500\n"
        "    call countdown, die\n"
        "%s"
        "    halt\n"
        "die:\n"
        "    halt\n"
        "countdown:\n"
        "    mov  bx, 0\n"
        "    cmp  ax, bx\n"
        "    je   base\n"
        "    mov  bx, 1\n"
        "    sub  ax, bx\n"
        "    call countdown, die\n"
        "    ret\n"
        "base:\n"
        "    ret\n", STORE);
    uint64_t r = asm_run(src);
    assert(r == 0);
    PASS("shadow_depth_500");
}

static void test_signed_overflow_wraps(void) {
    uint64_t maxval = (uint64_t)INT64_MAX;
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"
        "    mov  bx, 1\n"
        "    add  ax, bx\n"
        "%s"
        "    halt\n",
        (unsigned long long)maxval, STORE);
    uint64_t r = asm_run(src);
    assert((int64_t)r == INT64_MIN);
    PASS("signed_overflow_wraps");
}

static void test_shift_mask(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 0xFF\n"
        "    mov  bx, 64\n"
        "    shl  ax, bx\n"
        "%s"
        "    halt\n", STORE);
    uint64_t r = asm_run(src);
    assert(r == 0xFF);
    PASS("shift_mask_64");
}

// ─── Thread lifecycle (C-level) ───────────────────────────────────────────────

// Simplest possible guest program: just halts.
static uint8_t g_halt_prog[] = { OP_HALT };

static void test_thread_spawn_join(void) {
    VMThread *t = vm_thread_create(g_halt_prog, 64 * 1024);
    assert(t != NULL);
    vm_thread_join(t);
    assert(t->state == VM_THREAD_FINISHED);
    vm_thread_destroy(t);
    PASS("thread_spawn_join");
}

static void test_thread_detach(void) {
    // Detached thread — we can't join it, just make sure it doesn't crash.
    VMThread *t = vm_thread_create(g_halt_prog, 64 * 1024);
    assert(t != NULL);
    vm_thread_detach(t);
    // Give it a moment to finish (detached, so no join possible).
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000000 };
    nanosleep(&ts, NULL);
    PASS("thread_detach");
}

static void test_thread_self_id(void) {
    // Spawn a thread that reads its own ID into a shared slot.
    static volatile uint64_t got_id = 0;

    // Guest program: THREAD_ID syscall → store result → halt
    // We build the bytecode in a Buf via the assembler.
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"    // DVM_SYS_THREAD_ID
        "    syscall\n"
        "    mov  di, %llu\n"    // &got_id
        "    mov  [di], ax\n"
        "    halt\n",
        (unsigned long long)DVM_SYS_THREAD_ID,
        (unsigned long long)(uint64_t)&got_id);

    AsmCtx ac; memset(&ac, 0, sizeof(ac));
    assert(asm_compile(src, &ac));

    VMThread *t = vm_thread_create(ac.text.buf, 64 * 1024);
    vm_thread_join(t);
    assert(got_id != 0 && "thread should have a non-zero id");
    assert(got_id == (uint64_t)t->id);
    vm_thread_destroy(t);
    PASS("thread_self_id");
}

static void test_thread_multiple_join(void) {
    // Spawn N threads, each writing its own index to a shared array, then join all.
    #define N_THREADS 8
    static volatile uint64_t results[N_THREADS];
    memset((void*)results, 0, sizeof(results));

    // Each thread gets its own tiny bytecode that stores a unique value.
    // We encode the value as an immediate into the guest program.
    AsmCtx ctxs[N_THREADS];
    VMThread *threads[N_THREADS];
    char src[512];

    for (int i = 0; i < N_THREADS; i++) {
        memset(&ctxs[i], 0, sizeof(ctxs[i]));
        snprintf(src, sizeof(src),
            "    mov  ax, %llu\n"
            "    mov  di, %llu\n"
            "    mov  [di], ax\n"
            "    halt\n",
            (unsigned long long)(uint64_t)(i + 1),
            (unsigned long long)(uint64_t)&results[i]);
        assert(asm_compile(src, &ctxs[i]));
        threads[i] = vm_thread_create(ctxs[i].text.buf, 64 * 1024);
    }

    for (int i = 0; i < N_THREADS; i++) {
        vm_thread_join(threads[i]);
        assert(results[i] == (uint64_t)(i + 1));
        vm_thread_destroy(threads[i]);
    }
    #undef N_THREADS
    PASS("thread_multiple_join");
}

// ─── Thread syscalls via OP_SYSCALL ───────────────────────────────────────────

static void test_op_thread_spawn_join(void) {
    // Host spawns a child from guest bytecode via the SPAWN syscall,
    // then JOINs it. Child just halts.
    // The spawned child's bytecode is the halt program above.

    static volatile uint64_t child_handle = 0;

    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"   // DVM_SYS_THREAD_SPAWN
        "    mov  bx, %llu\n"   // bytecode ptr = g_halt_prog
        "    mov  cx, %llu\n"   // stack size
        "    syscall\n"
        // ax now holds child VMThread*
        "    mov  di, %llu\n"   // save handle for C to inspect
        "    mov  [di], ax\n"
        "    mov  bx, ax\n"     // a1 = handle
        "    mov  ax, %llu\n"   // DVM_SYS_THREAD_JOIN
        "    syscall\n"
        "    halt\n",
        (unsigned long long)DVM_SYS_THREAD_SPAWN,
        (unsigned long long)(uint64_t)g_halt_prog,
        (unsigned long long)(uint64_t)(64 * 1024),
        (unsigned long long)(uint64_t)&child_handle,
        (unsigned long long)DVM_SYS_THREAD_JOIN);

    AsmCtx ac; memset(&ac, 0, sizeof(ac));
    assert(asm_compile(src, &ac));
    vm_run(ac.text.buf, 64 * 1024);
    assert(child_handle != 0 && "spawn should return a non-null handle");
    PASS("op_thread_spawn_join");
}

// ─── Mutex (C-level) ─────────────────────────────────────────────────────────

static void test_mutex_basic(void) {
    uint64_t handle = 0;
    // create
    int rc = dvm_syscall(DVM_SYS_MUTEX_CREATE, 0, 0, 0, 0, 0, &handle);
    assert(rc == 0 && handle != 0);
    // lock
    uint64_t r = 0;
    rc = dvm_syscall(DVM_SYS_MUTEX_LOCK, handle, 0, 0, 0, 0, &r);
    assert(rc == 0 && r == 0);
    // unlock
    rc = dvm_syscall(DVM_SYS_MUTEX_UNLOCK, handle, 0, 0, 0, 0, &r);
    assert(rc == 0 && r == 0);
    // destroy
    rc = dvm_syscall(DVM_SYS_MUTEX_DESTROY, handle, 0, 0, 0, 0, &r);
    assert(rc == 0);
    PASS("mutex_basic");
}

static void test_mutex_null_handle(void) {
    uint64_t r = 0;
    int rc = dvm_syscall(DVM_SYS_MUTEX_LOCK, 0, 0, 0, 0, 0, &r);
    assert(rc == -1);
    rc = dvm_syscall(DVM_SYS_MUTEX_UNLOCK, 0, 0, 0, 0, 0, &r);
    assert(rc == -1);
    PASS("mutex_null_handle");
}

// Shared-counter test: N threads each increment a counter behind a mutex.
#define MUTEX_THREADS   16
#define MUTEX_ITERS     1000

typedef struct {
    uint64_t mutex_handle;
    volatile uint64_t counter;
    int iters;
} MutexShared;

static void *mutex_worker(void *arg) {
    MutexShared *s = arg;
    uint64_t r = 0;
    for (int i = 0; i < s->iters; i++) {
        dvm_syscall(DVM_SYS_MUTEX_LOCK,   s->mutex_handle, 0, 0, 0, 0, &r);
        s->counter++;
        dvm_syscall(DVM_SYS_MUTEX_UNLOCK, s->mutex_handle, 0, 0, 0, 0, &r);
    }
    return NULL;
}

static void test_mutex_shared_counter(void) {
    MutexShared s = {0};
    uint64_t r = 0;
    dvm_syscall(DVM_SYS_MUTEX_CREATE, 0, 0, 0, 0, 0, &s.mutex_handle);
    s.iters = MUTEX_ITERS;

    pthread_t tids[MUTEX_THREADS];
    for (int i = 0; i < MUTEX_THREADS; i++)
        pthread_create(&tids[i], NULL, mutex_worker, &s);
    for (int i = 0; i < MUTEX_THREADS; i++)
        pthread_join(tids[i], NULL);

    dvm_syscall(DVM_SYS_MUTEX_DESTROY, s.mutex_handle, 0, 0, 0, 0, &r);

    uint64_t expected = (uint64_t)MUTEX_THREADS * MUTEX_ITERS;
    if (s.counter != expected)
        FAIL("mutex_shared_counter", "expected %llu got %llu",
             (unsigned long long)expected, (unsigned long long)s.counter);
    PASS("mutex_shared_counter");
}
#undef MUTEX_THREADS
#undef MUTEX_ITERS

// ─── Atomics (C-level) ────────────────────────────────────────────────────────

static void test_atomic_fetchadd_basic(void) {
    _Atomic uint64_t val = 10;
    uint64_t old = 0;
    int rc = dvm_syscall(DVM_SYS_ATOMIC_FETCHADD,
                         (uint64_t)(uintptr_t)&val, 5, 0, 0, 0, &old);
    assert(rc == 0);
    assert(old == 10 && "fetchadd should return old value");
    assert(atomic_load(&val) == 15 && "value should be incremented");
    PASS("atomic_fetchadd_basic");
}

static void test_atomic_cmpxchg_success(void) {
    _Atomic uint64_t val = 42;
    uint64_t old = 0;
    // expected=42, desired=99 → should succeed
    int rc = dvm_syscall(DVM_SYS_ATOMIC_CMPXCHG,
                         (uint64_t)(uintptr_t)&val, 42, 99, 0, 0, &old);
    assert(rc == 0);
    assert(old == 42 && "cmpxchg should return old value on success");
    assert(atomic_load(&val) == 99 && "value should be swapped");
    PASS("atomic_cmpxchg_success");
}

static void test_atomic_cmpxchg_fail(void) {
    _Atomic uint64_t val = 42;
    uint64_t old = 0;
    // expected=7 (wrong) → should fail, val unchanged
    int rc = dvm_syscall(DVM_SYS_ATOMIC_CMPXCHG,
                         (uint64_t)(uintptr_t)&val, 7, 99, 0, 0, &old);
    assert(rc == 0);
    assert(old == 42 && "cmpxchg failure should return actual old value");
    assert(atomic_load(&val) == 42 && "value should be unchanged on failure");
    PASS("atomic_cmpxchg_fail");
}

// ─── Concurrency: atomic counter stress ──────────────────────────────────────
// N host threads each call ATOMIC_FETCHADD on a shared counter M times.
// Final value must equal N*M with no mutex — proving atomics are sufficient.

#define ATOMIC_THREADS  32
#define ATOMIC_ITERS    10000

typedef struct {
    _Atomic uint64_t *counter;
    int iters;
} AtomicWorkerArg;

static void *atomic_worker(void *arg) {
    AtomicWorkerArg *a = arg;
    uint64_t r = 0;
    for (int i = 0; i < a->iters; i++)
        dvm_syscall(DVM_SYS_ATOMIC_FETCHADD,
                    (uint64_t)(uintptr_t)a->counter, 1, 0, 0, 0, &r);
    return NULL;
}

static void test_atomic_stress(void) {
    static _Atomic uint64_t counter = 0;
    atomic_store(&counter, 0);

    AtomicWorkerArg arg = { .counter = &counter, .iters = ATOMIC_ITERS };
    pthread_t tids[ATOMIC_THREADS];
    for (int i = 0; i < ATOMIC_THREADS; i++)
        pthread_create(&tids[i], NULL, atomic_worker, &arg);
    for (int i = 0; i < ATOMIC_THREADS; i++)
        pthread_join(tids[i], NULL);

    uint64_t got      = atomic_load(&counter);
    uint64_t expected = (uint64_t)ATOMIC_THREADS * ATOMIC_ITERS;
    if (got != expected)
        FAIL("atomic_stress", "expected %llu got %llu",
             (unsigned long long)expected, (unsigned long long)got);
    PASS("atomic_stress");
}
#undef ATOMIC_THREADS
#undef ATOMIC_ITERS

// ─── Concurrency: VM threads sharing memory via mutex ────────────────────────
// Spawn N VM threads; each increments a shared host counter behind a DVM mutex.
// Counter must equal N at the end.

#define VM_CONC_THREADS 16

typedef struct {
    uint64_t        mutex_handle;
    volatile int64_t counter;
} VMConcShared;

static void test_vm_thread_shared_mutex(void) {
    static VMConcShared shared;
    uint64_t r = 0;
    shared.counter = 0;
    dvm_syscall(DVM_SYS_MUTEX_CREATE, 0, 0, 0, 0, 0, &shared.mutex_handle);

    // Each thread: lock mutex, increment counter, unlock, halt.
    char src[768];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"    // MUTEX_LOCK
        "    mov  bx, %llu\n"    // mutex handle
        "    syscall\n"
        // increment counter
        "    mov  di, %llu\n"    // &counter
        "    mov  ax, [di]\n"
        "    mov  bx, 1\n"
        "    add  ax, bx\n"
        "    mov  [di], ax\n"
        "    mov  ax, %llu\n"    // MUTEX_UNLOCK
        "    mov  bx, %llu\n"    // mutex handle
        "    syscall\n"
        "    halt\n",
        (unsigned long long)DVM_SYS_MUTEX_LOCK,
        (unsigned long long)shared.mutex_handle,
        (unsigned long long)(uint64_t)&shared.counter,
        (unsigned long long)DVM_SYS_MUTEX_UNLOCK,
        (unsigned long long)shared.mutex_handle);

    AsmCtx ac; memset(&ac, 0, sizeof(ac));
    assert(asm_compile(src, &ac));

    VMThread *threads[VM_CONC_THREADS];
    for (int i = 0; i < VM_CONC_THREADS; i++)
        threads[i] = vm_thread_create(ac.text.buf, 64 * 1024);
    for (int i = 0; i < VM_CONC_THREADS; i++) {
        vm_thread_join(threads[i]);
        vm_thread_destroy(threads[i]);
    }

    dvm_syscall(DVM_SYS_MUTEX_DESTROY, shared.mutex_handle, 0, 0, 0, 0, &r);

    if (shared.counter != VM_CONC_THREADS)
        FAIL("vm_thread_shared_mutex", "expected %d got %lld",
             VM_CONC_THREADS, (long long)shared.counter);
    PASS("vm_thread_shared_mutex");
}
#undef VM_CONC_THREADS

// ─── Stress: many VM threads, atomic counter, no mutex ───────────────────────
// N VM threads each call ATOMIC_FETCHADD to bump a shared counter.
// Validates that atomic syscalls are correctly serialised across VM threads.

#define VM_ATOMIC_THREADS 32

static void test_vm_thread_atomic_stress(void) {
    static _Atomic uint64_t counter = 0;
    atomic_store(&counter, 0);

    // Guest: ATOMIC_FETCHADD(counter, 1) → halt
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"    // DVM_SYS_ATOMIC_FETCHADD
        "    mov  bx, %llu\n"    // ptr = &counter
        "    mov  cx, 1\n"       // delta = 1
        "    syscall\n"
        "    halt\n",
        (unsigned long long)DVM_SYS_ATOMIC_FETCHADD,
        (unsigned long long)(uint64_t)&counter);

    AsmCtx ac; memset(&ac, 0, sizeof(ac));
    assert(asm_compile(src, &ac));

    VMThread *threads[VM_ATOMIC_THREADS];
    for (int i = 0; i < VM_ATOMIC_THREADS; i++)
        threads[i] = vm_thread_create(ac.text.buf, 64 * 1024);
    for (int i = 0; i < VM_ATOMIC_THREADS; i++) {
        vm_thread_join(threads[i]);
        vm_thread_destroy(threads[i]);
    }

    uint64_t got = atomic_load(&counter);
    if (got != VM_ATOMIC_THREADS)
        FAIL("vm_thread_atomic_stress", "expected %d got %llu",
             VM_ATOMIC_THREADS, (unsigned long long)got);
    PASS("vm_thread_atomic_stress");
}
#undef VM_ATOMIC_THREADS

// ─── Race detection: unsynchronised access should be detectable ──────────────
// This test intentionally omits synchronisation and documents the expected
// outcome: the count may be wrong. It is compiled in but the assertion is
// inverted — we assert that the race is *possible* (i.e. the result could
// differ from expected), not that it definitely occurs. Run under TSan to
// surface it reliably.
//
// Under TSan: compile with -fsanitize=thread and this will be flagged.
// Without TSan: we skip the counter-value assertion and just note the intent.

#define RACE_THREADS 8
#define RACE_ITERS   5000

static void test_racy_counter_documents_race(void) {
    // Shared counter with no synchronisation.
    static volatile uint64_t racy_counter = 0;
    racy_counter = 0;

    char src[512];
    snprintf(src, sizeof(src),
        "    mov  di, %llu\n"      // &racy_counter
        "    mov  cx, %llu\n"      // iteration count
        "loop:\n"
        "    mov  ax, [di]\n"      // load (non-atomic)
        "    mov  bx, 1\n"
        "    add  ax, bx\n"
        "    mov  [di], ax\n"      // store (non-atomic)
        "    sub  cx, bx\n"
        "    mov  bx, 0\n"
        "    cmp  cx, bx\n"
        "    jne  loop\n"
        "    halt\n",
        (unsigned long long)(uint64_t)&racy_counter,
        (unsigned long long)(uint64_t)RACE_ITERS);

    AsmCtx ac; memset(&ac, 0, sizeof(ac));
    assert(asm_compile(src, &ac));

    VMThread *threads[RACE_THREADS];
    for (int i = 0; i < RACE_THREADS; i++)
        threads[i] = vm_thread_create(ac.text.buf, 64 * 1024);
    for (int i = 0; i < RACE_THREADS; i++) {
        vm_thread_join(threads[i]);
        vm_thread_destroy(threads[i]);
    }

    uint64_t expected = (uint64_t)RACE_THREADS * RACE_ITERS;
    // Do not assert equality — the point of this test is the race, not the result.
    // Run with -fsanitize=thread for definitive detection.
    printf("NOTE  racy_counter=%llu expected=%llu (race; run with TSan to flag)\n",
           (unsigned long long)racy_counter, (unsigned long long)expected);
    PASS("racy_counter_documents_race");
}
#undef RACE_THREADS
#undef RACE_ITERS

// ─── main ─────────────────────────────────────────────────────────────────────
int main(void) {
    init_store();

    printf("─── dvm_mmap ───────────────────────────────────────\n");
    test_mmap_rw();
    test_mmap_rwx();
    test_mmap_multiple();
    test_mprotect_rw_to_rx();

    printf("─── syscall dispatch ───────────────────────────────\n");
    test_syscall_write();
    test_syscall_getpid();
    test_syscall_clock();
    test_syscall_mmap_via_dispatch();
    test_syscall_unknown();

    printf("─── OP_SYSCALL via VM ──────────────────────────────\n");
    test_op_syscall_write();
    test_op_syscall_getpid();

    printf("─── edge cases ─────────────────────────────────────\n");
    test_shadow_depth();
    test_signed_overflow_wraps();
    test_shift_mask();

    printf("─── thread lifecycle ────────────────────────────────\n");
    test_thread_spawn_join();
    test_thread_detach();
    test_thread_self_id();
    test_thread_multiple_join();
    test_op_thread_spawn_join();

    printf("─── mutex ───────────────────────────────────────────\n");
    test_mutex_basic();
    test_mutex_null_handle();
    test_mutex_shared_counter();

    printf("─── atomics ─────────────────────────────────────────\n");
    test_atomic_fetchadd_basic();
    test_atomic_cmpxchg_success();
    test_atomic_cmpxchg_fail();
    test_atomic_stress();

    printf("─── VM thread concurrency ───────────────────────────\n");
    test_vm_thread_shared_mutex();
    test_vm_thread_atomic_stress();
    test_racy_counter_documents_race();

    printf("────────────────────────────────────────────────────\n");
    printf("All tests passed.\n");
    return 0;
}
