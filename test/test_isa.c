#include "vm.h"
#include "asm.h"
#include "dvm_io.h"
#include "dvm_path.h"
#include "linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

// ─── Path resolution ──────────────────────────────────────────────────────────
static char _proj_root[4096];
static char _asm_dir[4096];
static char _path_buf[4096];

static void init_paths(void) {
    char abs[4096] = {0};
    if (!dvm_abspath(__FILE__, abs, sizeof(abs))) {
        snprintf(_proj_root, sizeof(_proj_root), ".");
        snprintf(_asm_dir,   sizeof(_asm_dir),   "./test/asm");
        return;
    }
    dvm_dirname(abs);   // strip filename → .../test
    dvm_dirname(abs);   // strip /test    → project root
    snprintf(_proj_root, sizeof(_proj_root), "%s", abs);
    snprintf(_asm_dir,   sizeof(_asm_dir),   "%s/test/asm", abs);
}

static const char *asm_path(const char *name) {
    dvm_path_join(_path_buf, sizeof(_path_buf), _asm_dir, name);
    return _path_buf;
}

static const char *proj_path(const char *rel) {
    dvm_path_join(_path_buf, sizeof(_path_buf), _proj_root, rel);
    return _path_buf;
}

// ─── Infra ────────────────────────────────────────────────────────────────────
#define PASS(name) printf("PASS  %-40s\n", name)
#define FAIL(name, ...) do { \
    fprintf(stderr, "FAIL  %s: ", name); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); \
} while(0)
#define CHECK(name, got, want) do { \
    if ((uint64_t)(got) != (uint64_t)(want)) \
        FAIL(name, "got 0x%llX want 0x%llX", \
             (unsigned long long)(got), (unsigned long long)(want)); \
    PASS(name); \
} while(0)

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc((size_t)sz + 1);
    (void)fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static int asm_file(const char *path, DvmProg *out) {
    char *src = read_file(path);
    if (!src) return 0;
    AsmCtx ctx; int ok = asm_compile(src, &ctx); free(src);
    if (!ok) return 0;
    DvmProg tmp; asm_to_prog(&ctx, &tmp);
    memset(out, 0, sizeof(*out));
    if (tmp.cnst_len) { out->cnst = malloc(tmp.cnst_len); memcpy(out->cnst, tmp.cnst, tmp.cnst_len); }
    if (tmp.data_len) { out->data = malloc(tmp.data_len); memcpy(out->data, tmp.data, tmp.data_len); }
    if (tmp.code_len) { out->code = malloc(tmp.code_len); memcpy(out->code, tmp.code, tmp.code_len); }
    out->cnst_len = tmp.cnst_len; out->data_len = tmp.data_len; out->code_len = tmp.code_len;
    out->nsyms = tmp.nsyms; out->nrels = tmp.nrels;
    out->has_entry = tmp.has_entry; out->entry_offset = tmp.entry_offset;
    memcpy(out->syms, tmp.syms, tmp.nsyms * sizeof(DvmSym));
    memcpy(out->rels, tmp.rels, tmp.nrels  * sizeof(DvmRel));
    return 1;
}

static int64_t sym_offset_in(const DvmProg *p, const char *name, DvmSection *sec) {
    for (size_t i = 0; i < p->nsyms; i++)
        if (!strcmp(p->syms[i].name, name)) {
            if (sec) *sec = p->syms[i].section;
            return p->syms[i].offset;
        }
    return -1;
}

static uint64_t run_asm_file(const char *path) {
    DvmProg prog; assert(asm_file(path, &prog));
    DvmLoaded loaded; assert(dvm_load(&prog, &loaded));
    DvmSection sec; int64_t roff = sym_offset_in(&prog, "result", &sec);
    assert(roff >= 0 && sec == DVM_SEC_DATA);
    uint64_t *rp = (uint64_t*)(loaded.data + roff);
    vm_run(loaded.code + loaded.entry_offset, 256 * 1024);
    uint64_t val = *rp;
    dvm_unload(&loaded); dvm_prog_free(&prog);
    return val;
}

// ─── Direct interpreter tests ─────────────────────────────────────────────────
typedef struct { uint8_t buf[1024]; size_t len; } Prog;
static void pu8 (Prog *p, uint8_t  v) { p->buf[p->len++] = v; }
static void pi32(Prog *p, int32_t  v) { memcpy(p->buf + p->len, &v, 4); p->len += 4; }
static void pu64(Prog *p, uint64_t v) { memcpy(p->buf + p->len, &v, 8); p->len += 8; }
#define RB(d,s) ((uint8_t)(((d)<<4)|(s)))

static uint64_t R[8];

static void store_r(Prog *p, uint8_t src, int slot) {
    pu8(p, OP_MOV_RI); pu8(p, RB(REG_DI, 0)); pu64(p, (uint64_t)&R[slot]);
    pu8(p, OP_MOV_MR); pu8(p, RB(REG_DI, src));
}

static void run(Prog *p) {
    memset(R, 0, sizeof(R));
    vm_run(p->buf, 64 * 1024);
}

// ── Sized loads ────────────────────────────────────────────────────────────────
static void test_movzx8(void) {
    static uint8_t mem = 0xFE;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);    pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOVZX_RM8); pu8(&p, RB(REG_AX, REG_BX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movzx_rm8", R[0], 0xFEULL);
}

static void test_movsx8(void) {
    static int8_t mem = -1;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);    pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOVSX_RM8); pu8(&p, RB(REG_AX, REG_BX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movsx_rm8", (int64_t)R[0], -1LL);
}

static void test_movzx16(void) {
    static uint16_t mem = 0xABCD;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOVZX_RM16); pu8(&p, RB(REG_AX, REG_BX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movzx_rm16", R[0], 0xABCDULL);
}

static void test_movzx32(void) {
    static uint32_t mem = 0xDEADBEEF;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOVZX_RM32); pu8(&p, RB(REG_AX, REG_BX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movzx_rm32", R[0], 0xDEADBEEFULL);
}

static void test_movsx32(void) {
    static int32_t mem = -42;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOVSX_RM32); pu8(&p, RB(REG_AX, REG_BX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movsx_rm32", (int64_t)R[0], -42LL);
}

// ── Sized stores ───────────────────────────────────────────────────────────────
static void test_mov_mr8(void) {
    static uint64_t mem = 0xFFFFFFFFFFFFFFFFULL;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);  pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOV_RI);  pu8(&p, RB(REG_AX, 0)); pu64(&p, 0x42ULL);
    pu8(&p, OP_MOV_MR8); pu8(&p, RB(REG_BX, REG_AX));
    pu8(&p, OP_HALT);
    run(&p);
    assert(mem == 0xFFFFFFFFFFFFFF42ULL);
    PASS("mov_mr8");
    mem = 0xFFFFFFFFFFFFFFFFULL;
}

static void test_mov_mr16(void) {
    static uint64_t mem = 0xFFFFFFFFFFFFFFFFULL;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);   pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOV_RI);   pu8(&p, RB(REG_AX, 0)); pu64(&p, 0x1234ULL);
    pu8(&p, OP_MOV_MR16); pu8(&p, RB(REG_BX, REG_AX));
    pu8(&p, OP_HALT);
    run(&p);
    assert(mem == 0xFFFFFFFFFFFF1234ULL);
    PASS("mov_mr16");
    mem = 0xFFFFFFFFFFFFFFFFULL;
}

static void test_mov_mr32(void) {
    static uint64_t mem = 0xFFFFFFFFFFFFFFFFULL;
    Prog p = {0};
    pu8(&p, OP_MOV_RI);   pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&mem);
    pu8(&p, OP_MOV_RI);   pu8(&p, RB(REG_AX, 0)); pu64(&p, 0xCAFEBABEULL);
    pu8(&p, OP_MOV_MR32); pu8(&p, RB(REG_BX, REG_AX));
    pu8(&p, OP_HALT);
    run(&p);
    assert(mem == 0xFFFFFFFFCAFEBABEULL);
    PASS("mov_mr32");
    mem = 0xFFFFFFFFFFFFFFFFULL;
}

// ── Indexed memory ─────────────────────────────────────────────────────────────
static void test_mov_rm_off(void) {
    static uint64_t arr[4] = {10, 20, 30, 40};
    Prog p = {0};
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)arr);
    pu8(&p, OP_MOV_RM_OFF); pu8(&p, RB(REG_AX, REG_BX)); pi32(&p, 16);
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("mov_rm_off", R[0], 30ULL);
}

static void test_mov_mr_off(void) {
    static uint64_t arr[4] = {0, 0, 0, 0};
    Prog p = {0};
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)arr);
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_AX, 0)); pu64(&p, 0xBEEFULL);
    pu8(&p, OP_MOV_MR_OFF); pu8(&p, RB(REG_BX, REG_AX)); pi32(&p, 8);
    pu8(&p, OP_HALT);
    run(&p);
    assert(arr[1] == 0xBEEF);
    PASS("mov_mr_off");
    arr[1] = 0;
}

static void test_indexed_negative_off(void) {
    static uint64_t arr[4] = {10, 20, 30, 40};
    Prog p = {0};
    pu8(&p, OP_MOV_RI);     pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&arr[2]);
    pu8(&p, OP_MOV_RM_OFF); pu8(&p, RB(REG_AX, REG_BX)); pi32(&p, -8);
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("indexed_negative_off", R[0], 20ULL);
}

static void test_movzx_rm8_off(void) {
    static uint8_t arr[4] = {0x11, 0x22, 0xFE, 0x44};
    Prog p = {0};
    pu8(&p, OP_MOV_RI);        pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)arr);
    pu8(&p, OP_MOVZX_RM8_OFF); pu8(&p, RB(REG_AX, REG_BX)); pi32(&p, 2);
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movzx_rm8_off", R[0], 0xFEULL);
}

static void test_movsx_rm8_off(void) {
    static int8_t arr[4] = {1, 2, -1, 4};
    Prog p = {0};
    pu8(&p, OP_MOV_RI);        pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)arr);
    pu8(&p, OP_MOVSX_RM8_OFF); pu8(&p, RB(REG_AX, REG_BX)); pi32(&p, 2);
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("movsx_rm8_off", (int64_t)R[0], -1LL);
}

// ── Indirect call ──────────────────────────────────────────────────────────────
static void test_callr(void) {
    Prog p = {0};
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_AX, 0)); pu64(&p, 21ULL);
    size_t bx_load = p.len;
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_BX, 0)); pu64(&p, 0ULL); // patched below
    pu8(&p, OP_CALLR);  pu8(&p, RB(REG_BX, 0));
    size_t err_patch = p.len; pi32(&p, 0);                        // patched below
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    size_t die_off = p.len;
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_AX, 0)); pu64(&p, 0xDEADULL);
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    size_t fn_off = p.len;   // double_fn: ax = ax + ax
    pu8(&p, OP_MOV_RR); pu8(&p, RB(REG_BX, REG_AX));
    pu8(&p, OP_ADD);    pu8(&p, RB(REG_AX, REG_BX));
    pu8(&p, OP_RET);
    // patch bx = native ptr to double_fn
    uint64_t fn_ptr = (uint64_t)(p.buf + fn_off);
    memcpy(p.buf + bx_load + 2, &fn_ptr, 8);
    // patch err_off = die_off
    int32_t die_i = (int32_t)die_off;
    memcpy(p.buf + err_patch, &die_i, 4);
    memset(R, 0, sizeof(R));
    vm_run(p.buf, 64 * 1024);
    CHECK("callr", R[0], 42ULL);
}

// ── Alloca ─────────────────────────────────────────────────────────────────────
static void test_alloca(void) {
    Prog p = {0};
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_AX, 0)); pu64(&p, 16ULL);
    pu8(&p, OP_ALLOCA);
    pu8(&p, OP_MOV_RR); pu8(&p, RB(REG_BX, REG_AX));
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_CX, 0)); pu64(&p, 0xCAFEULL);
    pu8(&p, OP_MOV_MR); pu8(&p, RB(REG_BX, REG_CX));
    pu8(&p, OP_MOV_RM); pu8(&p, RB(REG_AX, REG_BX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("alloca", R[0], 0xCAFEULL);
}

// ── LEA ────────────────────────────────────────────────────────────────────────
static void test_lea(void) {
    static uint64_t arr[3] = {0xAAAA, 0xBBBB, 0xCCCC};
    Prog p = {0};
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_BX, 0)); pu64(&p, (uint64_t)&arr[1]);
    pu8(&p, OP_LEA);    pu8(&p, RB(REG_AX, REG_BX)); pi32(&p, -8);
    pu8(&p, OP_MOV_RM); pu8(&p, RB(REG_AX, REG_AX));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("lea", R[0], 0xAAAAULL);
}

// ── Truncation ─────────────────────────────────────────────────────────────────
static void test_trunc8(void) {
    Prog p = {0};
    pu8(&p, OP_MOV_RI); pu8(&p, RB(REG_AX, 0)); pu64(&p, 0xDEADBEEFCAFE1234ULL);
    pu8(&p, OP_TRUNC8); pu8(&p, RB(REG_AX, 0));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("trunc8", R[0], 0x34ULL);
}

static void test_trunc16(void) {
    Prog p = {0};
    pu8(&p, OP_MOV_RI);  pu8(&p, RB(REG_AX, 0)); pu64(&p, 0xDEADBEEFCAFE1234ULL);
    pu8(&p, OP_TRUNC16); pu8(&p, RB(REG_AX, 0));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("trunc16", R[0], 0x1234ULL);
}

static void test_trunc32(void) {
    Prog p = {0};
    pu8(&p, OP_MOV_RI);  pu8(&p, RB(REG_AX, 0)); pu64(&p, 0xDEADBEEFCAFE1234ULL);
    pu8(&p, OP_TRUNC32); pu8(&p, RB(REG_AX, 0));
    store_r(&p, REG_AX, 0); pu8(&p, OP_HALT);
    run(&p);
    CHECK("trunc32", R[0], 0xCAFE1234ULL);
}

// ─── .asm file tests ──────────────────────────────────────────────────────────
static void test_asm_sized_mem(void) { CHECK("asm_sized_mem", run_asm_file(asm_path("sized_mem.asm")), 0x08ULL); }
static void test_asm_indexed(void)   { CHECK("asm_indexed",   run_asm_file(asm_path("indexed.asm")),   60ULL);   }
static void test_asm_callr(void)     { CHECK("asm_callr",     run_asm_file(asm_path("callr.asm")),     42ULL);   }
static void test_asm_alloca(void)    { CHECK("asm_alloca",    run_asm_file(asm_path("alloca.asm")),    0xABCDULL); }
static void test_asm_trunc(void)     { CHECK("asm_trunc",     run_asm_file(asm_path("trunc.asm")),     0x34ULL); }

// ─── Linker entry point ───────────────────────────────────────────────────────
static DvmProg make_prog(const char *src) {
    AsmCtx ctx; assert(asm_compile(src, &ctx));
    DvmProg tmp; asm_to_prog(&ctx, &tmp);
    DvmProg prog = {0};
    prog.code = malloc(tmp.code_len); memcpy(prog.code, tmp.code, tmp.code_len); prog.code_len = tmp.code_len;
    prog.nsyms = tmp.nsyms; prog.nrels = tmp.nrels;
    memcpy(prog.syms, tmp.syms, tmp.nsyms * sizeof(DvmSym));
    memcpy(prog.rels, tmp.rels, tmp.nrels  * sizeof(DvmRel));
    return prog;
}

static void test_entry_default_zero(void) {
    DvmProg prog = make_prog(".text\nhalt\n");
    DvmProg out; assert(dvm_link(&prog, 1, &out, NULL));
    assert(out.entry_offset == 0);
    assert(out.has_entry == 1);
    dvm_prog_free(&out); dvm_prog_free(&prog);
    PASS("entry_default_zero");
}

static void test_entry_start_symbol(void) {
    DvmProg prog = make_prog(
        ".text\n"
        "    halt\n"
        "    halt\n"
        "global _start\n"
        "_start:\n"
        "    halt\n");
    DvmProg out; assert(dvm_link(&prog, 1, &out, NULL));
    assert(out.entry_offset == 2);
    dvm_prog_free(&out); dvm_prog_free(&prog);
    PASS("entry_start_symbol");
}

static void test_entry_explicit(void) {
    DvmProg prog = make_prog(
        ".text\n"
        "global _start\n"
        "_start: halt\n"
        "global mymain\n"
        "mymain: halt\n");
    DvmProg out; assert(dvm_link(&prog, 1, &out, "mymain"));
    assert(out.entry_offset == 1);
    dvm_prog_free(&out); dvm_prog_free(&prog);
    PASS("entry_explicit_sym");
}

static void test_entry_multiobj_linked(void) {
    DvmProg lib, main_obj, linked;
    assert(asm_file(asm_path("entry_lib.asm"),  &lib));
    assert(asm_file(asm_path("entry_main.asm"), &main_obj));
    const DvmProg objs[2] = {main_obj, lib};
    assert(dvm_link(objs, 2, &linked, NULL));
    assert(linked.has_entry);
    DvmLoaded loaded; assert(dvm_load(&linked, &loaded));
    DvmSection sec; int64_t roff = sym_offset_in(&linked, "result", &sec);
    assert(roff >= 0 && sec == DVM_SEC_DATA);
    uint64_t *rp = (uint64_t*)(loaded.data + roff);
    vm_run(loaded.code + loaded.entry_offset, 256 * 1024);
    uint64_t got = *rp;
    dvm_unload(&loaded);
    dvm_prog_free(&linked); dvm_prog_free(&lib); dvm_prog_free(&main_obj);
    CHECK("entry_multiobj", got, 37ULL);
}

static void test_entry_round_trip(void) {
    DvmProg prog = make_prog(
        ".text\n"
        "    halt\n"
        "    halt\n"
        "global _start\n"
        "_start: halt\n");
    DvmProg linked; assert(dvm_link(&prog, 1, &linked, NULL));
    uint32_t expected_entry = linked.entry_offset;
    assert(expected_entry == 2);

    // Write to bin/ so we stay within the project tree (works on all platforms)
    const char *tmp_path = proj_path("bin/dvm_entry_rt.dvm");
    assert(dvm_write_file(tmp_path, &linked));
    DvmProg rt; assert(dvm_read_file(tmp_path, &rt));
    assert(rt.entry_offset == expected_entry);
    assert(rt.has_entry);
    DvmLoaded loaded; assert(dvm_load(&rt, &loaded));
    assert(loaded.entry_offset == expected_entry);
    dvm_unload(&loaded);
    dvm_prog_free(&rt);
    dvm_prog_free(&linked);
    dvm_prog_free(&prog);
    PASS("entry_round_trip");
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main(void) {
    init_paths();

    printf("─── sized loads ────────────────────────────\n");
    test_movzx8(); test_movsx8(); test_movzx16(); test_movzx32(); test_movsx32();

    printf("─── sized stores ───────────────────────────\n");
    test_mov_mr8(); test_mov_mr16(); test_mov_mr32();

    printf("─── indexed memory ─────────────────────────\n");
    test_mov_rm_off(); test_mov_mr_off(); test_indexed_negative_off();
    test_movzx_rm8_off(); test_movsx_rm8_off();

    printf("─── indirect call / stack / trunc ──────────\n");
    test_callr(); test_alloca(); test_lea();
    test_trunc8(); test_trunc16(); test_trunc32();

    printf("─── .asm file tests ────────────────────────\n");
    test_asm_sized_mem(); test_asm_indexed(); test_asm_callr();
    test_asm_alloca(); test_asm_trunc();

    printf("─── linker entry point ─────────────────────\n");
    test_entry_default_zero(); test_entry_start_symbol();
    test_entry_explicit(); test_entry_multiobj_linked();
    test_entry_round_trip();

    printf("────────────────────────────────────────────\n");
    printf("All ISA tests passed.\n");
    return 0;
}
