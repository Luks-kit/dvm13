#include "dvm_io.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Write helpers ────────────────────────────────────────────────────────────

static int fw_u8 (FILE *f, uint8_t  v) { return fwrite(&v,1,1,f)==1; }
static int fw_u32(FILE *f, uint32_t v) { return fwrite(&v,4,1,f)==1; }
static int fw_tag(FILE *f, const char *tag) { return fwrite(tag,4,1,f)==1; }

static int fw_section(FILE *f, const char *tag, const uint8_t *data, size_t len) {
    return fw_tag(f,tag) && fw_u32(f,(uint32_t)len) && fwrite(data,1,len,f)==len;
}

// ─── dvm_write ────────────────────────────────────────────────────────────────

int dvm_write(FILE *f, const DvmProg *prog) {
    // magic
    if (fwrite(DVM_MAGIC,1,DVM_MAGIC_LEN,f) != DVM_MAGIC_LEN) return 0;

    // sections (all present, zero-size if empty)
    static const uint8_t empty = 0;
    if (!fw_section(f, DVM_TAG_CNST, prog->cnst_len ? prog->cnst : &empty, prog->cnst_len)) return 0;
    if (!fw_section(f, DVM_TAG_DATA, prog->data_len ? prog->data : &empty, prog->data_len)) return 0;
    if (!fw_section(f, DVM_TAG_CODE, prog->code,     prog->code_len)) return 0;

    // symbol table
    if (!fw_tag(f, DVM_TAG_SYMS)) return 0;
    if (!fw_u32(f, (uint32_t)prog->nsyms)) return 0;
    for (size_t i = 0; i < prog->nsyms; i++) {
        const DvmSym *s = &prog->syms[i];
        uint8_t  namelen = (uint8_t)strlen(s->name);
        if (!fw_u8(f, (uint8_t)s->section)) return 0;
        if (!fw_u32(f, s->offset))          return 0;
        if (!fw_u8(f, namelen))             return 0;
        if (fwrite(s->name,1,namelen,f) != namelen) return 0;
    }

    // relocation table
    if (!fw_tag(f, DVM_TAG_RELS)) return 0;
    if (!fw_u32(f, (uint32_t)prog->nrels)) return 0;
    for (size_t i = 0; i < prog->nrels; i++) {
        const DvmRel *r = &prog->rels[i];
        if (!fw_u32(f, r->code_offset))   return 0;
        if (!fw_u8(f, (uint8_t)r->section)) return 0;
        if (!fw_u32(f, r->sym_offset))    return 0;
    }

    // end sentinel
    if (!fw_tag(f, DVM_TAG_END)) return 0;
    return 1;
}

int dvm_write_file(const char *path, const DvmProg *prog) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    int ok = dvm_write(f, prog);
    fclose(f);
    if (!ok) fprintf(stderr, "dvm_write_file: write error: %s\n", path);
    return ok;
}

// ─── Read helpers ─────────────────────────────────────────────────────────────

static int fr_u8 (FILE *f, uint8_t  *v) { return fread(v,1,1,f)==1; }
static int fr_u32(FILE *f, uint32_t *v) { return fread(v,4,1,f)==1; }
static int fr_tag(FILE *f, char tag[5]) {
    tag[4] = 0;
    return fread(tag,1,4,f)==4;
}
static int fr_expect_tag(FILE *f, const char *expected) {
    char got[5];
    if (!fr_tag(f,got)) return 0;
    if (memcmp(got,expected,4)) {
        fprintf(stderr,"dvm: expected tag '%s', got '%.4s'\n",expected,got);
        return 0;
    }
    return 1;
}

static uint8_t *fr_section(FILE *f, size_t *len_out) {
    uint32_t sz;
    if (!fr_u32(f,&sz)) return NULL;
    *len_out = sz;
    if (sz == 0) return (uint8_t*)calloc(1,1);  // non-NULL empty
    uint8_t *buf = malloc(sz);
    if (!buf) return NULL;
    if (fread(buf,1,sz,f) != sz) { free(buf); return NULL; }
    return buf;
}

// ─── dvm_read ─────────────────────────────────────────────────────────────────

int dvm_read(FILE *f, DvmProg *prog) {
    memset(prog, 0, sizeof(*prog));

    // magic
    char magic[5]; magic[4]=0;
    if (fread(magic,1,4,f)!=4 || memcmp(magic,DVM_MAGIC,4)) {
        fputs("dvm: bad magic\n",stderr); return 0;
    }

    // sections
    if (!fr_expect_tag(f,DVM_TAG_CNST)) return 0;
    if (!(prog->cnst = fr_section(f, &prog->cnst_len))) return 0;

    if (!fr_expect_tag(f,DVM_TAG_DATA)) return 0;
    if (!(prog->data = fr_section(f, &prog->data_len))) return 0;

    if (!fr_expect_tag(f,DVM_TAG_CODE)) return 0;
    if (!(prog->code = fr_section(f, &prog->code_len))) return 0;
    if (!prog->code_len) { fputs("dvm: empty CODE section\n",stderr); return 0; }

    // symbol table
    if (!fr_expect_tag(f,DVM_TAG_SYMS)) return 0;
    uint32_t nsyms;
    if (!fr_u32(f,&nsyms)) return 0;
    if (nsyms > DVM_MAX_SYMS) { fputs("dvm: too many symbols\n",stderr); return 0; }
    prog->nsyms = nsyms;
    for (uint32_t i = 0; i < nsyms; i++) {
        DvmSym *s = &prog->syms[i];
        uint8_t sec, namelen;
        if (!fr_u8(f,&sec))     return 0;
        if (!fr_u32(f,&s->offset)) return 0;
        if (!fr_u8(f,&namelen)) return 0;
        s->section = (DvmSection)sec;
        if (fread(s->name,1,namelen,f) != namelen) return 0;
        s->name[namelen] = 0;
    }

    // relocation table
    if (!fr_expect_tag(f,DVM_TAG_RELS)) return 0;
    uint32_t nrels;
    if (!fr_u32(f,&nrels)) return 0;
    if (nrels > DVM_MAX_RELS) { fputs("dvm: too many relocations\n",stderr); return 0; }
    prog->nrels = nrels;
    for (uint32_t i = 0; i < nrels; i++) {
        DvmRel *r = &prog->rels[i];
        uint8_t sec;
        if (!fr_u32(f,&r->code_offset)) return 0;
        if (!fr_u8(f,&sec))             return 0;
        if (!fr_u32(f,&r->sym_offset))  return 0;
        r->section = (DvmSection)sec;
    }

    // end sentinel
    if (!fr_expect_tag(f,DVM_TAG_END)) return 0;
    return 1;
}

int dvm_read_file(const char *path, DvmProg *prog) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    int ok = dvm_read(f, prog);
    fclose(f);
    return ok;
}

void dvm_prog_free(DvmProg *prog) {
    free(prog->cnst); prog->cnst = NULL;
    free(prog->data); prog->data = NULL;
    free(prog->code); prog->code = NULL;
}

// ─── dvm_load ─────────────────────────────────────────────────────────────────
// Allocates proper pages for each section, copies data in,
// applies relocations (patches imm64 in CODE with runtime section address),
// then sets CODE to RX.

int dvm_load(const DvmProg *prog, DvmLoaded *out) {
    memset(out, 0, sizeof(*out));

    // Allocate sections (minimum 1 page each even if empty)
    out->cnst_len = prog->cnst_len;
    out->data_len = prog->data_len;
    out->code_len = prog->code_len;

    // CNST: RW initially so we can copy, then set RO
    if (prog->cnst_len) {
        out->cnst = dvm_mmap(prog->cnst_len, DVM_MMAP_RW);
        memcpy(out->cnst, prog->cnst, prog->cnst_len);
    }

    // DATA: RW
    if (prog->data_len) {
        out->data = dvm_mmap(prog->data_len, DVM_MMAP_RW);
        memcpy(out->data, prog->data, prog->data_len);
    }

    // CODE: RW initially for patching, then RX
    out->code = dvm_mmap(prog->code_len, DVM_MMAP_RW);
    memcpy(out->code, prog->code, prog->code_len);

    // Apply relocations
    const uint8_t *bases[3] = {
        [DVM_SEC_CNST] = out->cnst,
        [DVM_SEC_DATA] = out->data,
        [DVM_SEC_CODE] = out->code,
    };

    for (size_t i = 0; i < prog->nrels; i++) {
        const DvmRel *r = &prog->rels[i];
        if (r->code_offset + 8 > prog->code_len) {
            fprintf(stderr,"dvm_load: reloc out of bounds at code+%u\n",r->code_offset);
            dvm_unload(out); return 0;
        }
        if ((unsigned)r->section > 2 || !bases[r->section]) {
            fprintf(stderr,"dvm_load: reloc references empty section %d\n",r->section);
            dvm_unload(out); return 0;
        }
        uint64_t target = (uint64_t)(bases[r->section] + r->sym_offset);
        memcpy(out->code + r->code_offset, &target, 8);
    }

    // Protect sections
    if (out->cnst) dvm_mprotect(out->cnst, out->cnst_len, DVM_MMAP_READ);
    dvm_mprotect(out->code, out->code_len, DVM_MMAP_READ | DVM_MMAP_EXEC);

    return 1;
}

void dvm_unload(DvmLoaded *loaded) {
    if (loaded->cnst) { dvm_munmap(loaded->cnst, loaded->cnst_len); loaded->cnst=NULL; }
    if (loaded->data) { dvm_munmap(loaded->data, loaded->data_len); loaded->data=NULL; }
    if (loaded->code) { dvm_munmap(loaded->code, loaded->code_len); loaded->code=NULL; }
}

int dvm_load_file(const char *path, DvmLoaded *out) {
    DvmProg prog;
    if (!dvm_read_file(path, &prog)) return 0;
    int ok = dvm_load(&prog, out);
    dvm_prog_free(&prog);
    return ok;
}
