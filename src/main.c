#include "vm.h"
#include "asm.h"
#include "dvm_io.h"
#include <asm-generic/errno-base.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s run   <file.dvm>      — load and execute a .dvm binary\n"
        "  %s asm   <file.s> [out]  — assemble source to .dvm (default: out.dvm)\n"
        "  %s dump  <file.dvm>      — print section info\n",
        argv0, argv0, argv0);
    exit(1);
}

// ─── run ──────────────────────────────────────────────────────────────────────

static int cmd_run(const char *path) {
    DvmLoaded loaded;
    if (!dvm_load_file(path, &loaded)) {
        fprintf(stderr, "dvm: failed to load '%s'\n", path);
        return 1;
    }
    vm_run(loaded.code, 1 << 20);  // 1MB stack
    dvm_unload(&loaded);
    return 0;
}

// ─── asm ──────────────────────────────────────────────────────────────────────

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if(fread(buf, 1, (size_t)sz, f) != (size_t)sz) return NULL;
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static int cmd_asm(const char *src_path, const char *out_path) {
    char *src = read_file(src_path);
    if (!src) return 1;

    AsmCtx ctx;
    if (!asm_compile(src, &ctx)) {
        free(src);
        return 1;
    }
    free(src);

    DvmProg prog;
    asm_to_prog(&ctx, &prog);

    if (!dvm_write_file(out_path, &prog)) return 1;

    printf("wrote %s  (cnst=%zu data=%zu code=%zu syms=%zu rels=%zu)\n",
           out_path,
           prog.cnst_len, prog.data_len, prog.code_len,
           prog.nsyms, prog.nrels);
    return 0;
}

// ─── dump ─────────────────────────────────────────────────────────────────────

static const char *sec_name(DvmSection s) {
    switch(s) {
    case DVM_SEC_CNST: return "cnst";
    case DVM_SEC_DATA: return "data";
    case DVM_SEC_CODE: return "code";
    default:           return "?";
    }
}

static int cmd_dump(const char *path) {
    DvmProg prog;
    if (!dvm_read_file(path, &prog)) return 1;

    printf("file:   %s\n", path);
    printf("cnst:   %zu bytes\n", prog.cnst_len);
    printf("data:   %zu bytes\n", prog.data_len);
    printf("code:   %zu bytes\n", prog.code_len);
    printf("syms:   %zu\n", prog.nsyms);
    for (size_t i = 0; i < prog.nsyms; i++) {
        const DvmSym *s = &prog.syms[i];
        printf("  [%zu] %-20s  %s+0x%x\n",
               i, s->name, sec_name(s->section), s->offset);
    }
    printf("rels:   %zu\n", prog.nrels);
    for (size_t i = 0; i < prog.nrels; i++) {
        const DvmRel *r = &prog.rels[i];
        printf("  [%zu] code+0x%04x  →  %s+0x%x\n",
               i, r->code_offset, sec_name(r->section), r->sym_offset);
    }

    dvm_prog_free(&prog);
    return 0;
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    if (argc < 3) usage(argv[0]);

    if (!strcmp(argv[1], "run"))  return cmd_run(argv[2]);
    if (!strcmp(argv[1], "dump")) return cmd_dump(argv[2]);
    if (!strcmp(argv[1], "asm")) {
        const char *out = (argc >= 4) ? argv[3] : "out.dvm";
        return cmd_asm(argv[2], out);
    }

    fprintf(stderr, "dvm: unknown command '%s'\n", argv[1]);
    usage(argv[0]);
}
