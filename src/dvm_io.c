#include "dvm_io.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Write ────────────────────────────────────────────────────────────────────

static int fw_u8 (FILE *f, uint8_t  v) { return fwrite(&v,1,1,f)==1; }
static int fw_u32(FILE *f, uint32_t v) { return fwrite(&v,4,1,f)==1; }
static int fw_tag(FILE *f, const char *t) { return fwrite(t,4,1,f)==1; }

static int fw_sec(FILE *f, const char *tag, const uint8_t *d, size_t len) {
    static const uint8_t _empty = 0;
    return fw_tag(f,tag)
        && fw_u32(f,(uint32_t)len)
        && fwrite(len ? d : &_empty, 1, len, f) == len;
}

int dvm_write(FILE *f, const DvmProg *p) {
    if (fwrite(DVM_MAGIC,1,4,f)!=4) return 0;
    if (!fw_sec(f,DVM_TAG_CNST,p->cnst,p->cnst_len)) return 0;
    if (!fw_sec(f,DVM_TAG_DATA,p->data,p->data_len)) return 0;
    if (!fw_sec(f,DVM_TAG_CODE,p->code,p->code_len)) return 0;

    // SYMS
    if (!fw_tag(f,DVM_TAG_SYMS) || !fw_u32(f,(uint32_t)p->nsyms)) return 0;
    for (size_t i=0; i<p->nsyms; i++) {
        const DvmSym *s=&p->syms[i];
        uint8_t nl=(uint8_t)strlen(s->name);
        if (!fw_u8(f,s->flags))    return 0;
        if (!fw_u8(f,(uint8_t)s->section)) return 0;
        if (!fw_u32(f,s->offset))  return 0;
        if (!fw_u8(f,nl))          return 0;
        if (fwrite(s->name,1,nl,f)!=nl) return 0;
    }

    // RELS
    if (!fw_tag(f,DVM_TAG_RELS) || !fw_u32(f,(uint32_t)p->nrels)) return 0;
    for (size_t i=0; i<p->nrels; i++) {
        if (!fw_u32(f,p->rels[i].code_offset)) return 0;
        if (!fw_u32(f,p->rels[i].sym_index))   return 0;
    }

    return fw_tag(f,DVM_TAG_END);
}

int dvm_write_file(const char *path, const DvmProg *prog) {
    FILE *f=fopen(path,"wb");
    if (!f) { perror(path); return 0; }
    int ok=dvm_write(f,prog);
    fclose(f);
    if (!ok) fprintf(stderr,"dvm_write: error writing %s\n",path);
    return ok;
}

// ─── Read ─────────────────────────────────────────────────────────────────────

static int fr_u8 (FILE *f, uint8_t  *v) { return fread(v,1,1,f)==1; }
static int fr_u32(FILE *f, uint32_t *v) { return fread(v,4,1,f)==1; }

static int fr_expect_tag(FILE *f, const char *want) {
    char got[5]={0};
    if (fread(got,1,4,f)!=4) return 0;
    if (memcmp(got,want,4)) {
        fprintf(stderr,"dvm: expected tag '%.4s', got '%.4s'\n",want,got);
        return 0;
    }
    return 1;
}

static uint8_t *fr_section(FILE *f, size_t *out_len) {
    uint32_t sz;
    if (!fr_u32(f,&sz)) return NULL;
    *out_len = sz;
    if (!sz) return calloc(1,1);
    uint8_t *buf=malloc(sz);
    if (!buf || fread(buf,1,sz,f)!=sz) { free(buf); return NULL; }
    return buf;
}

int dvm_read(FILE *f, DvmProg *prog) {
    memset(prog,0,sizeof(*prog));

    char magic[4];
    if (fread(magic,1,4,f)!=4 || memcmp(magic,DVM_MAGIC,4)) {
        fputs("dvm: bad magic\n",stderr); return 0;
    }

    if (!fr_expect_tag(f,DVM_TAG_CNST)) return 0;
    if (!(prog->cnst=fr_section(f,&prog->cnst_len))) return 0;

    if (!fr_expect_tag(f,DVM_TAG_DATA)) return 0;
    if (!(prog->data=fr_section(f,&prog->data_len))) return 0;

    if (!fr_expect_tag(f,DVM_TAG_CODE)) return 0;
    if (!(prog->code=fr_section(f,&prog->code_len))) return 0;

    if (!fr_expect_tag(f,DVM_TAG_SYMS)) return 0;
    uint32_t nsyms;
    if (!fr_u32(f,&nsyms) || nsyms>DVM_MAX_SYMS) return 0;
    prog->nsyms=nsyms;
    for (uint32_t i=0;i<nsyms;i++) {
        DvmSym *s=&prog->syms[i];
        uint8_t sec, nl;
        if (!fr_u8(f,&s->flags))   return 0;
        if (!fr_u8(f,&sec))        return 0;
        if (!fr_u32(f,&s->offset)) return 0;
        if (!fr_u8(f,&nl))         return 0;
        s->section=(DvmSection)sec;
        if (fread(s->name,1,nl,f)!=nl) return 0;
        s->name[nl]=0;
    }

    if (!fr_expect_tag(f,DVM_TAG_RELS)) return 0;
    uint32_t nrels;
    if (!fr_u32(f,&nrels) || nrels>DVM_MAX_RELS) return 0;
    prog->nrels=nrels;
    for (uint32_t i=0;i<nrels;i++) {
        if (!fr_u32(f,&prog->rels[i].code_offset)) return 0;
        if (!fr_u32(f,&prog->rels[i].sym_index))   return 0;
    }

    return fr_expect_tag(f,DVM_TAG_END);
}

int dvm_read_file(const char *path, DvmProg *prog) {
    FILE *f=fopen(path,"rb");
    if (!f) { perror(path); return 0; }
    int ok=dvm_read(f,prog);
    fclose(f);
    return ok;
}

void dvm_prog_free(DvmProg *prog) {
    free(prog->cnst); prog->cnst=NULL;
    free(prog->data); prog->data=NULL;
    free(prog->code); prog->code=NULL;
}

// ─── Load ─────────────────────────────────────────────────────────────────────

int dvm_load(const DvmProg *prog, DvmLoaded *out) {
    memset(out,0,sizeof(*out));
    out->cnst_len=prog->cnst_len;
    out->data_len=prog->data_len;
    out->code_len=prog->code_len;

    if (prog->cnst_len) {
        out->cnst=dvm_mmap(prog->cnst_len, DVM_MMAP_RW);
        memcpy(out->cnst,prog->cnst,prog->cnst_len);
    }
    if (prog->data_len) {
        out->data=dvm_mmap(prog->data_len, DVM_MMAP_RW);
        memcpy(out->data,prog->data,prog->data_len);
    }
    if (!prog->code_len) { fputs("dvm_load: empty CODE\n",stderr); return 0; }
    out->code=dvm_mmap(prog->code_len, DVM_MMAP_RW);
    memcpy(out->code,prog->code,prog->code_len);

    // Section base pointers for relocation
    const uint8_t *bases[3] = {
        [DVM_SEC_CNST]=out->cnst,
        [DVM_SEC_DATA]=out->data,
        [DVM_SEC_CODE]=out->code,
    };

    // Apply relocations
    for (size_t i=0; i<prog->nrels; i++) {
        const DvmRel *r=&prog->rels[i];
        if (r->sym_index >= prog->nsyms) {
            fprintf(stderr,"dvm_load: reloc[%zu] sym_index %u out of range\n",i,r->sym_index);
            dvm_unload(out); return 0;
        }
        const DvmSym *s=&prog->syms[r->sym_index];
        if (s->flags==DVM_SYM_EXTERN || s->section==DVM_SEC_NONE) {
            fprintf(stderr,"dvm_load: unresolved extern '%s'\n",s->name);
            dvm_unload(out); return 0;
        }
        if ((unsigned)s->section>2 || !bases[s->section]) {
            fprintf(stderr,"dvm_load: reloc '%s' references empty section\n",s->name);
            dvm_unload(out); return 0;
        }
        uint64_t target=(uint64_t)(bases[s->section]+s->offset);
        memcpy(out->code+r->code_offset,&target,8);
    }

    // Protect
    if (out->cnst) dvm_mprotect(out->cnst,out->cnst_len,DVM_MMAP_READ);
    dvm_mprotect(out->code,out->code_len,DVM_MMAP_READ|DVM_MMAP_EXEC);
    return 1;
}

void dvm_unload(DvmLoaded *l) {
    if (l->cnst) { dvm_munmap(l->cnst,l->cnst_len); l->cnst=NULL; }
    if (l->data) { dvm_munmap(l->data,l->data_len); l->data=NULL; }
    if (l->code) { dvm_munmap(l->code,l->code_len); l->code=NULL; }
}

int dvm_load_file(const char *path, DvmLoaded *out) {
    DvmProg prog;
    if (!dvm_read_file(path,&prog)) return 0;
    int ok=dvm_load(&prog,out);
    dvm_prog_free(&prog);
    return ok;
}
