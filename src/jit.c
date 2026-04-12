#include "jit.h"
#include "syscall.h"
#include "jit_trampoline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── JitBuf ───────────────────────────────────────────────────────────────────

JitBuf jit_buf_new(size_t cap) {
    JitBuf j;
    j.buf = dvm_mmap(cap, DVM_MMAP_RW);
    j.len = 0;
    j.cap = cap;
    return j;
}

void jit_buf_free(JitBuf *j) {
    if (j->buf) dvm_munmap(j->buf, j->cap);
    j->buf = NULL;
}

void jit_buf_make_exec(JitBuf *j) {
    dvm_mprotect(j->buf, j->cap, DVM_MMAP_READ | DVM_MMAP_EXEC);
}

uint8_t *jit_buf_alloc(JitBuf *j, size_t need) {
    if (j->len + need > j->cap) {
        size_t new_cap = j->cap * 2;
        uint8_t *nb = dvm_mmap(new_cap, DVM_MMAP_RW);
        memcpy(nb, j->buf, j->len);
        dvm_munmap(j->buf, j->cap);
        j->buf = nb;
        j->cap = new_cap;
    }
    uint8_t *p = j->buf + j->len;
    j->len += need;
    return p;
}

// ─── Primitive emitters ───────────────────────────────────────────────────────

void jit_u8 (JitBuf *j, uint8_t  v) { *jit_buf_alloc(j,1) = v; }
void jit_u16(JitBuf *j, uint16_t v) { memcpy(jit_buf_alloc(j,2),&v,2); }
void jit_u32(JitBuf *j, uint32_t v) { memcpy(jit_buf_alloc(j,4),&v,4); }
void jit_i32(JitBuf *j, int32_t  v) { memcpy(jit_buf_alloc(j,4),&v,4); }
void jit_u64(JitBuf *j, uint64_t v) { memcpy(jit_buf_alloc(j,8),&v,8); }

// REX prefix: W=64bit, R=reg-field ext, B=rm-field ext
static inline uint8_t rex(int W, int R, int B) {
    return (uint8_t)(0x40 | (W<<3) | (R<<2) | B);
}

// REX.W + op + ModRM(mod=11, reg=dst, rm=src)
void jit_rr(JitBuf *j, uint8_t op, uint8_t d, uint8_t s) {
    jit_u8(j, rex(1, (d>>3)&1, (s>>3)&1));
    jit_u8(j, op);
    jit_u8(j, (uint8_t)(0xC0 | ((d&7)<<3) | (s&7)));
}

// MOV r64, imm64  (vm_dst → x86 reg)
void jit_mov_ri(JitBuf *j, uint8_t vm_dst, uint64_t imm) {
    uint8_t r = vm2x86[vm_dst];
    jit_u8(j, rex(1, 0, (r>>3)&1));
    jit_u8(j, (uint8_t)(0xB8 | (r&7)));
    jit_u64(j, imm);
}

void jit_alu_rr(JitBuf *j, uint8_t x86op, uint8_t vm_d, uint8_t vm_s) {
    jit_rr(j, x86op, vm2x86[vm_d], vm2x86[vm_s]);
}

// ─── Prologue / epilogue ──────────────────────────────────────────────────────

void jit_prologue(JitBuf *j) {
    // push rbp
    jit_u8(j, 0x55);
    // mov rbp, rsp
    jit_u8(j, rex(1,0,0)); jit_u8(j, 0x89); jit_u8(j, 0xE5);
}

void jit_epilogue(JitBuf *j) {
    jit_u8(j, 0x5D);   // pop rbp
    jit_u8(j, 0xC3);   // ret
}

// ─── Shadow stack ops (r14 = shadow ptr, grows downward) ─────────────────────
// Frame layout in native mode (r14 points at current frame base):
//   [r14+0]  = err trampoline ptr
//   [r14+8]  = ok  trampoline ptr

void jit_emit_call(JitBuf *j, uint8_t *ok_tramp, uint8_t *err_tramp) {
    // sub r14, 16
    jit_u8(j,0x49); jit_u8(j,0x83); jit_u8(j,0xEE); jit_u8(j,0x10);

    // mov rax, err_tramp
    jit_u8(j,0x48); jit_u8(j,0xB8); jit_u64(j,(uint64_t)err_tramp);
    // mov [r14], rax
    jit_u8(j,0x49); jit_u8(j,0x89); jit_u8(j,0x06);

    // mov rax, ok_tramp
    jit_u8(j,0x48); jit_u8(j,0xB8); jit_u64(j,(uint64_t)ok_tramp);
    // mov [r14+8], rax
    jit_u8(j,0x49); jit_u8(j,0x89); jit_u8(j,0x46); jit_u8(j,0x08);

    // jmp rax  (ok_tramp still in rax)
    jit_u8(j,0xFF); jit_u8(j,0xE0);
}

void jit_emit_ret(JitBuf *j) {
    // mov rax, [r14+8]   ; ok ptr
    jit_u8(j,0x49); jit_u8(j,0x8B); jit_u8(j,0x46); jit_u8(j,0x08);
    // add r14, 16
    jit_u8(j,0x49); jit_u8(j,0x83); jit_u8(j,0xC6); jit_u8(j,0x10);
    // jmp rax
    jit_u8(j,0xFF); jit_u8(j,0xE0);
}

void jit_emit_throw(JitBuf *j) {
    // mov rax, [r14]     ; err ptr
    jit_u8(j,0x49); jit_u8(j,0x8B); jit_u8(j,0x06);
    // add r14, 16
    jit_u8(j,0x49); jit_u8(j,0x83); jit_u8(j,0xC6); jit_u8(j,0x10);
    // jmp rax
    jit_u8(j,0xFF); jit_u8(j,0xE0);
}

// ─── Block compiler ───────────────────────────────────────────────────────────

uint8_t *jit_compile_block(JitCtx *ctx, uint32_t bc_offset) {
    Block *b = jit_block_get(ctx, bc_offset);
    if (b->native) return b->native;   // already compiled

    JitBuf *j = &ctx->code;
    uint8_t *block_start = j->buf + j->len;

    uint8_t *ip  = ctx->bc + bc_offset;
    uint8_t *end = ctx->bc + ctx->bc_len;

    while (ip < end) {
        uint8_t op = *ip++;

        switch (op) {

        // ── mov ────────────────────────────────────────────────────────────
        case OP_MOV_RR: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            jit_rr(j, 0x8B, vm2x86[d], vm2x86[s]);  // MOV r64, r/m64
            break;
        }
        case OP_MOV_RI: {
            uint8_t d=(*ip>>4)&7; ip++;
            uint64_t imm; memcpy(&imm, ip, 8); ip+=8;
            jit_mov_ri(j, d, imm);
            break;
        }

        // ── arithmetic ─────────────────────────────────────────────────────
        case OP_ADD:  { uint8_t d=(*ip>>4)&7,s=(*ip)&7; ip++; jit_alu_rr(j,0x03,d,s); break; }
        case OP_SUB:  { uint8_t d=(*ip>>4)&7,s=(*ip)&7; ip++; jit_alu_rr(j,0x2B,d,s); break; }
        case OP_AND:  { uint8_t d=(*ip>>4)&7,s=(*ip)&7; ip++; jit_alu_rr(j,0x23,d,s); break; }
        case OP_OR:   { uint8_t d=(*ip>>4)&7,s=(*ip)&7; ip++; jit_alu_rr(j,0x0B,d,s); break; }
        case OP_XOR:  { uint8_t d=(*ip>>4)&7,s=(*ip)&7; ip++; jit_alu_rr(j,0x33,d,s); break; }
        case OP_MUL:
        case OP_IMUL: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            uint8_t rd=vm2x86[d], rs=vm2x86[s];
            // IMUL r64, r/m64  (0F AF /r)
            jit_u8(j, rex(1,(rd>>3)&1,(rs>>3)&1));
            jit_u8(j, 0x0F); jit_u8(j, 0xAF);
            jit_u8(j, (uint8_t)(0xC0|((rd&7)<<3)|(rs&7)));
            break;
        }
        case OP_NEG: {
            uint8_t d=(*ip>>4)&7; ip++;
            uint8_t rd=vm2x86[d];
            jit_u8(j, rex(1,0,(rd>>3)&1));
            jit_u8(j, 0xF7);
            jit_u8(j, (uint8_t)(0xD8|(rd&7)));  // NEG r/m64 /3
            break;
        }
        case OP_SHL: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            // mov cl, src_low_byte  (rcx = shift amount)
            jit_rr(j, 0x89, X86_RCX, vm2x86[s]);   // mov rcx, src
            uint8_t rd=vm2x86[d];
            jit_u8(j, rex(1,0,(rd>>3)&1)); jit_u8(j,0xD3);
            jit_u8(j, (uint8_t)(0xE0|(rd&7)));      // SHL r/m64, CL
            break;
        }
        case OP_SHR: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            jit_rr(j, 0x89, X86_RCX, vm2x86[s]);
            uint8_t rd=vm2x86[d];
            jit_u8(j, rex(1,0,(rd>>3)&1)); jit_u8(j,0xD3);
            jit_u8(j, (uint8_t)(0xE8|(rd&7)));      // SHR r/m64, CL
            break;
        }
        case OP_SAR: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            jit_rr(j, 0x89, X86_RCX, vm2x86[s]);
            uint8_t rd=vm2x86[d];
            jit_u8(j, rex(1,0,(rd>>3)&1)); jit_u8(j,0xD3);
            jit_u8(j, (uint8_t)(0xF8|(rd&7)));      // SAR r/m64, CL
            break;
        }

        // ── float ──────────────────────────────────────────────────────────
        case OP_FADD: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            // ADDSD xmm_d, xmm_s  (F2 0F 58 /r)
            jit_u8(j,0xF2);
            if(d>=8||s>=8) jit_u8(j,rex(0,(d>>3)&1,(s>>3)&1));
            jit_u8(j,0x0F); jit_u8(j,0x58);
            jit_u8(j,(uint8_t)(0xC0|((d&7)<<3)|(s&7)));
            break;
        }
        case OP_FMUL: {
            uint8_t d=(*ip>>4)&7, s=(*ip)&7; ip++;
            // MULSD  (F2 0F 59 /r)
            jit_u8(j,0xF2);
            if(d>=8||s>=8) jit_u8(j,rex(0,(d>>3)&1,(s>>3)&1));
            jit_u8(j,0x0F); jit_u8(j,0x59);
            jit_u8(j,(uint8_t)(0xC0|((d&7)<<3)|(s&7)));
            break;
        }

        // ── call / ret / throw ─────────────────────────────────────────────
        case OP_CALL: {
            int32_t ok_off, err_off;
            memcpy(&ok_off,  ip,   4); ip+=4;
            memcpy(&err_off, ip,   4); ip+=4;
            uint8_t *ok_t  = jit_tramp_for(ctx, (uint32_t)ok_off);
            uint8_t *err_t = jit_tramp_for(ctx, (uint32_t)err_off);
            jit_emit_call(j, ok_t, err_t);
            // CALL ends this block — next bytecode is a new block
            goto block_end;
        }
        case OP_RET:   jit_emit_ret(j);   goto block_end;
        case OP_THROW: jit_emit_throw(j); goto block_end;

        case OP_HALT:
            jit_epilogue(j);
            goto block_end;

        default:
            fprintf(stderr, "jit: unhandled op 0x%02x at bc+%u\n",
                    op, (uint32_t)(ip-1-ctx->bc));
            abort();
        }
    }

block_end:
    jit_buf_make_exec(j);
    b->native = block_start;
    jit_tramp_patch(b->trampoline, block_start);
    return block_start;
}
