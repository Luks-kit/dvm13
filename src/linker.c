#include "linker.h"
#include "dvm_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── dvm linker ───────────────────────────────────────────────────────────────
// Merges N object DvmProgs into one fully-linked executable.
//
// Merge layout:
//   out.cnst = obj[0].cnst ++ obj[1].cnst ++ ...
//   out.data = obj[0].data ++ obj[1].data ++ ...
//   out.code = obj[0].code ++ obj[1].code ++ ...
//
// Symbol remapping:
//   Each object's symbols get their offsets adjusted by the section base
//   accumulated so far.  Globals land in a merged symbol table.
//   Externs are resolved against globals from all objects.
//
// Relocation remapping:
//   code_offset += code_base[obj_i]
//   sym_index   → index into merged symbol table

// Per-object state during link
typedef struct {
    size_t cnst_base;
    size_t data_base;
    size_t code_base;
    // Maps obj-local sym index → merged sym index (-1 if dropped)
    int32_t sym_map[DVM_MAX_SYMS];
} ObjState;

// Merged symbol table (globals only + resolved externs)
typedef struct {
    DvmSym syms[DVM_MAX_SYMS];
    size_t nsyms;
} MergedSyms;

static int32_t msyms_find(const MergedSyms *ms, const char *name) {
    for (size_t i=0; i<ms->nsyms; i++)
        if (!strcmp(ms->syms[i].name, name))
            return (int32_t)i;
    return -1;
}

static int32_t msyms_add(MergedSyms *ms, const DvmSym *s) {
    if (ms->nsyms >= DVM_MAX_SYMS) {
        fputs("link: symbol table full\n", stderr); return -1;
    }
    ms->syms[ms->nsyms] = *s;
    return (int32_t)ms->nsyms++;
}

int dvm_link(const DvmProg *objs, size_t nobjs, DvmProg *out) {
    memset(out, 0, sizeof(*out));

    // ── Pass 1: measure merged section sizes ──────────────────────────────────
    size_t total_cnst=0, total_data=0, total_code=0;
    for (size_t i=0; i<nobjs; i++) {
        total_cnst += objs[i].cnst_len;
        total_data += objs[i].data_len;
        total_code += objs[i].code_len;
    }

    // Allocate output sections
    out->cnst = total_cnst ? calloc(1, total_cnst) : NULL;
    out->data = total_data ? calloc(1, total_data) : NULL;
    out->code = total_code ? calloc(1, total_code) : NULL;
    out->cnst_len = total_cnst;
    out->data_len = total_data;
    out->code_len = total_code;
    if ((total_cnst && !out->cnst) || (total_data && !out->data) ||
        (total_code && !out->code)) {
        fputs("link: allocation failed\n", stderr);
        dvm_prog_free(out); return 0;
    }

    // ── Pass 2: copy section data + build base table ───────────────────────────
    ObjState *states = calloc(nobjs, sizeof(ObjState));
    size_t cnst_off=0, data_off=0, code_off=0;
    for (size_t i=0; i<nobjs; i++) {
        states[i].cnst_base = cnst_off;
        states[i].data_base = data_off;
        states[i].code_base = code_off;
        if (objs[i].cnst_len) {
            memcpy(out->cnst+cnst_off, objs[i].cnst, objs[i].cnst_len);
            cnst_off += objs[i].cnst_len;
        }
        if (objs[i].data_len) {
            memcpy(out->data+data_off, objs[i].data, objs[i].data_len);
            data_off += objs[i].data_len;
        }
        if (objs[i].code_len) {
            memcpy(out->code+code_off, objs[i].code, objs[i].code_len);
            code_off += objs[i].code_len;
        }
        for (size_t j=0; j<DVM_MAX_SYMS; j++) states[i].sym_map[j] = -1;
    }

    // ── Pass 3: build merged symbol table ─────────────────────────────────────
    // First pass: define all globals (with adjusted offsets)
    MergedSyms ms; memset(&ms, 0, sizeof(ms));

    for (size_t i=0; i<nobjs; i++) {
        const ObjState *st = &states[i];
        for (size_t j=0; j<objs[i].nsyms; j++) {
            const DvmSym *s = &objs[i].syms[j];
            if (s->flags == DVM_SYM_EXTERN) continue;  // handle in pass 4

            // Build adjusted symbol
            DvmSym adj = *s;
            switch (s->section) {
            case DVM_SEC_CNST: adj.offset += (uint32_t)st->cnst_base; break;
            case DVM_SEC_DATA: adj.offset += (uint32_t)st->data_base; break;
            case DVM_SEC_CODE: adj.offset += (uint32_t)st->code_base; break;
            default: break;
            }

            if (s->flags == DVM_SYM_GLOBAL) {
                // Check for duplicate global definition
                int32_t existing = msyms_find(&ms, s->name);
                if (existing >= 0 && ms.syms[existing].flags == DVM_SYM_GLOBAL &&
                    ms.syms[existing].section != DVM_SEC_NONE) {
                    fprintf(stderr,"link: duplicate global symbol '%s'\n",s->name);
                    free(states); dvm_prog_free(out); return 0;
                }
                if (existing >= 0) {
                    // Was a forward extern placeholder — resolve it
                    ms.syms[existing] = adj;
                    states[i].sym_map[j] = existing;
                } else {
                    states[i].sym_map[j] = msyms_add(&ms, &adj);
                }
            } else {
                // LOCAL: add to merged table (still needed for relocs)
                states[i].sym_map[j] = msyms_add(&ms, &adj);
            }
        }
    }

    // ── Pass 4: resolve externs ───────────────────────────────────────────────
    for (size_t i=0; i<nobjs; i++) {
        for (size_t j=0; j<objs[i].nsyms; j++) {
            const DvmSym *s = &objs[i].syms[j];
            if (s->flags != DVM_SYM_EXTERN) continue;

            int32_t resolved = msyms_find(&ms, s->name);
            if (resolved < 0 || ms.syms[resolved].section == DVM_SEC_NONE) {
                fprintf(stderr,"link: unresolved extern '%s'\n",s->name);
                free(states); dvm_prog_free(out); return 0;
            }
            states[i].sym_map[j] = resolved;
        }
    }

    // ── Pass 5: copy and remap relocations ────────────────────────────────────
    for (size_t i=0; i<nobjs; i++) {
        const ObjState *st = &states[i];
        for (size_t j=0; j<objs[i].nrels; j++) {
            const DvmRel *r = &objs[i].rels[j];
            if (out->nrels >= DVM_MAX_RELS) {
                fputs("link: reloc table full\n", stderr);
                free(states); dvm_prog_free(out); return 0;
            }
            int32_t new_sym = (r->sym_index < objs[i].nsyms)
                ? states[i].sym_map[r->sym_index] : -1;
            if (new_sym < 0) {
                fprintf(stderr,"link: reloc[%zu] in obj[%zu] unmapped sym\n",j,i);
                free(states); dvm_prog_free(out); return 0;
            }
            DvmRel *nr = &out->rels[out->nrels++];
            nr->code_offset = r->code_offset + (uint32_t)st->code_base;
            nr->sym_index   = (uint32_t)new_sym;
            nr->kind        = r->kind;
        }
    }

    // Finalize symbol table
    out->nsyms = ms.nsyms;
    memcpy(out->syms, ms.syms, ms.nsyms * sizeof(DvmSym));

    free(states);
    return 1;
}

int dvm_link_files(const char **obj_paths, size_t nobjs, const char *out_path) {
    DvmProg *objs = calloc(nobjs, sizeof(DvmProg));
    int ok = 1;

    for (size_t i=0; i<nobjs; i++) {
        if (!dvm_read_file(obj_paths[i], &objs[i])) {
            fprintf(stderr,"link: failed to read '%s'\n",obj_paths[i]);
            ok = 0; goto done;
        }
    }

    DvmProg linked;
    if (!dvm_link(objs, nobjs, &linked)) { ok=0; goto done; }

    if (!dvm_write_file(out_path, &linked)) {
        fprintf(stderr,"link: failed to write '%s'\n",out_path);
        ok = 0;
    }
    dvm_prog_free(&linked);

done:
    for (size_t i=0; i<nobjs; i++) dvm_prog_free(&objs[i]);
    free(objs);
    return ok;
}
