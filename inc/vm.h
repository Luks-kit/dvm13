#pragma once
// ─── dvm — dispatch virtual machine ──────────────────────────────────────────
// Registers:  ax bx cx dx di si ex fx   (general, 64-bit)
//             fp[0..7]                  (float, double)
//             ip sp bp fl               (special)
//
// x86-64 hints (when register pinning is enabled):
//   ax→rax  bx→rbx  cx→rcx  dx→rdx  di→rdi  si→rsi
//   ex→r8   fx→r9   ip→r10  bp→r11  sp→r12  fl→r13
//   r15 = error register (untouched by VM, set before THROW)
//
// Jump / call operands are ABSOLUTE bytecode offsets (never relative).
// CALL ok:i32 err:i32 — push err_ip (deep), push ok_ip (top) onto shadow stack.
// RET   — pop ok_ip → jump; discard err_ip.
// THROW — discard ok_ip; pop err_ip → jump.  (r15 = error value beforehand)

#include <stdint.h>
#include <stddef.h>

// ─── Register IDs (3-bit, 0-7) ───────────────────────────────────────────────
#define REG_AX 0
#define REG_BX 1
#define REG_CX 2
#define REG_DX 3
#define REG_DI 4
#define REG_SI 5
#define REG_EX 6
#define REG_FX 7

// ─── Flags ────────────────────────────────────────────────────────────────────
#define FL_ZF  (1ULL << 0)   // zero / equal
#define FL_SF  (1ULL << 1)   // sign / less-than (signed)
#define FL_CF  (1ULL << 2)   // carry / less-than (unsigned)
#define FL_OF  (1ULL << 3)   // overflow

// ─── Opcodes ─────────────────────────────────────────────────────────────────
typedef enum __attribute__((packed)) {
    // mov                              encoding
    OP_MOV_RR = 0x00,   // dst = src   [op][ds]
    OP_MOV_RI,          // dst = imm64 [op][d_][imm:8]
    OP_MOV_RM,          // dst = [src] [op][ds]
    OP_MOV_MR,          // [dst] = src [op][ds]

    // arithmetic                       [op][ds]
    OP_ADD,   OP_SUB,
    OP_MUL,   OP_IMUL,  // unsigned / signed
    OP_DIV,   OP_IDIV,
    OP_MOD,
    OP_NEG,             //               [op][d_]

    // bitwise                          [op][ds]
    OP_AND,   OP_OR,    OP_XOR,
    OP_NOT,             //               [op][d_]
    OP_SHL,   OP_SHR,   OP_SAR,  // logical / arithmetic shift

    // compare — writes fl only         [op][ds]
    OP_CMP,   OP_TEST,

    // jumps — absolute bytecode offset [op][off:4]
    OP_JMP,
    OP_JE,  OP_JNE,
    OP_JL,  OP_JLE,
    OP_JG,  OP_JGE,

    // data stack                       [op][d_]
    OP_PUSH,  OP_POP,
    OP_ENTER,           // push bp; bp=sp; sp-=imm16  [op][sz:2]
    OP_LEAVE,           //                             [op]

    // control (shadow stack, ROP-safe) [op][ok:4][err:4]
    OP_CALL,
    OP_RET,
    OP_THROW,           // r15 = error value beforehand

    // float fp[0..7]                   [op][ds]
    OP_FADD,  OP_FSUB,  OP_FMUL,  OP_FDIV,
    OP_FCMP,
    OP_FMOV_RR,         // fp[d] = fp[s]
    OP_FMOV_RI,         // fp[d] = imm64 (double bits) [op][d_][imm:8]
    OP_FMOV_RM,         // fp[d] = [reg]
    OP_FMOV_MR,         // [reg] = fp[s]
    OP_ITOF,            // fp[d] = (double)(int64)src   [op][ds]
    OP_FTOI,            // dst   = (int64)fp[s]         [op][ds]

    // system
    OP_SYSCALL,         // ax=dvm_nr  bx..si=args  →  ax=result
    OP_HALT,

    // ── sized memory loads (zero-extending) ──────── [op][ds]
    OP_MOVZX_RM8,       // dst = (uint64_t)*(uint8_t *)[src]
    OP_MOVZX_RM16,      // dst = (uint64_t)*(uint16_t*)[src]
    OP_MOVZX_RM32,      // dst = (uint64_t)*(uint32_t*)[src]

    // ── sized memory loads (sign-extending) ─────── [op][ds]
    OP_MOVSX_RM8,       // dst = (int64_t)*(int8_t *)[src]
    OP_MOVSX_RM16,      // dst = (int64_t)*(int16_t*)[src]
    OP_MOVSX_RM32,      // dst = (int64_t)*(int32_t*)[src]

    // ── sized memory stores (truncating) ─────────── [op][ds]
    OP_MOV_MR8,         // *(uint8_t *)[dst] = src & 0xFF
    OP_MOV_MR16,        // *(uint16_t*)[dst] = src & 0xFFFF
    OP_MOV_MR32,        // *(uint32_t*)[dst] = src & 0xFFFFFFFF

    // ── indexed memory (base + signed imm32 offset) ─ [op][ds][off:4]
    OP_MOV_RM_OFF,      // dst  = *(uint64_t*)(src  + off)
    OP_MOV_MR_OFF,      // *(uint64_t*)(dst + off) = src

    // ── indexed + sized loads (zero-extending) ───── [op][ds][off:4]
    OP_MOVZX_RM8_OFF,
    OP_MOVZX_RM16_OFF,
    OP_MOVZX_RM32_OFF,

    // ── indexed + sized loads (sign-extending) ───── [op][ds][off:4]
    OP_MOVSX_RM8_OFF,
    OP_MOVSX_RM16_OFF,
    OP_MOVSX_RM32_OFF,

    // ── indexed + sized stores ───────────────────── [op][ds][off:4]
    OP_MOV_MR8_OFF,
    OP_MOV_MR16_OFF,
    OP_MOV_MR32_OFF,

    // ── indirect call ────────────────────────────────────────────────
    // callr dst, err_off — ip = GPR[dst]; push err+ok onto shadow stack
    OP_CALLR,           // [op][d_][err:4]  (ok = ip after fetch)

    // ── dynamic stack / addressing ───────────────────────────────────
    // alloca: sp -= ax (rounded up to 8-byte align), ax = new sp
    OP_ALLOCA,          // [op]

    // lea dst, [bp+off] — effective address of bp-relative slot
    OP_LEA,             // [op][d_][off:4]  (signed imm32 offset)

    // ── truncation helpers ────────────────────────────────────────────
    OP_TRUNC8,          // dst &= 0xFF              [op][d_]
    OP_TRUNC16,         // dst &= 0xFFFF            [op][d_]
    OP_TRUNC32,         // dst &= 0xFFFFFFFF        [op][d_]

    OP_COUNT
} Op;

// ─── Register file ────────────────────────────────────────────────────────────
// gpr is a union: named fields for readability, array for indexed access.
// The two views are always in sync — pick whichever is cleaner at the call site.

typedef union {
    struct {
        uint64_t ax, bx, cx, dx, di, si, ex, fx;
    };
    uint64_t r[8];  // r[REG_AX] .. r[REG_FX]
} GPR;

typedef struct {
    GPR      gpr;
    uint64_t ip, sp, bp, fl;
    double   fp[8];
} RF;

// Convenience: RF_G(rf,n) and RF_S(rf,n,v) for indexed general reg access
#define RF_G(rf, n)    ((rf)->gpr.r[(n) & 7])
#define RF_S(rf, n, v) ((rf)->gpr.r[(n) & 7] = (v))

// ─── Bytecode buffer (shared by assembler + JIT) ─────────────────────────────
#define DVM_BUF_MAX (256 * 1024)

typedef struct {
    uint8_t buf[DVM_BUF_MAX];
    size_t  len;
} Buf;

// ─── vm_run ───────────────────────────────────────────────────────────────────
void vm_run(uint8_t *bytecode, size_t stack_size);
