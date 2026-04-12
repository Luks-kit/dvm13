#pragma once
// ─── dvm binary format (.dvm) ─────────────────────────────────────────────────
//
// Layout:
//   [magic:4]              "dvm@"  (0x64 0x76 0x6d 0x40)
//   [CNST:4][size:4][data] read-only constants (may be size=0)
//   [DATA:4][size:4][data] read-write data     (may be size=0)
//   [CODE:4][size:4][data] bytecode            (must be non-empty)
//   [SYMS:4][count:4]      symbol table
//     per symbol: [section:1][offset:4][namelen:1][name:namelen]
//   [RELS:4][count:4]      relocation table
//     per reloc:  [code_offset:4][section:1][sym_offset:4]
//   [END\0:4]              end sentinel
//
// Sections are fixed-order; all must be present (zero size = empty section).
// All multi-byte integers are little-endian.
//
// Relocation semantics: at load time, for each reloc entry:
//   *(uint64_t*)(code_base + code_offset) = section_base[section] + sym_offset
// This patches the imm64 payload of a MOV_RI instruction that referenced
// a cross-section symbol.

#include <stdint.h>
#include <stddef.h>

// ─── Magic and tags ───────────────────────────────────────────────────────────

#define DVM_MAGIC   "\x64\x76\x6d\x40"   // "dvm@"
#define DVM_MAGIC_LEN 4

#define DVM_TAG_CNST "CNST"
#define DVM_TAG_DATA "DATA"
#define DVM_TAG_CODE "CODE"
#define DVM_TAG_SYMS "SYMS"
#define DVM_TAG_RELS "RELS"
#define DVM_TAG_END  "END\0"
#define DVM_TAG_LEN  4

// ─── Section IDs ─────────────────────────────────────────────────────────────
// Used in symbol table and relocation entries.

typedef enum __attribute__((packed)) {
    DVM_SEC_CNST = 0,
    DVM_SEC_DATA = 1,
    DVM_SEC_CODE = 2,
    DVM_SEC_NONE = 0xFF,   // undefined / not a data symbol
} DvmSection;

// ─── In-memory representation ─────────────────────────────────────────────────

#define DVM_MAX_SYMS  512
#define DVM_MAX_RELS  1024
#define DVM_SYM_NAME  64    // max symbol name length (including NUL)

typedef struct {
    char       name[DVM_SYM_NAME];
    DvmSection section;
    uint32_t   offset;       // byte offset within the section
} DvmSym;

typedef struct {
    uint32_t   code_offset;  // offset within CODE section of the imm64 to patch
    DvmSection section;      // which section the symbol lives in
    uint32_t   sym_offset;   // offset within that section
} DvmRel;

// Fully-linked program ready for serialization or execution
typedef struct {
    uint8_t   *cnst;   size_t cnst_len;   // read-only data
    uint8_t   *data;   size_t data_len;   // read-write data
    uint8_t   *code;   size_t code_len;   // bytecode

    DvmSym     syms[DVM_MAX_SYMS];
    size_t     nsyms;

    DvmRel     rels[DVM_MAX_RELS];
    size_t     nrels;
} DvmProg;

// Loaded (runtime) state — sections mapped to executable/writable memory
typedef struct {
    uint8_t *cnst;   size_t cnst_len;
    uint8_t *data;   size_t data_len;
    uint8_t *code;   size_t code_len;
    // cnst is PROT_READ, data is PROT_READ|PROT_WRITE, code is PROT_READ|PROT_EXEC
} DvmLoaded;
