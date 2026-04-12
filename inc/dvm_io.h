#pragma once
#include "dvm_fmt.h"
#include <stdio.h>

// ─── dvm I/O — serialize, deserialize, load ───────────────────────────────────

// ─── Writer ───────────────────────────────────────────────────────────────────
// Serialize a DvmProg to an open FILE* (must be opened in binary write mode).
// Returns 1 on success, 0 on error (message to stderr).
int dvm_write(FILE *f, const DvmProg *prog);

// Convenience: write to path.
int dvm_write_file(const char *path, const DvmProg *prog);

// ─── Reader ───────────────────────────────────────────────────────────────────
// Deserialize a .dvm file into a DvmProg.
// Section data is heap-allocated; caller must free prog->cnst, prog->data, prog->code.
// Returns 1 on success, 0 on error.
int dvm_read(FILE *f, DvmProg *prog);

int dvm_read_file(const char *path, DvmProg *prog);

// Free heap-allocated section data from a DvmProg produced by dvm_read.
void dvm_prog_free(DvmProg *prog);

// ─── Loader ───────────────────────────────────────────────────────────────────
// Map a DvmProg into executable memory and apply relocations.
// Returns 1 on success, 0 on error.
// Call dvm_unload() when done.
int  dvm_load  (const DvmProg *prog, DvmLoaded *out);
void dvm_unload(DvmLoaded *loaded);

// Load from file directly (convenience: dvm_read_file + dvm_load).
int dvm_load_file(const char *path, DvmLoaded *out);
