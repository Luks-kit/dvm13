#include "vm.h"
#include "asm.h"
#include "dvm_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

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

// ─── Round-trip helpers ───────────────────────────────────────────────────────

static void round_trip(const DvmProg *orig, DvmProg *out) {
    const char *tmp = "/tmp/dvm_test_binary.dvm";
    assert(dvm_write_file(tmp, orig));
    assert(dvm_read_file(tmp, out));
}

// Deep-copy DvmProg section data from an AsmCtx (ctx is stack-local)
static void prog_from_ctx(const AsmCtx *ctx, DvmProg *prog) {
    DvmProg tmp; asm_to_prog(ctx, &tmp);
    memset(prog, 0, sizeof(*prog));
    if (tmp.cnst_len) { prog->cnst = malloc(tmp.cnst_len); memcpy(prog->cnst, tmp.cnst, tmp.cnst_len); }
    if (tmp.data_len) { prog->data = malloc(tmp.data_len); memcpy(prog->data, tmp.data, tmp.data_len); }
    if (tmp.code_len) { prog->code = malloc(tmp.code_len); memcpy(prog->code, tmp.code, tmp.code_len); }
    prog->cnst_len = tmp.cnst_len;
    prog->data_len = tmp.data_len;
    prog->code_len = tmp.code_len;
    prog->nsyms    = tmp.nsyms;
    prog->nrels    = tmp.nrels;
    memcpy(prog->syms, tmp.syms, tmp.nsyms * sizeof(DvmSym));
    memcpy(prog->rels, tmp.rels, tmp.nrels  * sizeof(DvmRel));
}

// ─── Format: write / read round-trips ────────────────────────────────────────

static void test_rt_minimal(void) {
    // Minimal valid prog: one HALT in CODE, nothing else
    DvmProg orig = {0};
    uint8_t code[] = { OP_HALT };
    orig.code = code; orig.code_len = 1;

    DvmProg rt; round_trip(&orig, &rt);

    assert(rt.code_len == 1 && rt.code[0] == OP_HALT);
    assert(rt.cnst_len == 0);
    assert(rt.data_len == 0);
    assert(rt.nsyms == 0);
    assert(rt.nrels == 0);
    dvm_prog_free(&rt);
    PASS("rt_minimal");
}

static void test_rt_all_sections(void) {
    // All three sections filled with distinct sentinel bytes
    DvmProg orig = {0};
    uint8_t cnst[] = { 0xAA, 0xBB, 0xCC };
    uint8_t data[] = { 0x11, 0x22 };
    uint8_t code[] = { OP_HALT };
    orig.cnst = cnst; orig.cnst_len = 3;
    orig.data = data; orig.data_len = 2;
    orig.code = code; orig.code_len = 1;

    DvmProg rt; round_trip(&orig, &rt);

    assert(rt.cnst_len == 3 && rt.cnst[0] == 0xAA && rt.cnst[2] == 0xCC);
    assert(rt.data_len == 2 && rt.data[0] == 0x11 && rt.data[1] == 0x22);
    assert(rt.code_len == 1 && rt.code[0] == OP_HALT);
    dvm_prog_free(&rt);
    PASS("rt_all_sections");
}

static void test_rt_syms(void) {
    // Symbol table survives round-trip with correct flags/section/offset
    DvmProg orig = {0};
    uint8_t code[] = { OP_HALT };
    orig.code = code; orig.code_len = 1;

    orig.nsyms = 3;
    snprintf(orig.syms[0].name, DVM_SYM_NAME, "local_fn");
    orig.syms[0].flags = DVM_SYM_LOCAL; orig.syms[0].section = DVM_SEC_CODE; orig.syms[0].offset = 0;
    snprintf(orig.syms[1].name, DVM_SYM_NAME, "exported");
    orig.syms[1].flags = DVM_SYM_GLOBAL; orig.syms[1].section = DVM_SEC_DATA; orig.syms[1].offset = 8;
    snprintf(orig.syms[2].name, DVM_SYM_NAME, "imported");
    orig.syms[2].flags = DVM_SYM_EXTERN; orig.syms[2].section = DVM_SEC_NONE; orig.syms[2].offset = 0;

    DvmProg rt; round_trip(&orig, &rt);

    assert(rt.nsyms == 3);
    assert(rt.syms[0].flags   == DVM_SYM_LOCAL  && rt.syms[0].section == DVM_SEC_CODE && rt.syms[0].offset == 0);
    assert(rt.syms[1].flags   == DVM_SYM_GLOBAL && rt.syms[1].section == DVM_SEC_DATA && rt.syms[1].offset == 8);
    assert(rt.syms[2].flags   == DVM_SYM_EXTERN && rt.syms[2].section == DVM_SEC_NONE);
    assert(!strcmp(rt.syms[0].name, "local_fn"));
    assert(!strcmp(rt.syms[1].name, "exported"));
    assert(!strcmp(rt.syms[2].name, "imported"));
    dvm_prog_free(&rt);
    PASS("rt_syms_flags");
}

static void test_rt_rels(void) {
    // Relocation table: both kinds survive round-trip
    DvmProg orig = {0};
    uint8_t code[16] = { OP_HALT };
    orig.code = code; orig.code_len = 16;
    orig.nsyms = 1;
    snprintf(orig.syms[0].name, DVM_SYM_NAME, "target");
    orig.syms[0].flags = DVM_SYM_GLOBAL; orig.syms[0].section = DVM_SEC_DATA; orig.syms[0].offset = 4;

    orig.nrels = 2;
    orig.rels[0].code_offset = 1; orig.rels[0].sym_index = 0; orig.rels[0].kind = DVM_REL_DATA;
    orig.rels[1].code_offset = 9; orig.rels[1].sym_index = 0; orig.rels[1].kind = DVM_REL_CODE;

    DvmProg rt; round_trip(&orig, &rt);

    assert(rt.nrels == 2);
    assert(rt.rels[0].code_offset == 1 && rt.rels[0].sym_index == 0 && rt.rels[0].kind == DVM_REL_DATA);
    assert(rt.rels[1].code_offset == 9 && rt.rels[1].sym_index == 0 && rt.rels[1].kind == DVM_REL_CODE);
    dvm_prog_free(&rt);
    PASS("rt_rels_kinds");
}

static void test_rt_long_sym_name(void) {
    // Symbol name at max length (63 chars + NUL)
    DvmProg orig = {0};
    uint8_t code[] = { OP_HALT };
    orig.code = code; orig.code_len = 1;
    orig.nsyms = 1;
    memset(orig.syms[0].name, 'a', DVM_SYM_NAME - 1);
    orig.syms[0].name[DVM_SYM_NAME - 1] = 0;
    orig.syms[0].flags = DVM_SYM_GLOBAL; orig.syms[0].section = DVM_SEC_CODE; orig.syms[0].offset = 0;

    DvmProg rt; round_trip(&orig, &rt);
    assert(!strcmp(rt.syms[0].name, orig.syms[0].name));
    dvm_prog_free(&rt);
    PASS("rt_long_sym_name");
}

static void test_rt_large_code(void) {
    // 4KB code section survives intact
    DvmProg orig = {0};
    uint8_t *code = malloc(4096);
    for (int i = 0; i < 4096; i++) code[i] = (uint8_t)(i & 0xFF);
    code[4095] = OP_HALT;
    orig.code = code; orig.code_len = 4096;

    DvmProg rt; round_trip(&orig, &rt);
    assert(rt.code_len == 4096);
    assert(memcmp(rt.code, orig.code, 4096) == 0);
    dvm_prog_free(&rt);
    free(code);
    PASS("rt_large_code");
}

// ─── Error paths ──────────────────────────────────────────────────────────────

static void test_bad_magic(void) {
    const char *tmp = "/tmp/dvm_test_badmagic.dvm";
    FILE *f = fopen(tmp, "wb"); fwrite("BAAD\0\0\0\0", 1, 8, f); fclose(f);
    DvmProg prog;
    FILE *old = stderr; stderr = fopen("/dev/null", "w");
    int ok = dvm_read_file(tmp, &prog);
    fclose(stderr); stderr = old;
    assert(!ok);
    PASS("bad_magic_rejected");
}

static void test_truncated_file(void) {
    // Write a valid header then truncate — should fail gracefully
    const char *tmp = "/tmp/dvm_test_trunc.dvm";
    DvmProg orig = {0};
    uint8_t code[] = { OP_HALT };
    orig.code = code; orig.code_len = 1;
    assert(dvm_write_file(tmp, &orig));

    // Truncate to 8 bytes (just past magic + CNST tag)
    truncate(tmp, 8);

    DvmProg prog;
    FILE *old = stderr; stderr = fopen("/dev/null", "w");
    int ok = dvm_read_file(tmp, &prog);
    fclose(stderr); stderr = old;
    assert(!ok);
    PASS("truncated_file_rejected");
}

// ─── Pseudo-op byte layout ────────────────────────────────────────────────────
// These check that the assembler emits the correct bytes in data sections,
// independent of the VM running anything.

static void test_db(void) {
    AsmCtx ctx;
    assert(asm_compile(".data\nfoo: db 0x42\n.text\nhalt\n", &ctx));
    assert(ctx.data.len == 1 && ctx.data.buf[0] == 0x42);
    PASS("pseudo_db");
}

static void test_dw(void) {
    AsmCtx ctx;
    assert(asm_compile(".data\nfoo: dw 0x1234\n.text\nhalt\n", &ctx));
    assert(ctx.data.len == 2);
    uint16_t v; memcpy(&v, ctx.data.buf, 2);
    assert(v == 0x1234);
    PASS("pseudo_dw_le");
}

static void test_dd(void) {
    AsmCtx ctx;
    assert(asm_compile(".data\nfoo: dd 0xDEADBEEF\n.text\nhalt\n", &ctx));
    assert(ctx.data.len == 4);
    uint32_t v; memcpy(&v, ctx.data.buf, 4);
    assert(v == 0xDEADBEEF);
    PASS("pseudo_dd_le");
}

static void test_dq(void) {
    AsmCtx ctx;
    assert(asm_compile(".data\nfoo: dq 0xCAFEBABEDEAD1234\n.text\nhalt\n", &ctx));
    assert(ctx.data.len == 8);
    uint64_t v; memcpy(&v, ctx.data.buf, 8);
    assert(v == 0xCAFEBABEDEAD1234ULL);
    PASS("pseudo_dq_le");
}

static void test_ds(void) {
    AsmCtx ctx;
    assert(asm_compile(".cnst\nmsg: ds \"hi\"\n.text\nhalt\n", &ctx));
    assert(ctx.cnst.len == 2);
    assert(ctx.cnst.buf[0] == 'h' && ctx.cnst.buf[1] == 'i');
    PASS("pseudo_ds_no_nul");
}

static void test_dsz(void) {
    AsmCtx ctx;
    assert(asm_compile(".cnst\nmsg: dsz \"hi\"\n.text\nhalt\n", &ctx));
    assert(ctx.cnst.len == 3);
    assert(ctx.cnst.buf[0] == 'h' && ctx.cnst.buf[1] == 'i' && ctx.cnst.buf[2] == 0);
    PASS("pseudo_dsz_nul_term");
}

static void test_ds_escapes(void) {
    AsmCtx ctx;
    assert(asm_compile(".data\nmsg: ds \"\\n\\t\\\\\"\n.text\nhalt\n", &ctx));
    assert(ctx.data.len == 3);
    assert(ctx.data.buf[0] == '\n');
    assert(ctx.data.buf[1] == '\t');
    assert(ctx.data.buf[2] == '\\');
    PASS("pseudo_ds_escapes");
}

static void test_multi_section_layout(void) {
    // Multiple labels in each section land at correct offsets
    AsmCtx ctx;
    const char *src =
        ".cnst\n"
        "ca: db 1\n"
        "cb: dw 0x200\n"    // offset 1
        ".data\n"
        "da: dd 0\n"
        "db_: dq 0\n"       // offset 4
        ".text\n"
        "entry:\n"
        "    halt\n";
    assert(asm_compile(src, &ctx));

    AsmLabel *ca = asm_label_find(&ctx.lt, "ca");
    AsmLabel *cb = asm_label_find(&ctx.lt, "cb");
    AsmLabel *da = asm_label_find(&ctx.lt, "da");
    AsmLabel *db_ = asm_label_find(&ctx.lt, "db_");
    AsmLabel *entry = asm_label_find(&ctx.lt, "entry");

    assert(ca && ca->section == DVM_SEC_CNST && ca->offset == 0);
    assert(cb && cb->section == DVM_SEC_CNST && cb->offset == 1);
    assert(da && da->section == DVM_SEC_DATA && da->offset == 0);
    assert(db_ && db_->section == DVM_SEC_DATA && db_->offset == 4);
    assert(entry && entry->section == DVM_SEC_CODE && entry->offset == 0);
    assert(ctx.cnst.len == 3);   // 1 byte + 2 bytes
    assert(ctx.data.len == 12);  // 4 bytes + 8 bytes
    PASS("multi_section_layout");
}

static void test_equ_in_data(void) {
    // .equ constants usable in data pseudo-ops
    AsmCtx ctx;
    const char *src =
        ".equ MAGIC, 0xBEEF\n"
        ".data\n"
        "val: dw MAGIC\n"
        ".text\nhalt\n";
    assert(asm_compile(src, &ctx));
    assert(ctx.data.len == 2);
    uint16_t v; memcpy(&v, ctx.data.buf, 2);
    assert(v == 0xBEEF);
    PASS("equ_in_data_pseudo_op");
}

// ─── Symbol flags ─────────────────────────────────────────────────────────────

static void test_global_flag(void) {
    AsmCtx ctx;
    const char *src =
        "global myfunc\n"
        ".text\n"
        "myfunc: halt\n";
    assert(asm_compile(src, &ctx));
    AsmLabel *l = asm_label_find(&ctx.lt, "myfunc");
    assert(l && l->flags == DVM_SYM_GLOBAL && l->section == DVM_SEC_CODE);
    PASS("global_flag_set");
}

static void test_extern_flag(void) {
    AsmCtx ctx;
    const char *src =
        "extern foreign\n"
        ".text\nhalt\n";
    assert(asm_compile(src, &ctx));
    AsmLabel *l = asm_label_find(&ctx.lt, "foreign");
    assert(l && l->flags == DVM_SYM_EXTERN && l->section == DVM_SEC_NONE);
    PASS("extern_flag_set");
}

static void test_asm_to_prog_sym_table(void) {
    // Verify asm_to_prog produces correct symbol table from all three sections
    AsmCtx ctx;
    const char *src =
        "global entry\n"
        "extern ext_fn\n"
        ".cnst\n"
        "pi: dq 0\n"
        ".data\n"
        "count: dq 0\n"
        ".text\n"
        "entry: halt\n";
    assert(asm_compile(src, &ctx));

    DvmProg prog; asm_to_prog(&ctx, &prog);

    // find each expected symbol
    int found_pi=0, found_count=0, found_entry=0, found_ext=0;
    for (size_t i=0; i<prog.nsyms; i++) {
        const DvmSym *s = &prog.syms[i];
        if (!strcmp(s->name,"pi"))    { found_pi=1;    assert(s->section==DVM_SEC_CNST); assert(s->flags==DVM_SYM_LOCAL); }
        if (!strcmp(s->name,"count")) { found_count=1; assert(s->section==DVM_SEC_DATA); }
        if (!strcmp(s->name,"entry")) { found_entry=1; assert(s->flags==DVM_SYM_GLOBAL); assert(s->section==DVM_SEC_CODE); }
        if (!strcmp(s->name,"ext_fn")){ found_ext=1;   assert(s->flags==DVM_SYM_EXTERN); assert(s->section==DVM_SEC_NONE); }
    }
    assert(found_pi && found_count && found_entry && found_ext);
    PASS("asm_to_prog_sym_table");
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(void) {
    printf("─── format round-trip ──────────────────────\n");
    test_rt_minimal();
    test_rt_all_sections();
    test_rt_syms();
    test_rt_rels();
    test_rt_long_sym_name();
    test_rt_large_code();

    printf("─── error paths ────────────────────────────\n");
    test_bad_magic();
    test_truncated_file();

    printf("─── pseudo-op byte layout ──────────────────\n");
    test_db();
    test_dw();
    test_dd();
    test_dq();
    test_ds();
    test_dsz();
    test_ds_escapes();
    test_multi_section_layout();
    test_equ_in_data();

    printf("─── symbol flags ───────────────────────────\n");
    test_global_flag();
    test_extern_flag();
    test_asm_to_prog_sym_table();

    printf("────────────────────────────────────────────\n");
    printf("All binary format tests passed.\n");
    return 0;
}
