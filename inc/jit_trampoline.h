#pragma once
#include "vm.h"
#include "jit.h"

// ─── dvm JIT — trampoline + block table ──────────────────────────────────────
// Every bytecode block gets a 16-byte trampoline stub allocated upfront.
// Shadow stack stores trampoline pointers — stable, valid before and after
// compilation. No two-pass needed; cold path hits the compile thunk once,
// hot path is a direct jmp through the (now-patched) trampoline.
//
// Trampoline layout (16 bytes, one cache line slot):
//   [0..1]   ff 25        JMP QWORD PTR [rip+0]
//   [2..9]   <target:8>   absolute native ptr
//   [10..15] 66 90 ...    padding (xchg ax,ax nops)
//
// Before compile: target = per-block compile stub (loads bc_offset, calls JIT)
// After  compile: target = native block ptr (direct, zero overhead)
//
// Patching is an atomic aligned 8-byte store — safe without a lock on x86.

// ─── Block ────────────────────────────────────────────────────────────────────
#define DVM_MAX_BLOCKS 4096
#define DVM_TRAMP_SIZE 16

typedef struct {
    uint32_t  bc_offset;    // bytecode offset this block starts at
    uint8_t  *trampoline;   // pointer into trampoline arena (16 bytes)
    uint8_t  *native;       // compiled native code, NULL until compiled
} Block;

// ─── JitCtx ──────────────────────────────────────────────────────────────────
struct JitCtx {
    uint8_t  *bc;           // bytecode (read-only, not owned)
    size_t    bc_len;

    // trampoline arena: RWX, MAX_BLOCKS * TRAMP_SIZE bytes
    uint8_t  *tramp_arena;
    size_t    tramp_cap;

    // native code arena: starts RW, regions made RX after each block compile
    JitBuf    code;

    // per-block compile stubs arena (one 32-byte stub per block, RWX)
    uint8_t  *stub_arena;
    size_t    stub_cap;

    Block     blocks[DVM_MAX_BLOCKS];
    size_t    nblocks;
};

// ─── API ──────────────────────────────────────────────────────────────────────

// Create / destroy a JIT context for the given bytecode.
JitCtx *jit_ctx_new(uint8_t *bc, size_t bc_len);
void    jit_ctx_free(JitCtx *ctx);

// Get or create a block entry for bc_offset.
// Allocates a trampoline and per-block compile stub on first call.
Block  *jit_block_get(JitCtx *ctx, uint32_t bc_offset);

// Return the trampoline address for bc_offset (creating if needed).
// This is what CALL/RET/THROW push onto the shadow stack.
uint8_t *jit_tramp_for(JitCtx *ctx, uint32_t bc_offset);

// Patch a trampoline to point at new_target (atomic).
void jit_tramp_patch(uint8_t *tramp, void *new_target);

// Entry point: get the trampoline for offset 0, JIT-compile it eagerly,
// return a function pointer to jump into.
typedef void (*JitEntry)(void);
JitEntry jit_entry(JitCtx *ctx);
