#pragma once
// ─── dvm binary format (.dvm) ─────────────────────────────────────────────────
//
// Supports both object files (unlinked, may have EXTERN symbols) and
// executables (fully linked, all relocs resolved to section+offset).
//
// File layout:
//   [magic:4]              "dvm@"  (0x64 0x76 0x6d 0x40)
//   [CNST:4][size:4][data] read-only constants (may be size=0)
//   [DATA:4][size:4][data] read-write data     (may be size=0)
//   [CODE:4][size:4][data] bytecode            (size=0 valid in object files)
//   [SYMS:4][count:4]      symbol table
//     per symbol:
//       [flags:1]          DVM_SYM_* flags
//       [section:1]        DvmSection (NONE for EXTERN)
//       [offset:4]         byte offset within section (0 for EXTERN)
//       [namelen:1]        length of name (max 63)
//       [name:namelen]     symbol name (no NUL)
//   [RELS:4][count:4]      relocation table
//     per reloc:
//       [code_offset:4]    offset in CODE section of the imm64 to patch
//       [sym_index:4]      index into SYMS table
//   [END\0:4]              end sentinel
//
// All multi-byte integers are little-endian.
//
// Relocation semantics (at link/load time):
//   *(uint64_t*)(code_base + code_offset) = section_base[sym.section] + sym.offset
//
// Object files may have EXTERN symbols (section=NONE) whose relocs are
// resolved by the linker. Executables have all symbols resolved.

#include <stdint.h>
#include <stddef.h>

// ─── Magic and tags ───────────────────────────────────────────────────────────

#define DVM_MAGIC     "\x64\x76\x6d\x40"
#define DVM_MAGIC_LEN 4

#define DVM_TAG_CNST "CNST"
#define DVM_TAG_DATA "DATA"
#define DVM_TAG_CODE "CODE"
#define DVM_TAG_SYMS "SYMS"
#define DVM_TAG_RELS "RELS"
#define DVM_TAG_END  "END\0"
#define DVM_TAG_LEN  4

// ─── Section IDs ─────────────────────────────────────────────────────────────

typedef enum __attribute__((packed)) {
    DVM_SEC_CNST = 0,
    DVM_SEC_DATA = 1,
    DVM_SEC_CODE = 2,
    DVM_SEC_NONE = 0xFF,  // undefined / extern
} DvmSection;

// ─── Symbol flags ─────────────────────────────────────────────────────────────

#define DVM_SYM_LOCAL   0x00  // visible only within this object
#define DVM_SYM_GLOBAL  0x01  // exported; visible to linker
#define DVM_SYM_EXTERN  0x02  // imported; must be resolved by linker

// ─── In-memory types ─────────────────────────────────────────────────────────

#define DVM_MAX_SYMS  1024
#define DVM_MAX_RELS  2048
#define DVM_SYM_NAME  64

typedef struct {
    char       name[DVM_SYM_NAME];
    uint8_t    flags;     // DVM_SYM_*
    DvmSection section;   // which section (NONE if extern)
    uint32_t   offset;    // byte offset within section
} DvmSym;

typedef struct {
    uint32_t code_offset; // offset in CODE section of the imm64 to patch
    uint32_t sym_index;   // index into syms[]
} DvmRel;

typedef struct {
    uint8_t  *cnst;  size_t cnst_len;
    uint8_t  *data;  size_t data_len;
    uint8_t  *code;  size_t code_len;

    DvmSym    syms[DVM_MAX_SYMS];
    size_t    nsyms;

    DvmRel    rels[DVM_MAX_RELS];
    size_t    nrels;
} DvmProg;

// Loaded (runtime) sections mapped into proper memory
typedef struct {
    uint8_t  *cnst;  size_t cnst_len;  // PROT_READ
    uint8_t  *data;  size_t data_len;  // PROT_READ|WRITE
    uint8_t  *code;  size_t code_len;  // PROT_READ|EXEC
} DvmLoaded;
