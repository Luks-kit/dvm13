#include "vm.h"
#include "asm.h"
#include "dvm_io.h"
#include "linker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage:\n"
        "  %s asm   <file.s> [out.dvm]          assemble to object/executable\n"
        "  %s link  <a.dvm> <b.dvm> ... -o out [--entry sym]  link objects\n"
        "  %s run   <file.dvm>                  load and execute\n"
        "  %s dump  <file.dvm>                  print section/symbol info\n",
        argv0,argv0,argv0,argv0);
    exit(1);
}

static char *read_file(const char *path) {
    FILE *f=fopen(path,"r"); if(!f){perror(path);return NULL;}
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf=malloc((size_t)sz+1);
    if (!buf) goto cleanup;
    if(!fread(buf,1,(size_t)sz,f)) {free(buf); buf = NULL; goto cleanup;} 
    buf[sz]=0;
cleanup: 
    fclose(f);
    return buf;
}

static int cmd_asm(int argc, char **argv) {
    // dvm asm <src> [out]
    if (argc < 1) { fputs("asm: need source file\n",stderr); return 1; }
    const char *src_path = argv[0];
    const char *out_path = (argc>=2) ? argv[1] : "out.dvm";

    char *src=read_file(src_path); if(!src) return 1;
    AsmCtx ctx; if(!asm_compile(src,&ctx)){free(src);return 1;} free(src);

    DvmProg prog; asm_to_prog(&ctx,&prog);
    if(!dvm_write_file(out_path,&prog)) return 1;

    // Count extern symbols
    size_t nexterns=0;
    for (size_t i=0;i<prog.nsyms;i++)
        if (prog.syms[i].flags==DVM_SYM_EXTERN) nexterns++;

    printf("wrote %s  (cnst=%zu data=%zu code=%zu syms=%zu rels=%zu externs=%zu)\n",
           out_path, prog.cnst_len,prog.data_len,prog.code_len,
           prog.nsyms,prog.nrels,nexterns);
    return 0;
}

static int cmd_link(int argc, char **argv) {
    const char *out_path  = "out.dvm";
    const char *entry_sym = NULL;
    const char *inputs[64]; size_t ninputs=0;
    for (int i=0; i<argc; i++) {
        if      (!strcmp(argv[i],"-o"))        { if(i+1<argc) out_path  = argv[++i]; }
        else if (!strcmp(argv[i],"--entry"))   { if(i+1<argc) entry_sym = argv[++i]; }
        else inputs[ninputs++] = argv[i];
    }
    if (!ninputs) { fputs("link: no input files\n",stderr); return 1; }
    if (!dvm_link_files(inputs,ninputs,out_path,entry_sym)) return 1;
    printf("linked %s\n",out_path);
    return 0;
}

static int cmd_run(const char *path) {
    DvmLoaded loaded;
    if (!dvm_load_file(path,&loaded)) {
        fprintf(stderr,"run: failed to load '%s'\n",path); return 1;
    }
    vm_run(loaded.code + loaded.entry_offset, 1<<20);
    dvm_unload(&loaded);
    return 0;
}

static const char *sec_name(DvmSection s) {
    switch(s){ case DVM_SEC_CNST: return "cnst"; case DVM_SEC_DATA: return "data";
               case DVM_SEC_CODE: return "code"; default: return "none"; }
}
static const char *sym_flag(uint8_t f) {
    switch(f){ case DVM_SYM_GLOBAL: return "global"; case DVM_SYM_EXTERN: return "extern";
               default: return "local"; }
}

static int cmd_dump(const char *path) {
    DvmProg prog; if(!dvm_read_file(path,&prog)) return 1;
    printf("file:  %s\n",path);
    printf("cnst:  %zu bytes\ndata:  %zu bytes\ncode:  %zu bytes\n",
           prog.cnst_len,prog.data_len,prog.code_len);
    printf("syms:  %zu\n",prog.nsyms);
    for (size_t i=0;i<prog.nsyms;i++) {
        const DvmSym *s=&prog.syms[i];
        printf("  [%2zu] %-8s %-8s %s+0x%x\n",
               i,sym_flag(s->flags),sec_name(s->section),sec_name(s->section),s->offset);
        printf("       %s\n",s->name);
    }
    printf("entry: code+0x%x%s\n", prog.entry_offset,
           prog.has_entry ? "" : " (default)");
    printf("rels:  %zu\n",prog.nrels);
    for (size_t i=0;i<prog.nrels;i++) {
        const DvmRel *r=&prog.rels[i];
        const char *sname=(r->sym_index<prog.nsyms)?prog.syms[r->sym_index].name:"?";
        printf("  [%2zu] code+0x%04x  →  %s\n",i,r->code_offset,sname);
    }
    dvm_prog_free(&prog);
    return 0;
}

int main(int argc, char **argv) {
    if (argc<3) usage(argv[0]);
    if (!strcmp(argv[1],"asm"))  return cmd_asm (argc-2,argv+2);
    if (!strcmp(argv[1],"link")) return cmd_link(argc-2,argv+2);
    if (!strcmp(argv[1],"run"))  return cmd_run (argv[2]);
    if (!strcmp(argv[1],"dump")) return cmd_dump(argv[2]);
    fprintf(stderr,"dvm: unknown command '%s'\n",argv[1]);
    usage(argv[0]);
}
