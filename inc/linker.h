#pragma once
#include "dvm_fmt.h"

// ─── dvm linker ───────────────────────────────────────────────────────────────
// Links N object DvmProgs into a single executable DvmProg.
//
// Algorithm:
//   1. Merge sections: concatenate each input's CNST/DATA/CODE
//   2. Relocate symbols: each input's symbols get their offsets adjusted
//      by the base of their section in the merged output
//   3. Resolve externs: for each EXTERN symbol, find a matching GLOBAL
//      from any other input; replace EXTERN with its resolved location
//   4. Patch relocs: using the resolved symbol table, all relocs become
//      {code_offset_in_merged_code, resolved_sym_index}
//   5. Error on: unresolved externs, duplicate globals
//
// Output is a fully-linked DvmProg with no EXTERN symbols.
// Section data in the output is heap-allocated; call dvm_prog_free() when done.

// Link N objects. Returns 1 on success, 0 on error (message to stderr).
// entry_sym: name of the entry point symbol (NULL = auto-detect _start, then main, then 0).
int dvm_link(const DvmProg *objs, size_t nobjs, DvmProg *out, const char *entry_sym);

// Convenience: link from files, write result to path.
int dvm_link_files(const char **obj_paths, size_t nobjs,
                   const char *out_path, const char *entry_sym);
