#pragma once
#include "vm.h"
#include "dvm_fmt.h"

// ─── dvm assembler ────────────────────────────────────────────────────────────
// Single-pass with backpatch for forward code labels.
// Cross-section data references generate relocation entries resolved at load.
//
// Section directives (switch active output section):
//   .text    — code (default)
//   .data    — read-write data
//   .cnst    — read-only constants
//
// Data pseudo-ops (valid in .data and .cnst sections):
//   db  42           — emit 1 byte
//   dw  0x1234       — emit 2 bytes (LE)
//   dd  0xDEAD       — emit 4 bytes (LE)
//   dq  0xCAFEBABE   — emit 8 bytes (LE)
//   ds  "hello"      — emit raw string bytes (no NUL)
//   dsz "hello"      — emit null-terminated string
//
// Cross-section references in .text:
//   mov ax, my_data_label   — emits MOV_RI + relocation entry
//   (the 8-byte imm64 is patched at load time to the label's runtime address)
//
// Code syntax unchanged from before.

// ─── Label table ─────────────────────────────────────────────────────────────

#define ASM_MAX_LABELS  512
#define ASM_MAX_PATCHES 1024
#define ASM_LABEL_LEN   64

typedef struct {
    char       name[ASM_LABEL_LEN];
    DvmSection section;   // which section this label belongs to
    int32_t    offset;    // byte offset within that section (-1 = undefined)
} AsmLabel;

typedef struct {
    char   name[ASM_LABEL_LEN];
    size_t patch_at;   // offset in target Buf of the value to fill
    int    is64;       // 1 = 8-byte imm (data reloc), 0 = 4-byte abs code offset
    int    line;
} AsmPatch;

typedef struct {
    AsmLabel  labels[ASM_MAX_LABELS];
    size_t    nlabels;
    AsmPatch  patches[ASM_MAX_PATCHES];
    size_t    npatches;
} LabelTable;

// ─── Assembler context ────────────────────────────────────────────────────────

typedef struct {
    Buf        text;    // .text — code
    Buf        data;    // .data — rw data
    Buf        cnst;    // .cnst — ro constants
    LabelTable lt;
    DvmRel     rels[DVM_MAX_RELS];
    size_t     nrels;
} AsmCtx;

// ─── API ──────────────────────────────────────────────────────────────────────

// Assemble null-terminated source into ctx.
// Returns 1 on success, 0 on error (message to stderr).
int asm_compile(const char *src, AsmCtx *ctx);

// Build a DvmProg from an AsmCtx.  Pointers into ctx's bufs — ctx must outlive prog.
void asm_to_prog(const AsmCtx *ctx, DvmProg *prog);

AsmLabel *asm_label_find(LabelTable *lt, const char *name);
