#include "vm.h"
#include "asm.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

static uint64_t R[8];
static AsmCtx _actx;

#define PASS(name) printf("PASS  %-32s\n", name)
#define FAIL(name, ...) do { \
    fprintf(stderr, "FAIL  %s: ", name); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); \
} while(0)

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
    vm_run(_actx.text.buf, 64*1024);
    return R[0];
}

// ─── dvm_mmap / dvm_munmap / dvm_mprotect ────────────────────────────────────

static void test_mmap_rw(void) {
    void *p = dvm_mmap(4096, DVM_MMAP_RW);
    assert(p != NULL);
    // write and read back
    ((uint8_t*)p)[0] = 0xAB;
    assert(((uint8_t*)p)[0] == 0xAB);
    dvm_munmap(p, 4096);
    PASS("mmap_rw");
}

static void test_mmap_rwx(void) {
    // Allocate RWX, write a ret instruction, execute it
    void *p = dvm_mmap(4096, DVM_MMAP_RWX);
    assert(p != NULL);
    ((uint8_t*)p)[0] = 0xC3;  // RET
    void (*fn)(void) = (void(*)(void))p;
    fn();  // should return immediately without crashing
    dvm_munmap(p, 4096);
    PASS("mmap_rwx");
}

static void test_mmap_multiple(void) {
    // multiple independent allocations don't overlap
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
    ((uint8_t*)p)[0] = 0xC3;  // RET
    dvm_mprotect(p, 4096, DVM_MMAP_READ | DVM_MMAP_EXEC);
    void (*fn)(void) = (void(*)(void))p;
    fn();
    dvm_munmap(p, 4096);
    PASS("mprotect_rw_to_rx");
}

// ─── Syscall dispatch (C-level, not via OP_SYSCALL) ──────────────────────────

static void test_syscall_write(void) {
    // DVM_SYS_WRITE to stdout: ".\n"
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
    // spin briefly
    volatile int x = 0;
    for (int i = 0; i < 1000000; i++) x++;
    dvm_syscall(DVM_SYS_CLOCK, 0, 0, 0, 0, 0, &t2);
    assert(t1 > 0 && "clock should be non-zero");
    assert(t2 > t1 && "clock should be monotonic");
    PASS("syscall_clock");
}

static void test_syscall_mmap_via_dispatch(void) {
    // DVM_SYS_MMAP via dvm_syscall
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
    // unknown syscall number should return -1
    uint64_t ret = 0;
    silence_stderr();
    int rc = dvm_syscall(DVM_SYS_COUNT + 99, 0, 0, 0, 0, 0, &ret);
    restore_stderr();
    assert(rc == -1 && "unknown syscall should fail");
    PASS("syscall_unknown");
}

// ─── OP_SYSCALL via VM ────────────────────────────────────────────────────────

static void test_op_syscall_write(void) {
    // write "." to stdout via OP_SYSCALL
    static const char dot[] = ".";
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, %llu\n"   // DVM_SYS_WRITE
        "    mov  bx, 1\n"       // fd = stdout
        "    mov  cx, %llu\n"   // buf ptr
        "    mov  dx, 1\n"       // len
        "    syscall\n"
        "%s"
        "    halt\n",
        (unsigned long long)DVM_SYS_WRITE,
        (unsigned long long)(uint64_t)dot,
        STORE); 
    uint64_t r = asm_run(src);
    assert(r == 1 && "write should return 1 byte written");
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

// ─── Shadow stack depth (deep recursion) ─────────────────────────────────────

static void test_shadow_depth(void) {
    // Recurse 500 levels deep — well within SHADOW_DEPTH=2048 (1024 frames)
    // count(n): if n==0 ret; else call count(n-1)
    // final ax = 0
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
    assert(r == 0 && "deep recursion should bottom out at 0");
    PASS("shadow_depth_500");
}

// ─── Edge: signed overflow wraps silently ────────────────────────────────────

static void test_signed_overflow_wraps(void) {
    // INT64_MAX + 1 should wrap to INT64_MIN (C undefined but two's complement)
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

// ─── Edge: shift masks to 6 bits ─────────────────────────────────────────────

static void test_shift_mask(void) {
    // shift by 64 → masked to 0 → result = original value
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 0xFF\n"
        "    mov  bx, 64\n"
        "    shl  ax, bx\n"
        "%s"
        "    halt\n", STORE);
    uint64_t r = asm_run(src);
    // x86 SHL masks to 6 bits: 64 & 63 = 0 → no shift
    assert(r == 0xFF && "shift by 64 masked to 0 = no shift");
    PASS("shift_mask_64");
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(void) {
    init_store();

    printf("─── dvm_mmap ───────────────────────────────\n");
    test_mmap_rw();
    test_mmap_rwx();
    test_mmap_multiple();
    test_mprotect_rw_to_rx();

    printf("─── syscall dispatch ───────────────────────\n");
    test_syscall_write();
    test_syscall_getpid();
    test_syscall_clock();
    test_syscall_mmap_via_dispatch();
    test_syscall_unknown();

    printf("─── OP_SYSCALL via VM ──────────────────────\n");
    test_op_syscall_write();
    test_op_syscall_getpid();

    printf("─── edge cases ─────────────────────────────\n");
    test_shadow_depth();
    test_signed_overflow_wraps();
    test_shift_mask();

    printf("────────────────────────────────────────────\n");
    printf("All syscall tests passed.\n");
    return 0;
}
