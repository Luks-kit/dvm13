#include "jit_trampoline.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Trampoline helpers ───────────────────────────────────────────────────────
// Layout: [ff 25 00 00 00 00] [target:8] [66 90 66 90 66 90]
//          JMP [rip+0]         ^ptr here   nop padding

static void tramp_init(uint8_t *t, void *target) {
    t[0]=0xFF; t[1]=0x25;
    t[2]=0x00; t[3]=0x00; t[4]=0x00; t[5]=0x00;
    memcpy(t+6, &target, 8);
    t[14]=0x66; t[15]=0x90;
}

void jit_tramp_patch(uint8_t *t, void *new_target) {
    // aligned 8-byte atomic store on x86
    __atomic_store_n((uint64_t*)(t+6), (uint64_t)new_target, __ATOMIC_RELEASE);
}

// ─── Per-block compile stub ───────────────────────────────────────────────────
// Each block gets a 32-byte stub in stub_arena that:
//   1. saves bc_offset in red zone [rsp-8]  (before any caller-save clobber)
//   2. saves caller-saved VM regs
//   3. calls jit_compile_block_c(ctx, bc_offset)
//   4. restores regs
//   5. jumps to newly compiled native code
//
// Convention on entry: all VM regs are live in their pinned hardware registers.
// We must preserve rax/rcx/rdx/rdi/rsi/r8/r9/r10/r11 around the C call.

// The C-callable trampoline handler
static void compile_and_patch(JitCtx *ctx, uint32_t bc_offset) {
    jit_compile_block(ctx, bc_offset);
    // trampoline is now patched — caller will re-fire it
}

static uint8_t *emit_compile_stub(JitBuf *stubs, JitCtx *ctx, uint32_t bc_offset) {
    uint8_t *base = stubs->buf + stubs->len;

    // Helper: write one byte
    #define B(v)  (stubs->buf[stubs->len++] = (v))
    #define W64(v) do { uint64_t _v=(uint64_t)(v); memcpy(stubs->buf+stubs->len,&_v,8); stubs->len+=8; } while(0)
    #define W32(v) do { uint32_t _v=(uint32_t)(v); memcpy(stubs->buf+stubs->len,&_v,4); stubs->len+=4; } while(0)

    // Save bc_offset in red zone before anything else
    // mov QWORD PTR [rsp-8], imm32(bc_offset)  — safe, red zone not touched by signal
    B(0x48); B(0xC7); B(0x44); B(0x24); B(0xF8);  // REX.W C7 /0 [rsp-8]
    W32(bc_offset);

    // Save caller-saved VM regs that C ABI doesn't preserve:
    // rax rcx rdx rdi rsi r8 r9 r10 r11
    // rbx r12 r13 r14 r15 rbp are callee-saved — compiler/VM preserves them
    B(0x50);             // push rax
    B(0x51);             // push rcx
    B(0x52);             // push rdx
    B(0x57);             // push rdi
    B(0x56);             // push rsi
    B(0x41); B(0x50);    // push r8
    B(0x41); B(0x51);    // push r9
    B(0x41); B(0x52);    // push r10
    B(0x41); B(0x53);    // push r11
    // 9 pushes = 72 bytes — rsp now 72 below original. Need 16-byte align:
    // original rsp was aligned; -72 = -8 mod 16 → sub 8 more
    B(0x48); B(0x83); B(0xEC); B(0x08);  // sub rsp, 8

    // mov rdi, ctx  (arg1)
    uint64_t ctx_ptr = (uint64_t)ctx;
    B(0x48); B(0xBF); W64(ctx_ptr);

    // Reload bc_offset from red zone (was at [rsp + 72 + 8 + 8] = [rsp+88])
    // Actually it's at original_rsp - 8 = current_rsp + 72 + 8 + 8 - 8 = rsp+80
    // Let's just use imm32 directly — bc_offset is a compile-time constant here
    B(0xBE); W32(bc_offset);  // mov esi, bc_offset  (arg2, zero-extended)

    // call compile_and_patch
    uint64_t fn = (uint64_t)compile_and_patch;
    B(0x48); B(0xB8); W64(fn);   // mov rax, fn
    B(0xFF); B(0xD0);            // call rax

    // add rsp, 8  (undo alignment pad)
    B(0x48); B(0x83); B(0xC4); B(0x08);

    // Restore regs (reverse order)
    B(0x41); B(0x5B);   // pop r11
    B(0x41); B(0x5A);   // pop r10
    B(0x41); B(0x59);   // pop r9
    B(0x41); B(0x58);   // pop r8
    B(0x5E);            // pop rsi
    B(0x5F);            // pop rdi
    B(0x5A);            // pop rdx
    B(0x59);            // pop rcx
    B(0x58);            // pop rax

    // ret — back to trampoline, which now has native ptr → fires directly
    B(0xC3);

    #undef B
    #undef W64
    #undef W32

    return base;
}

// ─── JitCtx ───────────────────────────────────────────────────────────────────

JitCtx *jit_ctx_new(uint8_t *bc, size_t bc_len) {
    JitCtx *ctx = calloc(1, sizeof(JitCtx));
    ctx->bc     = bc;
    ctx->bc_len = bc_len;

    ctx->tramp_cap   = DVM_MAX_BLOCKS * DVM_TRAMP_SIZE;
    ctx->tramp_arena = dvm_mmap(ctx->tramp_cap, DVM_MMAP_RWX);

    ctx->stub_cap   = DVM_MAX_BLOCKS * 64;  // 64 bytes per stub, generous
    ctx->stub_arena = dvm_mmap(ctx->stub_cap, DVM_MMAP_RWX);

    ctx->code = jit_buf_new(1 << 20);  // 1MB initial code arena

    return ctx;
}

void jit_ctx_free(JitCtx *ctx) {
    dvm_munmap(ctx->tramp_arena, ctx->tramp_cap);
    dvm_munmap(ctx->stub_arena,  ctx->stub_cap);
    jit_buf_free(&ctx->code);
    free(ctx);
}

// ─── Block table ─────────────────────────────────────────────────────────────

Block *jit_block_get(JitCtx *ctx, uint32_t bc_offset) {
    // Linear scan — fine for thousands of blocks; swap for hash if needed
    for (size_t i = 0; i < ctx->nblocks; i++)
        if (ctx->blocks[i].bc_offset == bc_offset)
            return &ctx->blocks[i];

    if (ctx->nblocks >= DVM_MAX_BLOCKS) {
        fputs("jit: block table full\n", stderr);
        abort();
    }

    Block *b = &ctx->blocks[ctx->nblocks];
    b->bc_offset  = bc_offset;
    b->native     = NULL;
    b->trampoline = ctx->tramp_arena + ctx->nblocks * DVM_TRAMP_SIZE;
    ctx->nblocks++;

    // Build the compile stub for this block
    JitBuf stubs = { ctx->stub_arena, ctx->nblocks * 64 - 64, ctx->stub_cap };
    uint8_t *stub = emit_compile_stub(&stubs, ctx, bc_offset);

    // Point trampoline at stub initially
    tramp_init(b->trampoline, stub);

    return b;
}

uint8_t *jit_tramp_for(JitCtx *ctx, uint32_t bc_offset) {
    return jit_block_get(ctx, bc_offset)->trampoline;
}

// ─── Entry ────────────────────────────────────────────────────────────────────

JitEntry jit_entry(JitCtx *ctx) {
    // Eagerly compile block 0
    jit_compile_block(ctx, 0);
    return (JitEntry)jit_block_get(ctx, 0)->trampoline;
}
