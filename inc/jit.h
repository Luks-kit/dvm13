#pragma once
#include "vm.h"
#include <stdint.h>

// ─── dvm JIT — x86-64 emitter ────────────────────────────────────────────────
// VM register → x86-64 register mapping (mirrors vm_run pinning):
//   ax→rax(0)  bx→rbx(3)  cx→rcx(1)  dx→rdx(2)
//   di→rdi(7)  si→rsi(6)  ex→r8(8)   fx→r9(9)
//   fp[0..7] → xmm0-7
//   sp→r12(12) bp→r11(11) fl→r13(13)
//   r14 = shadow stack ptr
//   r15 = error register (untouched)

// x86-64 register IDs (ModRM / REX encoding)
#define X86_RAX  0
#define X86_RCX  1
#define X86_RDX  2
#define X86_RBX  3
#define X86_RSP  4
#define X86_RBP  5
#define X86_RSI  6
#define X86_RDI  7
#define X86_R8   8
#define X86_R9   9
#define X86_R10  10
#define X86_R11  11
#define X86_R12  12
#define X86_R13  13
#define X86_R14  14
#define X86_R15  15

// VM reg id (0-7) → x86 reg id
static const uint8_t vm2x86[8] = {
    X86_RAX,  // ax
    X86_RBX,  // bx
    X86_RCX,  // cx
    X86_RDX,  // dx
    X86_RDI,  // di
    X86_RSI,  // si
    X86_R8,   // ex
    X86_R9,   // fx
};

// ─── Emit buffer ─────────────────────────────────────────────────────────────
// Separate from Buf — JIT output lives in a mmap'd W→X region.

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} JitBuf;

JitBuf  jit_buf_new(size_t cap);
void    jit_buf_free(JitBuf *j);
void    jit_buf_make_exec(JitBuf *j);   // mprotect RW→RX
uint8_t *jit_buf_alloc(JitBuf *j, size_t need);

// ─── Primitive emitters ───────────────────────────────────────────────────────
void jit_u8 (JitBuf *j, uint8_t  v);
void jit_u16(JitBuf *j, uint16_t v);
void jit_u32(JitBuf *j, uint32_t v);
void jit_i32(JitBuf *j, int32_t  v);
void jit_u64(JitBuf *j, uint64_t v);

// REX.W + op + ModRM(reg=dst, rm=src), both as x86 reg IDs
void jit_rr(JitBuf *j, uint8_t op, uint8_t dst_x86, uint8_t src_x86);

// MOV dst_vm, imm64
void jit_mov_ri(JitBuf *j, uint8_t vm_dst, uint64_t imm);

// ALU op between two VM regs (ADD=0x03, SUB=0x2B, AND=0x23, OR=0x0B, XOR=0x33)
void jit_alu_rr(JitBuf *j, uint8_t x86op, uint8_t vm_dst, uint8_t vm_src);

// Prologue / epilogue
void jit_prologue(JitBuf *j);
void jit_epilogue(JitBuf *j);

// CALL/RET/THROW using shadow stack in r14
// ok_tramp and err_tramp are native pointers (trampoline stubs)
void jit_emit_call (JitBuf *j, uint8_t *ok_tramp, uint8_t *err_tramp);
void jit_emit_ret  (JitBuf *j);
void jit_emit_throw(JitBuf *j);

// ─── Block compiler ───────────────────────────────────────────────────────────
// Forward decl — JitCtx defined in jit_trampoline.h
typedef struct JitCtx JitCtx;

// Compile the bytecode block starting at bc_offset into ctx->code_arena.
// Patches the block's trampoline to point at the new native code.
// Returns pointer to native code, or NULL on error.
uint8_t *jit_compile_block(JitCtx *ctx, uint32_t bc_offset);
