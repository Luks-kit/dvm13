#include "vm.h"
#include "asm.h"
#include "dvm_io.h"
#include "linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

// ─── Path resolution ──────────────────────────────────────────────────────────
// __FILE__ gives us the path to this source file at compile time.
// We strip back to the project root (two directories up from test/).

static char _proj_root[4096];
static char _asm_dir[4096];

static void init_paths(void) {
    // Resolve __FILE__ to an absolute path, then strip to project root.
    // __FILE__ may be relative (e.g. "test/test_asm_files.c") so use realpath.
    char abs[4096] = {0};
    if (!realpath(__FILE__, abs)) {
        // fallback: assume cwd is project root
        snprintf(_proj_root, 4096, ".");
        snprintf(_asm_dir,   4096,   "./test/asm");
        return;
    }
    // abs = /path/to/dvm/test/test_asm_files.c
    char *slash = strrchr(abs, '/');
    if (slash) *slash = 0;              // strip filename → .../test
    slash = strrchr(abs, '/');
    if (slash) *slash = 0;              // strip /test → project root
    snprintf(_proj_root, 4096, "%s", abs);
    snprintf(_asm_dir,   4096,   "%s/test/asm", abs);
}

// Build a path relative to the project root
static char _path_buf[4096];
static const char *proj_path(const char *rel) {
    snprintf(_path_buf, 4096, "%s/%s", _proj_root, rel);
    return _path_buf;
}

static const char *asm_path(const char *name) {
    snprintf(_path_buf, 4096, "%s/%s", _asm_dir, name);
    return _path_buf;
}

// ─── Infra ────────────────────────────────────────────────────────────────────

#define PASS(name) printf("PASS  %-36s\n", name)
#define FAIL(name, ...) do { \
    fprintf(stderr, "FAIL  %s: ", name); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); exit(1); \
} while(0)

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if(!fread(buf,1,(size_t)sz,f)) {free(buf);fclose(f); return NULL;}
    buf[sz] = 0;
    fclose(f);
    return buf;
}

// Assemble source file → DvmProg (heap-allocated sections via asm_to_prog copy)
static int asm_file(const char *path, DvmProg *out) {
    char *src = read_file(path);
    if (!src) return 0;

    AsmCtx ctx;
    int ok = asm_compile(src, &ctx);
    free(src);
    if (!ok) return 0;

    // asm_to_prog gives pointers into ctx's stack-local bufs; we need copies
    DvmProg tmp; asm_to_prog(&ctx, &tmp);

    memset(out, 0, sizeof(*out));
    if (tmp.cnst_len) {
        out->cnst = malloc(tmp.cnst_len);
        memcpy(out->cnst, tmp.cnst, tmp.cnst_len);
    }
    if (tmp.data_len) {
        out->data = malloc(tmp.data_len);
        memcpy(out->data, tmp.data, tmp.data_len);
    }
    if (tmp.code_len) {
        out->code = malloc(tmp.code_len);
        memcpy(out->code, tmp.code, tmp.code_len);
    }
    out->cnst_len = tmp.cnst_len;
    out->data_len = tmp.data_len;
    out->code_len = tmp.code_len;
    out->nsyms    = tmp.nsyms;
    out->nrels    = tmp.nrels;
    memcpy(out->syms, tmp.syms, tmp.nsyms * sizeof(DvmSym));
    memcpy(out->rels, tmp.rels, tmp.nrels  * sizeof(DvmRel));
    return 1;
}

// Find a symbol by name in a loaded program's symbol table.
// Returns the symbol's offset within its section, or -1 if not found.
static int64_t sym_offset(const DvmProg *prog, const char *name, DvmSection *sec_out) {
    for (size_t i = 0; i < prog->nsyms; i++) {
        if (!strcmp(prog->syms[i].name, name)) {
            if (sec_out) *sec_out = prog->syms[i].section;
            return (int64_t)prog->syms[i].offset;
        }
    }
    return -1;
}

// Get a pointer into the loaded DATA section at a symbol's offset.
static uint64_t *result_ptr(const DvmLoaded *l, const DvmProg *prog) {
    DvmSection sec;
    int64_t off = sym_offset(prog, "result", &sec);
    if (off < 0) return NULL;
    switch (sec) {
    case DVM_SEC_DATA: return (uint64_t*)(l->data + off);
    case DVM_SEC_CNST: return (uint64_t*)(l->cnst + off);
    case DVM_SEC_CODE: return (uint64_t*)(l->code + off);
    default:           return NULL;
    }
}

// Assemble, load, run, read result, unload.
// Expects a `result: dq 0` in .data of the source file.
static uint64_t run_asm(const char *path) {
    DvmProg prog;
    if (!asm_file(path, &prog)) {
        fprintf(stderr, "run_asm: assemble failed: %s\n", path);
        exit(1);
    }

    DvmLoaded loaded;
    if (!dvm_load(&prog, &loaded)) {
        fprintf(stderr, "run_asm: load failed: %s\n", path);
        dvm_prog_free(&prog); exit(1);
    }

    // Snapshot result pointer before run (it lives in loaded.data)
    DvmSection sec;
    int64_t roff = sym_offset(&prog, "result", &sec);
    if (roff < 0 || sec != DVM_SEC_DATA) {
        fprintf(stderr, "run_asm: no 'result' in .data: %s\n", path);
        dvm_unload(&loaded); dvm_prog_free(&prog); exit(1);
    }
    uint64_t *rp = (uint64_t*)(loaded.data + roff);

    vm_run(loaded.code, 256*1024);

    uint64_t val = *rp;
    dvm_unload(&loaded);
    dvm_prog_free(&prog);
    return val;
}

// ─── Single-object tests ──────────────────────────────────────────────────────

#define ASM_TEST(name, path, expected) do { \
    uint64_t got = run_asm(path); \
    if (got != (uint64_t)(expected)) \
        FAIL(name, "got 0x%llX want 0x%llX", \
             (unsigned long long)got, (unsigned long long)(expected)); \
    PASS(name); \
} while(0)

static void test_arith(void) {
    ASM_TEST("arith", asm_path("arith.asm"), 42);
}

static void test_bitwise(void) {
    // ~0xFF00 & 0xFFFF = 0x00FF,  0x00FF | 0x002A = 0x00FF (0x2A ⊆ 0xFF)
    ASM_TEST("bitwise", asm_path("bitwise.asm"), 0x00FF);
}

static void test_loop(void) {
    ASM_TEST("loop", asm_path("loop.asm"), 55);
}

static void test_fib(void) {
    ASM_TEST("fib", asm_path("fib.asm"), 55);
}

static void test_data_ref(void) {
    ASM_TEST("data_ref", asm_path("data_ref.asm"), 300);
}

static void test_equ(void) {
    // 80 * 24 - 2 = 1918
    ASM_TEST("equ", asm_path("equ.asm"), 1918);
}

static void test_throw(void) {
    ASM_TEST("throw", asm_path("throw.asm"), 0xCAFE);
}

static void test_float(void) {
    ASM_TEST("float", asm_path("float.asm"), 42);
}

// ─── Linker test ──────────────────────────────────────────────────────────────

static void test_link(void) {
    DvmProg lib, main_obj, linked;

    if (!asm_file(asm_path("math_lib.asm"), &lib)) {
        FAIL("link", "failed to assemble math_lib.asm");
    }
    if (!asm_file(asm_path("linker_main.asm"), &main_obj)) {
        FAIL("link", "failed to assemble linker_main.asm");
    }

    // Verify that linker_main has unresolved externs before linking
    int found_extern = 0;
    for (size_t i = 0; i < main_obj.nsyms; i++)
        if (main_obj.syms[i].flags == DVM_SYM_EXTERN) found_extern++;
    if (!found_extern)
        FAIL("link", "linker_main.asm should have extern symbols");

    const DvmProg objs[2] = { main_obj, lib };
    if (!dvm_link(objs, 2, &linked)) {
        FAIL("link", "dvm_link failed");
    }

    // Verify no unresolved externs remain
    for (size_t i = 0; i < linked.nsyms; i++) {
        if (linked.syms[i].flags == DVM_SYM_EXTERN &&
            linked.syms[i].section == DVM_SEC_NONE)
            FAIL("link", "unresolved extern '%s' after link", linked.syms[i].name);
    }

    DvmLoaded loaded;
    if (!dvm_load(&linked, &loaded))
        FAIL("link", "failed to load linked program");

    // result is in DATA section of linked output
    DvmSection sec;
    int64_t roff = sym_offset(&linked, "result", &sec);
    if (roff < 0 || sec != DVM_SEC_DATA)
        FAIL("link", "no 'result' symbol in linked .data");
    uint64_t *rp = (uint64_t*)(loaded.data + roff);

    vm_run(loaded.code, 256*1024);
    uint64_t got = *rp;

    dvm_unload(&loaded);
    dvm_prog_free(&linked);
    dvm_prog_free(&lib);
    dvm_prog_free(&main_obj);

    if (got != 37)
        FAIL("link", "got %llu want 37", (unsigned long long)got);
    PASS("link_multi_object");
}

// ─── Linker error cases ───────────────────────────────────────────────────────

static void test_link_unresolved_extern(void) {
    // An object with an extern that has no matching global → link fails
    const char *src =
        "extern missing_sym\n"
        ".data\n"
        "result: dq 0\n"
        ".text\n"
        "    mov  ax, missing_sym\n"
        "    halt\n";

    AsmCtx ctx; assert(asm_compile(src, &ctx));
    DvmProg tmp; asm_to_prog(&ctx, &tmp);
    DvmProg obj;
    memset(&obj,0,sizeof(obj));
    obj.code=malloc(tmp.code_len); memcpy(obj.code,tmp.code,tmp.code_len); obj.code_len=tmp.code_len;
    obj.data=malloc(tmp.data_len); memcpy(obj.data,tmp.data,tmp.data_len); obj.data_len=tmp.data_len;
    obj.nsyms=tmp.nsyms; obj.nrels=tmp.nrels;
    memcpy(obj.syms,tmp.syms,tmp.nsyms*sizeof(DvmSym));
    memcpy(obj.rels,tmp.rels,tmp.nrels*sizeof(DvmRel));

    DvmProg out;
    FILE *old = stderr; stderr = fopen("/dev/null","w");
    int ok = dvm_link(&obj, 1, &out);
    fclose(stderr); stderr = old;

    assert(!ok && "link with unresolved extern should fail");
    dvm_prog_free(&obj);
    PASS("link_unresolved_extern_fails");
}

static void test_link_duplicate_global(void) {
    // Two objects both define global `foo` → link fails
    const char *src_a =
        ".text\n"
        "global foo\n"
        "foo: halt\n";
    const char *src_b =
        ".text\n"
        "global foo\n"
        "foo: halt\n";

    AsmCtx ca, cb; DvmProg ta, tb, oa, ob;
    assert(asm_compile(src_a,&ca)); asm_to_prog(&ca,&ta);
    assert(asm_compile(src_b,&cb)); asm_to_prog(&cb,&tb);

    // Copy to heap
    oa=(DvmProg){0}; ob=(DvmProg){0};
    oa.code=malloc(ta.code_len); memcpy(oa.code,ta.code,ta.code_len); oa.code_len=ta.code_len;
    ob.code=malloc(tb.code_len); memcpy(ob.code,tb.code,tb.code_len); ob.code_len=tb.code_len;
    oa.nsyms=ta.nsyms; ob.nsyms=tb.nsyms;
    memcpy(oa.syms,ta.syms,ta.nsyms*sizeof(DvmSym));
    memcpy(ob.syms,tb.syms,tb.nsyms*sizeof(DvmSym));

    const DvmProg objs[2]={oa,ob};
    DvmProg out;
    FILE *old=stderr; stderr=fopen("/dev/null","w");
    int ok=dvm_link(objs,2,&out);
    fclose(stderr); stderr=old;

    assert(!ok && "duplicate global should fail");
    dvm_prog_free(&oa); dvm_prog_free(&ob);
    PASS("link_duplicate_global_fails");
}

// ─── Round-trip: asm → write → read → load → run ─────────────────────────────

static void test_file_roundtrip(void) {
    const char *src =
        ".cnst\n"
        "magic: dq 0xBEEF\n"
        ".data\n"
        "result: dq 0\n"
        ".text\n"
        "    mov  ax, magic\n"
        "    mov  ax, [ax]\n"
        "    mov  bx, result\n"
        "    mov  [bx], ax\n"
        "    halt\n";

    AsmCtx ctx; assert(asm_compile(src,&ctx));
    DvmProg tmp; asm_to_prog(&ctx,&tmp);

    // Copy to heap for write
    DvmProg prog={0};
    prog.cnst=malloc(tmp.cnst_len); memcpy(prog.cnst,tmp.cnst,tmp.cnst_len); prog.cnst_len=tmp.cnst_len;
    prog.data=malloc(tmp.data_len); memcpy(prog.data,tmp.data,tmp.data_len); prog.data_len=tmp.data_len;
    prog.code=malloc(tmp.code_len); memcpy(prog.code,tmp.code,tmp.code_len); prog.code_len=tmp.code_len;
    prog.nsyms=tmp.nsyms; prog.nrels=tmp.nrels;
    memcpy(prog.syms,tmp.syms,tmp.nsyms*sizeof(DvmSym));
    memcpy(prog.rels,tmp.rels,tmp.nrels*sizeof(DvmRel));

    const char *tmp_path="/tmp/dvm_test_rt2.dvm";
    assert(dvm_write_file(tmp_path,&prog));

    DvmProg loaded_prog; assert(dvm_read_file(tmp_path,&loaded_prog));
    DvmLoaded loaded;    assert(dvm_load(&loaded_prog,&loaded));

    DvmSection sec;
    int64_t roff=sym_offset(&loaded_prog,"result",&sec);
    assert(roff>=0 && sec==DVM_SEC_DATA);
    uint64_t *rp=(uint64_t*)(loaded.data+roff);

    vm_run(loaded.code,64*1024);
    uint64_t got=*rp;

    dvm_unload(&loaded);
    dvm_prog_free(&loaded_prog);
    dvm_prog_free(&prog);

    if (got!=0xBEEF)
        FAIL("file_roundtrip","got 0x%llX want 0xBEEF",(unsigned long long)got);
    PASS("file_roundtrip");
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(void) {
    init_paths();
    printf("─── single-object .asm tests ───────────────\n");
    test_arith();
    test_bitwise();
    test_loop();
    test_fib();
    test_data_ref();
    test_equ();
    test_throw();
    test_float();

    printf("─── linker ─────────────────────────────────\n");
    test_link();
    test_link_unresolved_extern();
    test_link_duplicate_global();

    printf("─── file round-trip ────────────────────────\n");
    test_file_roundtrip();

    printf("────────────────────────────────────────────\n");
    printf("All .asm file tests passed.\n");
    return 0;
}
