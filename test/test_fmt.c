#include "vm.h"
#include "asm.h"
#include "dvm_io.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

#define PASS(name) printf("PASS  %-36s\n", name)
#define FAIL(name, ...) do { \
    fprintf(stderr, "FAIL  %s: ", name); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); \
} while(0)

static uint64_t R[8];  // result sink
static char STORE[128];
static void init_store(void) {
    snprintf(STORE, sizeof(STORE),
        "    mov  di, %llu\n"
        "    mov  [di], ax\n",
        (unsigned long long)(uint64_t)&R[0]);
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void prog_round_trip(const DvmProg *orig, DvmProg *out) {
    // write to a temp file, read back
    const char *tmp = "/tmp/dvm_test_rt.dvm";
    assert(dvm_write_file(tmp, orig));
    assert(dvm_read_file(tmp, out));
}

// ─── Format: write / read round-trip ─────────────────────────────────────────

static void test_roundtrip_empty_sections(void) {
    DvmProg orig = {0};
    uint8_t code[] = { OP_HALT };
    orig.code = code; orig.code_len = 1;

    DvmProg rt;
    prog_round_trip(&orig, &rt);

    assert(rt.code_len == 1 && rt.code[0] == OP_HALT);
    assert(rt.cnst_len == 0 && rt.data_len == 0);
    assert(rt.nsyms == 0 && rt.nrels == 0);
    dvm_prog_free(&rt);
    PASS("roundtrip_empty_sections");
}

static void test_roundtrip_with_data(void) {
    DvmProg orig = {0};
    uint8_t cnst_data[] = { 0xAA, 0xBB, 0xCC };
    uint8_t data_data[] = { 0x11, 0x22 };
    uint8_t code[]      = { OP_HALT };
    orig.cnst = cnst_data; orig.cnst_len = 3;
    orig.data = data_data; orig.data_len = 2;
    orig.code = code;      orig.code_len = 1;

    // one symbol, one reloc
    orig.nsyms = 1;
    snprintf(orig.syms[0].name, DVM_SYM_NAME, "my_const");
    orig.syms[0].section = DVM_SEC_CNST;
    orig.syms[0].offset  = 1;

    orig.nrels = 1;
    orig.rels[0].code_offset = 0;
    orig.rels[0].section     = DVM_SEC_CNST;
    orig.rels[0].sym_offset  = 1;

    DvmProg rt;
    prog_round_trip(&orig, &rt);

    assert(rt.cnst_len == 3 && rt.cnst[0]==0xAA && rt.cnst[2]==0xCC);
    assert(rt.data_len == 2 && rt.data[1]==0x22);
    assert(rt.nsyms == 1 && !strcmp(rt.syms[0].name,"my_const"));
    assert(rt.syms[0].section == DVM_SEC_CNST && rt.syms[0].offset == 1);
    assert(rt.nrels == 1 && rt.rels[0].code_offset == 0);
    dvm_prog_free(&rt);
    PASS("roundtrip_with_data");
}

static void test_roundtrip_many_syms(void) {
    DvmProg orig = {0};
    uint8_t code[] = { OP_HALT };
    orig.code = code; orig.code_len = 1;
    orig.nsyms = 10;
    for (int i = 0; i < 10; i++) {
        snprintf(orig.syms[i].name, DVM_SYM_NAME, "sym_%d", i);
        orig.syms[i].section = DVM_SEC_DATA;
        orig.syms[i].offset  = (uint32_t)(i * 8);
    }
    DvmProg rt;
    prog_round_trip(&orig, &rt);
    assert(rt.nsyms == 10);
    for (int i = 0; i < 10; i++) {
        char expected[32]; snprintf(expected, 32, "sym_%d", i);
        assert(!strcmp(rt.syms[i].name, expected));
        assert(rt.syms[i].offset == (uint32_t)(i*8));
    }
    dvm_prog_free(&rt);
    PASS("roundtrip_many_syms");
}

static void test_bad_magic(void) {
    const char *tmp = "/tmp/dvm_test_badmagic.dvm";
    FILE *f = fopen(tmp, "wb");
    fwrite("BAAD", 1, 4, f);
    fclose(f);
    DvmProg prog;
    FILE *old = stderr; stderr = fopen("/dev/null","w");
    int ok = dvm_read_file(tmp, &prog);
    fclose(stderr); stderr = old;
    assert(!ok);
    PASS("bad_magic_rejected");
}

// ─── Loader: relocations applied ─────────────────────────────────────────────

static void test_loader_cnst_ref(void) {
    // Assemble a program that reads a constant from .cnst via a data label ref
    // and writes it to R[0] via SYSCALL-free method (direct pointer write)
    //
    // .cnst
    //   val: dq 0x1234
    // .text
    //   mov ax, val       ; ax = address of val (relocated)
    //   mov ax, [ax]      ; ax = *val = 0x1234
    //   mov di, &R[0]
    //   mov [di], ax
    //   halt

    char src[512];
    snprintf(src, sizeof(src),
        ".cnst\n"
        "val: dq 0x1234\n"
        ".text\n"
        "    mov  ax, val\n"
        "    mov  ax, [ax]\n"
        "    mov  di, %llu\n"
        "    mov  [di], ax\n"
        "    halt\n",
        (unsigned long long)(uint64_t)&R[0]);

    AsmCtx ctx;
    assert(asm_compile(src, &ctx));

    DvmProg prog;
    asm_to_prog(&ctx, &prog);
    assert(prog.nrels == 1);
    assert(prog.cnst_len == 8);

    DvmLoaded loaded;
    assert(dvm_load(&prog, &loaded));

    memset(R, 0, sizeof(R));
    vm_run(loaded.code, 64*1024);
    assert(R[0] == 0x1234);
    dvm_unload(&loaded);
    PASS("loader_cnst_ref");
}

static void test_loader_data_ref(void) {
    // .data: mutable value, write to it and read back
    char src[512];
    snprintf(src, sizeof(src),
        ".data\n"
        "counter: dq 0\n"
        ".text\n"
        "    mov  bx, counter\n"  // bx = address of counter
        "    mov  ax, 99\n"
        "    mov  [bx], ax\n"     // counter = 99
        "    mov  ax, [bx]\n"     // ax = 99
        "    mov  di, %llu\n"
        "    mov  [di], ax\n"
        "    halt\n",
        (unsigned long long)(uint64_t)&R[0]);

    AsmCtx ctx;
    assert(asm_compile(src, &ctx));
    assert(ctx.nrels == 1);

    DvmProg prog; asm_to_prog(&ctx, &prog);
    DvmLoaded loaded; assert(dvm_load(&prog, &loaded));

    memset(R, 0, sizeof(R));
    vm_run(loaded.code, 64*1024);
    assert(R[0] == 99);
    dvm_unload(&loaded);
    PASS("loader_data_ref");
}

static void test_loader_string_in_cnst(void) {
    // .cnst holds a string, code writes it to stdout via syscall
    // then checks return value (bytes written) in R[0]
    char src[512];
    snprintf(src, sizeof(src),
        ".cnst\n"
        "msg:    dsz \"hello\"\n"
        "msglen: dq 5\n"
        ".text\n"
        "    mov  ax, %llu\n"   // DVM_SYS_WRITE
        "    mov  bx, 1\n"      // stdout
        "    mov  cx, msg\n"    // buf (relocated)
        // actually easier: just use immediate 5
        "    mov  dx, 5\n"
        "    syscall\n"
        "    mov  di, %llu\n"
        "    mov  [di], ax\n"
        "    halt\n",
        (unsigned long long)DVM_SYS_WRITE,
        (unsigned long long)(uint64_t)&R[0]);

    AsmCtx ctx;
    assert(asm_compile(src, &ctx));

    DvmProg prog; asm_to_prog(&ctx, &prog);
    DvmLoaded loaded; assert(dvm_load(&prog, &loaded));

    memset(R, 0, sizeof(R));
    vm_run(loaded.code, 64*1024);
    assert(R[0] == 5 && "write should return 5");
    dvm_unload(&loaded);
    PASS("loader_string_in_cnst");
}

static void test_loader_multiple_rels(void) {
    // Two data labels, two relocations
    char src[512];
    snprintf(src, sizeof(src),
        ".data\n"
        "a: dq 10\n"
        "b: dq 32\n"
        ".text\n"
        "    mov  bx, a\n"   // reloc 0
        "    mov  cx, b\n"   // reloc 1
        "    mov  ax, [bx]\n"
        "    mov  dx, [cx]\n"
        "    add  ax, dx\n"
        "    mov  di, %llu\n"
        "    mov  [di], ax\n"
        "    halt\n",
        (unsigned long long)(uint64_t)&R[0]);

    AsmCtx ctx;
    assert(asm_compile(src, &ctx));
    assert(ctx.nrels == 2);

    DvmProg prog; asm_to_prog(&ctx, &prog);
    DvmLoaded loaded; assert(dvm_load(&prog, &loaded));

    memset(R, 0, sizeof(R));
    vm_run(loaded.code, 64*1024);
    assert(R[0] == 42);
    dvm_unload(&loaded);
    PASS("loader_multiple_rels");
}

// ─── Assembler section directives ────────────────────────────────────────────

static void test_section_data_pseudo_ops(void) {
    // Verify .data pseudo-ops produce the right bytes
    AsmCtx ctx;
    const char *src =
        ".data\n"
        "a: db 0x42\n"
        "b: dw 0x1234\n"
        "c: dd 0xDEAD\n"
        "d: dq 0xCAFE\n"
        ".text\n"
        "    halt\n";
    assert(asm_compile(src, &ctx));
    assert(ctx.data.len == 1+2+4+8);

    uint8_t *d = ctx.data.buf;
    assert(d[0] == 0x42);
    uint16_t w; memcpy(&w, d+1, 2); assert(w == 0x1234);
    uint32_t dw; memcpy(&dw, d+3, 4); assert(dw == 0xDEAD);
    uint64_t qw; memcpy(&qw, d+7, 8); assert(qw == 0xCAFE);
    PASS("section_data_pseudo_ops");
}

static void test_section_dsz(void) {
    AsmCtx ctx;
    const char *src =
        ".cnst\n"
        "msg: dsz \"hi\"\n"
        ".text\n"
        "    halt\n";
    assert(asm_compile(src, &ctx));
    assert(ctx.cnst.len == 3);  // 'h','i','\0'
    assert(ctx.cnst.buf[0]=='h' && ctx.cnst.buf[1]=='i' && ctx.cnst.buf[2]==0);
    PASS("section_dsz");
}

static void test_section_ds_no_nul(void) {
    AsmCtx ctx;
    const char *src =
        ".data\n"
        "msg: ds \"ab\"\n"
        ".text\n"
        "    halt\n";
    assert(asm_compile(src, &ctx));
    assert(ctx.data.len == 2);
    assert(ctx.data.buf[0]=='a' && ctx.data.buf[1]=='b');
    PASS("section_ds_no_nul");
}

static void test_labels_across_sections(void) {
    // Labels defined in each section, accessible from code via relocs
    AsmCtx ctx;
    char src[256];
    snprintf(src, sizeof(src),
        ".cnst\n"
        "c_val: dq 1\n"
        ".data\n"
        "d_val: dq 2\n"
        ".text\n"
        "    mov  ax, c_val\n"
        "    mov  bx, d_val\n"
        "    halt\n");
    assert(asm_compile(src, &ctx));
    assert(ctx.nrels == 2);
    // c_val is cnst, d_val is data
    AsmLabel *lc = asm_label_find(&ctx.lt, "c_val");
    AsmLabel *ld = asm_label_find(&ctx.lt, "d_val");
    assert(lc && lc->section == DVM_SEC_CNST && lc->offset == 0);
    assert(ld && ld->section == DVM_SEC_DATA && ld->offset == 0);
    PASS("labels_across_sections");
}

// ─── asm_to_prog symbol table ─────────────────────────────────────────────────

static void test_asm_to_prog_syms(void) {
    AsmCtx ctx;
    const char *src =
        ".cnst\n"
        "pi: dq 0\n"
        ".data\n"
        "count: dq 0\n"
        ".text\n"
        "entry:\n"
        "    halt\n";
    assert(asm_compile(src, &ctx));

    DvmProg prog; asm_to_prog(&ctx, &prog);
    assert(prog.nsyms >= 3);

    // find expected symbols
    int found_pi=0, found_count=0, found_entry=0;
    for (size_t i=0; i<prog.nsyms; i++) {
        if (!strcmp(prog.syms[i].name,"pi"))    { found_pi=1;    assert(prog.syms[i].section==DVM_SEC_CNST); }
        if (!strcmp(prog.syms[i].name,"count")) { found_count=1; assert(prog.syms[i].section==DVM_SEC_DATA); }
        if (!strcmp(prog.syms[i].name,"entry")) { found_entry=1; assert(prog.syms[i].section==DVM_SEC_CODE); }
    }
    assert(found_pi && found_count && found_entry);
    PASS("asm_to_prog_syms");
}

// ─── File I/O: asm → file → load → run ───────────────────────────────────────

static void test_file_asm_run(void) {
    // Full pipeline: assemble → write file → read file → load → run
    char src[512];
    snprintf(src, sizeof(src),
        ".cnst\n"
        "magic: dq 0xBEEF\n"
        ".text\n"
        "    mov  ax, magic\n"
        "    mov  ax, [ax]\n"
        "    mov  di, %llu\n"
        "    mov  [di], ax\n"
        "    halt\n",
        (unsigned long long)(uint64_t)&R[0]);

    AsmCtx ctx;
    assert(asm_compile(src, &ctx));

    DvmProg prog; asm_to_prog(&ctx, &prog);
    const char *tmp = "/tmp/dvm_test_pipeline.dvm";
    assert(dvm_write_file(tmp, &prog));

    DvmLoaded loaded;
    assert(dvm_load_file(tmp, &loaded));

    memset(R, 0, sizeof(R));
    vm_run(loaded.code, 64*1024);
    assert(R[0] == 0xBEEF);
    dvm_unload(&loaded);
    PASS("file_asm_run_pipeline");
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(void) {
    init_store(); (void)STORE;

    printf("─── format round-trip ──────────────────────\n");
    test_roundtrip_empty_sections();
    test_roundtrip_with_data();
    test_roundtrip_many_syms();
    test_bad_magic();

    printf("─── loader / relocations ───────────────────\n");
    test_loader_cnst_ref();
    test_loader_data_ref();
    test_loader_string_in_cnst();
    test_loader_multiple_rels();

    printf("─── assembler sections ─────────────────────\n");
    test_section_data_pseudo_ops();
    test_section_dsz();
    test_section_ds_no_nul();
    test_labels_across_sections();
    test_asm_to_prog_syms();

    printf("─── file pipeline ──────────────────────────\n");
    test_file_asm_run();

    printf("────────────────────────────────────────────\n");
    printf("All format tests passed.\n");
    return 0;
}
