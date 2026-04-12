#pragma once
#include "vm.h"
#include "dvm_fmt.h"

// ─── dvm assembler ────────────────────────────────────────────────────────────
//
// Section directives:
//   .text          switch to code section (default)
//   .data          switch to read-write data section
//   .cnst          switch to read-only constant section
//
// Symbol visibility directives:
//   global name    mark label as globally exported (visible to linker)
//   extern name    declare an external symbol (resolved by linker)
//
// Constant definitions:
//   .equ name, value   define a numeric constant (no storage allocated)
//                      value can be decimal or 0x hex
//                      usable as an immediate in any instruction
//
// Data pseudo-ops (.data / .cnst only):
//   db  expr           1 byte
//   dw  expr           2 bytes LE
//   dd  expr           4 bytes LE
//   dq  expr           8 bytes LE
//   ds  "string"       raw bytes (no NUL), supports \n \t \r \0 \\ \"
//   dsz "string"       null-terminated string
//
// Cross-section references in .text:
//   mov reg, data_label   emits MOV_RI + relocation (patched at load/link time)
//
// Code instructions: same as before (see original asm.h for full list)

#define ASM_MAX_LABELS  512
#define ASM_MAX_PATCHES 1024
#define ASM_MAX_EQUS    256
#define ASM_LABEL_LEN   64

typedef struct {
    char       name[ASM_LABEL_LEN];
    uint8_t    flags;     // DVM_SYM_* — LOCAL/GLOBAL/EXTERN
    DvmSection section;
    int32_t    offset;    // -1 = undefined
} AsmLabel;

typedef struct {
    char   name[ASM_LABEL_LEN];
    size_t patch_at;   // offset in target Buf of value to fill
    int    is64;       // 1 = 8-byte imm (data/extern reloc), 0 = 4-byte code offset
    int    line;
} AsmPatch;

typedef struct {
    char     name[ASM_LABEL_LEN];
    uint64_t value;
} AsmEqu;

typedef struct {
    AsmLabel  labels[ASM_MAX_LABELS];
    size_t    nlabels;
    AsmPatch  patches[ASM_MAX_PATCHES];
    size_t    npatches;
    AsmEqu    equs[ASM_MAX_EQUS];
    size_t    nequs;
} LabelTable;

typedef struct {
    Buf        text;
    Buf        data;
    Buf        cnst;
    LabelTable lt;
    DvmRel     rels[DVM_MAX_RELS];
    size_t     nrels;
} AsmCtx;

// Assemble null-terminated source into ctx.
// Returns 1 on success, 0 on error (message to stderr).
int asm_compile(const char *src, AsmCtx *ctx);

// Convert AsmCtx → DvmProg.  Pointers into ctx bufs; ctx must outlive prog.
void asm_to_prog(const AsmCtx *ctx, DvmProg *prog);

AsmLabel *asm_label_find(LabelTable *lt, const char *name);
