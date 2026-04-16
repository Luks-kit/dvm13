#include "vm.h"
#include "asm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

static uint64_t R[8];
static AsmCtx _actx;

#define PASS(name) printf("PASS  %-32s\n", name)
#define FAIL(name, ...) do { \
    fprintf(stderr, "FAIL  %s: ", name); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); \
} while(0)
#define CHECK(name, got, want) do { \
    if ((uint64_t)(got) != (uint64_t)(want)) \
        FAIL(name, "got 0x%llX want 0x%llX", \
             (unsigned long long)(got), (unsigned long long)(want)); \
    PASS(name); \
} while(0)

// Run assembled source, return result in R[0]
static uint64_t asm_run(const char *src) {
    memset(&_actx, 0, sizeof(_actx));
    memset(R, 0, sizeof(R));
    int ok = asm_compile(src, &_actx);
    assert(ok && "asm_compile failed unexpectedly");
    vm_run(_actx.text.buf, 64*1024);
    return R[0];
}

// Store snippet: moves ax into R[0]
static char STORE[128];
static void init_store(void) {
    snprintf(STORE, sizeof(STORE),
        "    mov  di, %llu\n"
        "    mov  [di], ax\n",
        (unsigned long long)(uint64_t)&R[0]);
}

// ─── Forward label resolution ─────────────────────────────────────────────────

static void test_forward_jmp(void) {
    // JMP forward over a bad instruction to a good one
    char src[512];
    snprintf(src, sizeof(src),
        "    jmp  skip\n"
        "    mov  ax, 0xBAD\n"   // never executed
        "skip:\n"
        "    mov  ax, 0x600D\n"
        "%s"
        "    halt\n", STORE);
    CHECK("forward_jmp", asm_run(src), 0x600DULL);
}

static void test_forward_je(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 5\n"
        "    mov  bx, 5\n"
        "    cmp  ax, bx\n"
        "    je   equal\n"
        "    mov  ax, 0xBAD\n"
        "    halt\n"
        "equal:\n"
        "    mov  ax, 0xE0\n"
        "%s"
        "    halt\n", STORE);
    CHECK("forward_je", asm_run(src), 0xE0ULL);
}

static void test_forward_call(void) {
    // call with forward-declared ok and err labels
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 10\n"
        "    call dbl, on_err\n"
        "%s"
        "    halt\n"
        "on_err:\n"
        "    mov  ax, 0xBAD\n"
        "    halt\n"
        "dbl:\n"
        "    mov  bx, ax\n"
        "    add  ax, bx\n"
        "    ret\n", STORE);
    CHECK("forward_call", asm_run(src), 20ULL);
}

static void test_forward_and_backward(void) {
    // loop uses backward jl but exit uses forward jmp — mix
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 0\n"
        "    mov  bx, 1\n"
        "    mov  cx, 6\n"
        "top:\n"
        "    add  ax, bx\n"
        "    mov  dx, 1\n"
        "    add  bx, dx\n"
        "    cmp  bx, cx\n"
        "    jl   top\n"
        "    jmp  done\n"
        "    mov  ax, 0xBAD\n"  // dead code
        "done:\n"
        "%s"
        "    halt\n", STORE);
    // sum 1..5 = 15
    CHECK("forward_and_backward", asm_run(src), 15ULL);
}

// ─── enter / leave via assembler ─────────────────────────────────────────────

static void test_asm_enter_leave(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    call myfunc, die\n"
        "%s"
        "    halt\n"
        "die:\n"
        "    halt\n"
        "myfunc:\n"
        "    enter 0\n"         // no locals needed, just test frame alloc/free
        "    mov  ax, 0xF4\n"
        "    leave\n"
        "    ret\n", STORE);
    CHECK("asm_enter_leave", asm_run(src), 0xF4ULL);
}

static void test_asm_enter_locals(void) {
    // ENTER 16 → write two locals via push, read via pop
    char src[512];
    snprintf(src, sizeof(src),
        "    call f, die\n"
        "%s"
        "    halt\n"
        "die:\n"
        "    halt\n"
        "f:\n"
        "    enter 16\n"
        "    mov  ax, 0xAA\n"
        "    push ax\n"
        "    mov  ax, 0xBB\n"
        "    push ax\n"
        "    pop  ax\n"         // ax = 0xBB
        "    pop  bx\n"         // bx = 0xAA
        "    add  ax, bx\n"     // ax = 0x165
        "    leave\n"
        "    ret\n", STORE);
    CHECK("asm_enter_locals", asm_run(src), 0x165ULL);
}

// ─── All jump flavors via assembler ──────────────────────────────────────────

static void test_asm_jne(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 1\n"
        "    mov  bx, 2\n"
        "    cmp  ax, bx\n"
        "    jne  ok\n"
        "    mov  ax, 0xBAD\n"
        "    halt\n"
        "ok:\n"
        "    mov  ax, 0x0E\n"
        "%s"
        "    halt\n", STORE);
    CHECK("asm_jne", asm_run(src), 0x0EULL);
}

static void test_asm_jge(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 5\n"
        "    mov  bx, 3\n"
        "    cmp  ax, bx\n"
        "    jge  ok\n"
        "    mov  ax, 0xBAD\n"
        "    halt\n"
        "ok:\n"
        "    mov  ax, 0x0E\n"
        "%s"
        "    halt\n", STORE);
    CHECK("asm_jge", asm_run(src), 0x0EULL);
}

static void test_asm_jle(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    mov  ax, 3\n"
        "    mov  bx, 5\n"
        "    cmp  ax, bx\n"
        "    jle  ok\n"
        "    mov  ax, 0xBAD\n"
        "    halt\n"
        "ok:\n"
        "    mov  ax, 0x0E\n"
        "%s"
        "    halt\n", STORE);
    CHECK("asm_jle", asm_run(src), 0x0EULL);
}

// ─── Error path tests (expected failures) ────────────────────────────────────

// Redirect stderr to /dev/null and verify asm_compile returns 0
static int asm_should_fail(const char *src) {
    silence_stderr();
    
    AsmCtx _tmp; memset(&_tmp, 0, sizeof(_tmp));
    int ok = asm_compile(src, &_tmp);
    
    restore_stderr();
    return !ok;
}

static void test_undefined_label(void) {
    assert(asm_should_fail("    jmp  ghost\n    halt\n"));
    PASS("undefined_label");
}

static void test_bad_mnemonic(void) {
    assert(asm_should_fail("    florbulate ax, bx\n    halt\n"));
    PASS("bad_mnemonic");
}

static void test_undefined_call_target(void) {
    assert(asm_should_fail("    call nowhere, also_nowhere\n    halt\n"));
    PASS("undefined_call_target");
}

// ─── Label at end of file (no trailing instruction) ──────────────────────────

static void test_label_at_eof(void) {
    // Label defined but no instruction after it — should assemble fine,
    // offset just points past the last byte.
    char src[256];
    snprintf(src, sizeof(src),
        "    mov  ax, 0x42\n"
        "%s"
        "    halt\n"
        "end:\n", STORE);
    CHECK("label_at_eof", asm_run(src), 0x42ULL);
}

// ─── Comment and whitespace robustness ───────────────────────────────────────

static void test_comments(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "; full line comment\n"
        "    mov  ax, 10 ; inline comment\n"
        "    mov  bx, 32\n"
        "    add  ax, bx ; ax = 42\n"
        "%s"
        "    halt\n", STORE);
    CHECK("comments", asm_run(src), 42ULL);
}

static void test_blank_lines(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "\n"
        "\n"
        "    mov  ax, 99\n"
        "\n"
        "%s"
        "\n"
        "    halt\n", STORE);
    CHECK("blank_lines", asm_run(src), 99ULL);
}

// ─── Label + instruction on same line ────────────────────────────────────────

static void test_label_same_line(void) {
    char src[512];
    snprintf(src, sizeof(src),
        "    jmp  entry\n"
        "    mov  ax, 0xBAD\n"
        "entry: mov  ax, 0x1D\n"
        "%s"
        "    halt\n", STORE);
    CHECK("label_same_line", asm_run(src), 0x1DULL);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(void) {
    init_store();

    printf("─── forward labels ─────────────────────────\n");
    test_forward_jmp();
    test_forward_je();
    test_forward_call();
    test_forward_and_backward();

    printf("─── enter / leave ──────────────────────────\n");
    test_asm_enter_leave();
    test_asm_enter_locals();

    printf("─── jump flavors ───────────────────────────\n");
    test_asm_jne();
    test_asm_jge();
    test_asm_jle();

    printf("─── error paths ────────────────────────────\n");
    test_undefined_label();
    test_bad_mnemonic();
    test_undefined_call_target();

    printf("─── edge cases ─────────────────────────────\n");
    test_label_at_eof();
    test_comments();
    test_blank_lines();
    test_label_same_line();

    printf("────────────────────────────────────────────\n");
    printf("All asm tests passed.\n");
    return 0;
}
