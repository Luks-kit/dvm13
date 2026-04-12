#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

// ─── Bytecode builder ─────────────────────────────────────────────────────────

typedef struct { uint8_t buf[4096]; size_t len; } Prog;

static void pu8 (Prog *p, uint8_t  v) { p->buf[p->len++] = v; }
static void pu16(Prog *p, uint16_t v) { memcpy(p->buf+p->len,&v,2); p->len+=2; }
static void pi32(Prog *p, int32_t  v) { memcpy(p->buf+p->len,&v,4); p->len+=4; }
static void pu64(Prog *p, uint64_t v) { memcpy(p->buf+p->len,&v,8); p->len+=8; }

#define RB(d,s) ((uint8_t)(((d)<<4)|(s)))

// Result sink — tests write here before HALT
static uint64_t R[8];
static double   FPR[8];

static void store_r(Prog *p, uint8_t src, int slot) {
    pu8(p, OP_MOV_RI); pu8(p, RB(REG_DI,0)); pu64(p,(uint64_t)&R[slot]);
    pu8(p, OP_MOV_MR); pu8(p, RB(REG_DI,src));
}
// store_r_safe: saves/restores scratch so it does not clobber src's value.
// Uses EX as scratch (saving it on data stack), falls back to DI if src==EX.
static void store_r_safe(Prog *p, uint8_t src, int slot) {
    uint8_t scratch = (src == REG_EX) ? REG_DI : REG_EX;
    pu8(p, OP_PUSH);   pu8(p, RB(scratch, 0));               // save scratch
    pu8(p, OP_MOV_RI); pu8(p, RB(scratch, 0)); pu64(p,(uint64_t)&R[slot]);
    pu8(p, OP_MOV_MR); pu8(p, RB(scratch, src));
    pu8(p, OP_POP);    pu8(p, RB(scratch, 0));               // restore scratch
}
static void store_f(Prog *p, uint8_t fp_src, int slot) {
    pu8(p, OP_MOV_RI); pu8(p, RB(REG_DI,0)); pu64(p,(uint64_t)&FPR[slot]);
    pu8(p, OP_FMOV_MR); pu8(p, RB(REG_DI,fp_src));
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void run(Prog *p) {
    memset(R,  0, sizeof(R));
    memset(FPR, 0, sizeof(FPR));
    vm_run(p->buf, 64*1024);
}

#define PASS(name) printf("PASS  %-32s\n", name)
#define FAIL(name, ...) do { \
    fprintf(stderr, "FAIL  %s: ", name); fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    exit(1); \
} while(0)

#define CHECK(name, got, want) do { \
    if ((got) != (want)) FAIL(name, "got %llu want %llu", \
        (unsigned long long)(got), (unsigned long long)(want)); \
    PASS(name); \
} while(0)

#define CHECK_I(name, got, want) do { \
    if ((int64_t)(got) != (int64_t)(want)) FAIL(name, "got %lld want %lld", \
        (long long)(got), (long long)(want)); \
    PASS(name); \
} while(0)

#define CHECK_F(name, got, want) do { \
    if ((got) != (want)) FAIL(name, "got %f want %f", (double)(got), (double)(want)); \
    PASS(name); \
} while(0)

// ─── Arithmetic ───────────────────────────────────────────────────────────────

static void test_sub(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,100);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,58);
    pu8(&p,OP_SUB);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("sub", R[0], 42ULL);
}

static void test_mul(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,6);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,7);
    pu8(&p,OP_MUL);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("mul", R[0], 42ULL);
}

static void test_imul_negative(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,(uint64_t)-6LL);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,7);
    pu8(&p,OP_IMUL);   pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK_I("imul_negative", R[0], -42LL);
}

static void test_div(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,84);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,2);
    pu8(&p,OP_DIV);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("div", R[0], 42ULL);
}

static void test_idiv_negative(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,(uint64_t)-84LL);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,2);
    pu8(&p,OP_IDIV);   pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK_I("idiv_negative", R[0], -42LL);
}

static void test_mod(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,17);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,5);
    pu8(&p,OP_MOD);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("mod", R[0], 2ULL);
}

static void test_neg(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,42);
    pu8(&p,OP_NEG);    pu8(&p,RB(REG_AX,0));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK_I("neg", R[0], -42LL);
}

// ─── Bitwise ──────────────────────────────────────────────────────────────────

static void test_and(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xFF0F);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,0x0FFF);
    pu8(&p,OP_AND);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("and", R[0], 0x0F0FULL);
}

static void test_or(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xF0);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,0x0F);
    pu8(&p,OP_OR);     pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("or", R[0], 0xFFULL);
}

static void test_xor(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xDEAD);
    pu8(&p,OP_XOR);    pu8(&p,RB(REG_AX,REG_AX));  // x ^ x = 0
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("xor_self", R[0], 0ULL);
}

static void test_not(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0ULL);
    pu8(&p,OP_NOT);    pu8(&p,RB(REG_AX,0));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("not_zero", R[0], UINT64_MAX);
}

static void test_not_allones(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,UINT64_MAX);
    pu8(&p,OP_NOT);    pu8(&p,RB(REG_AX,0));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("not_allones", R[0], 0ULL);
}

static void test_shl(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,1);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,10);
    pu8(&p,OP_SHL);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("shl", R[0], 1024ULL);
}

static void test_shl_by_zero(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xABCD);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,0);
    pu8(&p,OP_SHL);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("shl_by_zero", R[0], 0xABCDULL);
}

static void test_shl_by_63(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,1);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,63);
    pu8(&p,OP_SHL);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("shl_by_63", R[0], (1ULL<<63));
}

static void test_shr_logical(void) {
    // SHR: logical — fills with 0 even for negative numbers
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,(uint64_t)-1LL);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,1);
    pu8(&p,OP_SHR);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("shr_logical", R[0], (uint64_t)0x7FFFFFFFFFFFFFFFULL);
}

static void test_sar_arithmetic(void) {
    // SAR: arithmetic — fills with sign bit
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,(uint64_t)-8LL);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,1);
    pu8(&p,OP_SAR);    pu8(&p,RB(REG_AX,REG_BX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK_I("sar_arithmetic", R[0], -4LL);
}

// ─── Comparisons and branches ─────────────────────────────────────────────────

// Helper: run a branch test. Sets ax=1 if branch taken, ax=0 otherwise.
// op_cmp compares ax_val with bx_val, then branches using branch_op.
static uint64_t branch_test(uint64_t ax_val, uint64_t bx_val, Op cmp_op, Op branch_op) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,ax_val);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,bx_val);
    pu8(&p,cmp_op);    pu8(&p,RB(REG_AX,REG_BX));
    // branch_op label_taken
    pu8(&p, branch_op);
    size_t patch = p.len; pi32(&p, 0);
    // not taken: ax=0, halt
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0);
    size_t jmp_over = p.len; pu8(&p,OP_JMP); pi32(&p, 0);
    // taken: ax=1
    size_t taken = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,1);
    size_t done = p.len;
    // patch branch → taken
    int32_t t = (int32_t)taken;  memcpy(p.buf+patch,    &t, 4);
    int32_t d = (int32_t)done;   memcpy(p.buf+jmp_over+1, &d, 4);
    store_r(&p, REG_AX, 0);
    pu8(&p, OP_HALT);
    run(&p);
    return R[0];
}

static void test_je(void) {
    assert(branch_test(5, 5, OP_CMP, OP_JE)  == 1); // equal → taken
    assert(branch_test(5, 6, OP_CMP, OP_JE)  == 0); // not equal → not taken
    PASS("je");
}

static void test_jne(void) {
    assert(branch_test(5, 6, OP_CMP, OP_JNE) == 1);
    assert(branch_test(5, 5, OP_CMP, OP_JNE) == 0);
    PASS("jne");
}

static void test_jl(void) {
    // signed: -1 < 1
    assert(branch_test((uint64_t)-1LL, 1, OP_CMP, OP_JL) == 1);
    assert(branch_test(1, (uint64_t)-1LL, OP_CMP, OP_JL) == 0);
    assert(branch_test(5, 5, OP_CMP, OP_JL) == 0);
    PASS("jl");
}

static void test_jle(void) {
    assert(branch_test(5, 5, OP_CMP, OP_JLE) == 1);
    assert(branch_test(4, 5, OP_CMP, OP_JLE) == 1);
    assert(branch_test(6, 5, OP_CMP, OP_JLE) == 0);
    PASS("jle");
}

static void test_jg(void) {
    assert(branch_test(6, 5, OP_CMP, OP_JG)  == 1);
    assert(branch_test(5, 5, OP_CMP, OP_JG)  == 0);
    assert(branch_test(4, 5, OP_CMP, OP_JG)  == 0);
    PASS("jg");
}

static void test_jge(void) {
    assert(branch_test(5, 5, OP_CMP, OP_JGE) == 1);
    assert(branch_test(6, 5, OP_CMP, OP_JGE) == 1);
    assert(branch_test(4, 5, OP_CMP, OP_JGE) == 0);
    PASS("jge");
}

static void test_test_flag(void) {
    // TEST sets ZF if (a & b) == 0
    assert(branch_test(0xF0, 0x0F, OP_TEST, OP_JE)  == 1); // no overlap → ZF → JE taken
    assert(branch_test(0xFF, 0x0F, OP_TEST, OP_JE)  == 0); // overlap → no ZF
    assert(branch_test(0xF0, 0x0F, OP_TEST, OP_JNE) == 0);
    PASS("test_flag");
}

// ─── Memory ops ───────────────────────────────────────────────────────────────

static void test_mov_rm_mr(void) {
    static uint64_t mem = 0xDEADBEEF;
    Prog p = {0};
    // mov ax, [&mem]
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,(uint64_t)&mem);
    pu8(&p,OP_MOV_RM); pu8(&p,RB(REG_AX,REG_BX));
    // store ax+1 back to mem
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_CX,0)); pu64(&p,1);
    pu8(&p,OP_ADD);    pu8(&p,RB(REG_AX,REG_CX));
    pu8(&p,OP_MOV_MR); pu8(&p,RB(REG_BX,REG_AX));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("mov_rm_mr_result", R[0], 0xDEADBEEF+1);
    assert(mem == 0xDEADBEEF+1 && "mov_mr wrote back");
    PASS("mov_rm_mr");
    mem = 0xDEADBEEF; // reset
}

// ─── Stack frame ──────────────────────────────────────────────────────────────

static void test_enter_leave(void) {
    // ENTER 16: allocates 2 local slots; write to them, read back
    // frame layout after ENTER 16:
    //   [bp+0]  = saved bp
    //   [bp-8]  = local 0   (sp = bp-16 after enter)
    //   [bp-16] = local 1
    Prog p = {0};
    pu8(&p, OP_ENTER); pu16(&p, 16);
    // write 0xAA to [bp-8] and 0xBB to [bp-16]
    // bp is in rf->bp, but we need it in a reg
    // use the data stack push/pop to bridge — simpler: just use sp offset
    // sp = bp - 16 after ENTER
    // store 0xAA at [sp+8]  (= bp-8)
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xAA);
    pu8(&p,OP_PUSH);   pu8(&p,RB(REG_AX,0));
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xBB);
    pu8(&p,OP_PUSH);   pu8(&p,RB(REG_AX,0));
    // pop back in order and store results
    pu8(&p,OP_POP);    pu8(&p,RB(REG_AX,0));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_POP);    pu8(&p,RB(REG_AX,0));
    store_r(&p, REG_AX, 1);
    pu8(&p, OP_LEAVE);
    pu8(&p, OP_HALT);
    run(&p);
    CHECK("enter_leave_0", R[0], 0xBBULL);
    CHECK("enter_leave_1", R[1], 0xAAULL);
    PASS("enter_leave");
}

static void test_push_pop_all_regs(void) {
    // Push all 8 GPRs, pop into opposite registers, verify
    Prog p = {0};
    for (int i = 0; i < 8; i++) {
        pu8(&p,OP_MOV_RI); pu8(&p,RB(i,0)); pu64(&p,(uint64_t)(i+1)*10);
    }
    // push ax..fx
    for (int i = 0; i < 8; i++) { pu8(&p,OP_PUSH); pu8(&p,RB(i,0)); }
    // pop into fx..ax (reverse)
    for (int i = 7; i >= 0; i--) { pu8(&p,OP_POP); pu8(&p,RB(i,0)); }
    // now each reg should still hold its original value
    for (int i = 0; i < 8; i++) store_r_safe(&p, i, i);
    pu8(&p, OP_HALT);
    run(&p);
    for (int i = 0; i < 8; i++) {
        if (R[i] != (uint64_t)(i+1)*10) {
            FAIL("push_pop_all_regs", "reg%d: got %llu want %llu",
                 i, (unsigned long long)R[i], (unsigned long long)(i+1)*10);
        }
    }
    PASS("push_pop_all_regs");
}

// ─── Float ────────────────────────────────────────────────────────────────────

static void test_fsub(void) {
    Prog p = {0};
    double a=10.0, b=3.5, raw;
    uint64_t ra, rb;
    memcpy(&ra,&a,8); memcpy(&rb,&b,8);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(0,0)); pu64(&p,ra);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(1,0)); pu64(&p,rb);
    pu8(&p,OP_FSUB);    pu8(&p,RB(0,1));
    store_f(&p, 0, 0);
    pu8(&p,OP_HALT);
    run(&p);
    memcpy(&raw, &FPR[0], 8);
    CHECK_F("fsub", raw, 6.5);
}

static void test_fmul(void) {
    Prog p = {0};
    double a=3.0, b=14.0, raw; uint64_t ra,rb;
    memcpy(&ra,&a,8); memcpy(&rb,&b,8);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(0,0)); pu64(&p,ra);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(1,0)); pu64(&p,rb);
    pu8(&p,OP_FMUL);    pu8(&p,RB(0,1));
    store_f(&p, 0, 0);
    pu8(&p,OP_HALT);
    run(&p);
    memcpy(&raw, &FPR[0], 8);
    CHECK_F("fmul", raw, 42.0);
}

static void test_fdiv(void) {
    Prog p = {0};
    double a=84.0, b=2.0, raw; uint64_t ra,rb;
    memcpy(&ra,&a,8); memcpy(&rb,&b,8);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(0,0)); pu64(&p,ra);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(1,0)); pu64(&p,rb);
    pu8(&p,OP_FDIV);    pu8(&p,RB(0,1));
    store_f(&p, 0, 0);
    pu8(&p,OP_HALT);
    run(&p);
    memcpy(&raw, &FPR[0], 8);
    CHECK_F("fdiv", raw, 42.0);
}

static void test_fdiv_by_zero(void) {
    // IEEE 754: x / 0.0 = +inf, not a crash
    Prog p = {0};
    double a=1.0, b=0.0, raw; uint64_t ra,rb;
    memcpy(&ra,&a,8); memcpy(&rb,&b,8);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(0,0)); pu64(&p,ra);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(1,0)); pu64(&p,rb);
    pu8(&p,OP_FDIV);    pu8(&p,RB(0,1));
    store_f(&p, 0, 0);
    pu8(&p,OP_HALT);
    run(&p);
    memcpy(&raw, &FPR[0], 8);
    assert(isinf(raw) && raw > 0 && "fdiv by zero should be +inf");
    PASS("fdiv_by_zero");
}

static void test_fcmp_equal(void) {
    // fcmp equal → ZF set → JE taken
    Prog p = {0};
    double a=3.14; uint64_t ra; memcpy(&ra,&a,8);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(0,0)); pu64(&p,ra);
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(1,0)); pu64(&p,ra);
    pu8(&p,OP_FCMP);    pu8(&p,RB(0,1));
    // JE taken → ax=1
    pu8(&p,OP_JE); size_t patch=p.len; pi32(&p,0);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0);
    size_t jover=p.len; pu8(&p,OP_JMP); pi32(&p,0);
    size_t taken=p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,1);
    size_t done=p.len;
    int32_t t=(int32_t)taken; memcpy(p.buf+patch,   &t,4);
    int32_t d=(int32_t)done;  memcpy(p.buf+jover+1, &d,4);
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK("fcmp_equal", R[0], 1ULL);
}

static void test_fmov_rm_mr(void) {
    static double fmem = 0.0;
    double val = 2.718281828;
    uint64_t rv; memcpy(&rv,&val,8);
    Prog p = {0};
    // store val into fmem via fp reg
    pu8(&p,OP_FMOV_RI); pu8(&p,RB(0,0)); pu64(&p,rv);
    pu8(&p,OP_MOV_RI);  pu8(&p,RB(REG_BX,0)); pu64(&p,(uint64_t)&fmem);
    pu8(&p,OP_FMOV_MR); pu8(&p,RB(REG_BX,0));  // [bx] = fp0
    // load it back into fp1
    pu8(&p,OP_FMOV_RM); pu8(&p,RB(1,REG_BX));   // fp1 = [bx]
    // ftoi ax, fp1
    pu8(&p,OP_FTOI); pu8(&p,RB(REG_AX,1));
    store_r(&p, REG_AX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    assert(fmem == val && "fmov_mr wrote to memory");
    CHECK("fmov_rm_mr", R[0], 2ULL);  // (int64)2.718 = 2
    PASS("fmov_rm_mr");
}

static void test_itof_ftoi_roundtrip(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,(uint64_t)-99LL);
    pu8(&p,OP_ITOF);   pu8(&p,RB(0,REG_AX));    // fp0 = (double)-99
    pu8(&p,OP_FTOI);   pu8(&p,RB(REG_BX,0));    // bx = (int64)fp0
    store_r(&p, REG_BX, 0);
    pu8(&p,OP_HALT);
    run(&p);
    CHECK_I("itof_ftoi_roundtrip", R[0], -99LL);
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(void) {
    printf("─── arithmetic ─────────────────────────────\n");
    test_sub();
    test_mul();
    test_imul_negative();
    test_div();
    test_idiv_negative();
    test_mod();
    test_neg();

    printf("─── bitwise ────────────────────────────────\n");
    test_and();
    test_or();
    test_xor();
    test_not();
    test_not_allones();
    test_shl();
    test_shl_by_zero();
    test_shl_by_63();
    test_shr_logical();
    test_sar_arithmetic();

    printf("─── branches ───────────────────────────────\n");
    test_je();
    test_jne();
    test_jl();
    test_jle();
    test_jg();
    test_jge();
    test_test_flag();

    printf("─── memory ─────────────────────────────────\n");
    test_mov_rm_mr();

    printf("─── stack frame ────────────────────────────\n");
    test_enter_leave();
    test_push_pop_all_regs();

    printf("─── float ──────────────────────────────────\n");
    test_fsub();
    test_fmul();
    test_fdiv();
    test_fdiv_by_zero();
    test_fcmp_equal();
    test_fmov_rm_mr();
    test_itof_ftoi_roundtrip();

    printf("────────────────────────────────────────────\n");
    printf("All op tests passed.\n");
    return 0;
}
