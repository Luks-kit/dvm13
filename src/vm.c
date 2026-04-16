#include "vm.h"
#include "syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─── Shadow stack ─────────────────────────────────────────────────────────────
// Per-CALL frame (push order):
//   spush(err_ip)   ← deeper
//   spush(ok_ip)    ← top
//
// RET:   ok = spop(); discard spop(); ip = ok
// THROW: discard spop(); ip = spop()

#define SHADOW_DEPTH 2048

static uint64_t  _shad[SHADOW_DEPTH];
static uint64_t *_stop;

static inline void spush(uint64_t v) {
    if (__builtin_expect(_stop >= _shad + SHADOW_DEPTH, 0)) {
        fputs("dvm: shadow stack overflow\n", stderr);
        abort();
    }
    *_stop++ = v;
}

static inline uint64_t spop(void) {
    if (__builtin_expect(_stop == _shad, 0)) {
        fputs("dvm: shadow stack underflow\n", stderr);
        abort();
    }
    return *--_stop;
}

// ─── Fetch macros ─────────────────────────────────────────────────────────────
// Comma-operator form — safe as function arguments, no stray semicolons.

#define IP (rf->ip)
#define FETCH8()  (IP+=1, *( uint8_t*)(IP-1))
#define FETCH16() (IP+=2, *(uint16_t*)(IP-2))
#define FETCH32() (IP+=4, *(uint32_t*)(IP-4))
#define FETCH64() (IP+=8, *(uint64_t*)(IP-8))
#define FETCHREGS(d,s) do { uint8_t _b=FETCH8(); (d)=(_b>>4)&7; (s)=_b&7; } while(0)

// Indexed GPR access via union
#define G(n)   RF_G(rf, n)
#define S(n,v) RF_S(rf, n, v)

#define NEXT do { uint8_t _op = FETCH8(); goto *optable[_op]; } while(0)

// ─── vm_run ───────────────────────────────────────────────────────────────────

void vm_run(uint8_t *bytecode, size_t stack_size) {
    void *vm_stack = dvm_mmap(stack_size, DVM_MMAP_RW | DVM_MMAP_STACK);

    RF _rf; memset(&_rf, 0, sizeof(_rf));
    RF *rf  = &_rf;
    rf->ip  = (uint64_t)bytecode;
    rf->sp  = (uint64_t)((uint8_t*)vm_stack + stack_size);
    rf->bp  = rf->sp;
    _stop   = _shad;

    static const void *optable[OP_COUNT] = {
        [OP_MOV_RR]  = &&op_mov_rr,  [OP_MOV_RI]  = &&op_mov_ri,
        [OP_MOV_RM]  = &&op_mov_rm,  [OP_MOV_MR]  = &&op_mov_mr,
        [OP_ADD]     = &&op_add,     [OP_SUB]     = &&op_sub,
        [OP_MUL]     = &&op_mul,     [OP_IMUL]    = &&op_imul,
        [OP_DIV]     = &&op_div,     [OP_IDIV]    = &&op_idiv,
        [OP_MOD]     = &&op_mod,     [OP_NEG]     = &&op_neg,
        [OP_AND]     = &&op_and,     [OP_OR]      = &&op_or,
        [OP_XOR]     = &&op_xor,     [OP_NOT]     = &&op_not,
        [OP_SHL]     = &&op_shl,     [OP_SHR]     = &&op_shr,
        [OP_SAR]     = &&op_sar,     [OP_CMP]     = &&op_cmp,
        [OP_TEST]    = &&op_test,    [OP_JMP]     = &&op_jmp,
        [OP_JE]      = &&op_je,      [OP_JNE]     = &&op_jne,
        [OP_JL]      = &&op_jl,      [OP_JLE]     = &&op_jle,
        [OP_JG]      = &&op_jg,      [OP_JGE]     = &&op_jge,
        [OP_PUSH]    = &&op_push,    [OP_POP]     = &&op_pop,
        [OP_ENTER]   = &&op_enter,   [OP_LEAVE]   = &&op_leave,
        [OP_CALL]    = &&op_call,    [OP_RET]     = &&op_ret,
        [OP_THROW]   = &&op_throw,
        [OP_FADD]    = &&op_fadd,    [OP_FSUB]    = &&op_fsub,
        [OP_FMUL]    = &&op_fmul,    [OP_FDIV]    = &&op_fdiv,
        [OP_FCMP]    = &&op_fcmp,    [OP_FMOV_RR] = &&op_fmov_rr,
        [OP_FMOV_RI] = &&op_fmov_ri, [OP_FMOV_RM] = &&op_fmov_rm,
        [OP_FMOV_MR] = &&op_fmov_mr, [OP_ITOF]    = &&op_itof,
        [OP_FTOI]    = &&op_ftoi,    [OP_SYSCALL] = &&op_syscall,
        [OP_HALT]    = &&op_halt,
        // sized loads
        [OP_MOVZX_RM8]  = &&op_movzx_rm8,  [OP_MOVZX_RM16] = &&op_movzx_rm16,
        [OP_MOVZX_RM32] = &&op_movzx_rm32,
        [OP_MOVSX_RM8]  = &&op_movsx_rm8,  [OP_MOVSX_RM16] = &&op_movsx_rm16,
        [OP_MOVSX_RM32] = &&op_movsx_rm32,
        // sized stores
        [OP_MOV_MR8]    = &&op_mov_mr8,    [OP_MOV_MR16]   = &&op_mov_mr16,
        [OP_MOV_MR32]   = &&op_mov_mr32,
        // indexed 64-bit
        [OP_MOV_RM_OFF] = &&op_mov_rm_off, [OP_MOV_MR_OFF] = &&op_mov_mr_off,
        // indexed sized loads zx
        [OP_MOVZX_RM8_OFF]  = &&op_movzx_rm8_off,
        [OP_MOVZX_RM16_OFF] = &&op_movzx_rm16_off,
        [OP_MOVZX_RM32_OFF] = &&op_movzx_rm32_off,
        // indexed sized loads sx
        [OP_MOVSX_RM8_OFF]  = &&op_movsx_rm8_off,
        [OP_MOVSX_RM16_OFF] = &&op_movsx_rm16_off,
        [OP_MOVSX_RM32_OFF] = &&op_movsx_rm32_off,
        // indexed sized stores
        [OP_MOV_MR8_OFF]    = &&op_mov_mr8_off,
        [OP_MOV_MR16_OFF]   = &&op_mov_mr16_off,
        [OP_MOV_MR32_OFF]   = &&op_mov_mr32_off,
        // control
        [OP_CALLR]  = &&op_callr,
        [OP_ALLOCA] = &&op_alloca,
        [OP_LEA]    = &&op_lea,
        // truncation
        [OP_TRUNC8]  = &&op_trunc8,
        [OP_TRUNC16] = &&op_trunc16,
        [OP_TRUNC32] = &&op_trunc32,
    };

    NEXT;

    // ── mov ──────────────────────────────────────────────────────────────────
    op_mov_rr: { uint8_t d,s; FETCHREGS(d,s); S(d,G(s)); NEXT; }
    op_mov_ri: { uint8_t d,s; FETCHREGS(d,s); (void)s; S(d,FETCH64()); NEXT; }
    op_mov_rm: { uint8_t d,s; FETCHREGS(d,s); S(d,*(uint64_t*)G(s)); NEXT; }
    op_mov_mr: { uint8_t d,s; FETCHREGS(d,s); *(uint64_t*)G(d)=G(s); NEXT; }

    // ── arithmetic ───────────────────────────────────────────────────────────
    op_add:  { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)+G(s)); NEXT; }
    op_sub:  { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)-G(s)); NEXT; }
    op_mul:  { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)*G(s)); NEXT; }
    op_imul: { uint8_t d,s; FETCHREGS(d,s);
               S(d,(uint64_t)((int64_t)G(d)*(int64_t)G(s))); NEXT; }
    op_div:  { uint8_t d,s; FETCHREGS(d,s);
               if(!G(s)){fputs("dvm: div/0\n",stderr);abort();}
               S(d,G(d)/G(s)); NEXT; }
    op_idiv: { uint8_t d,s; FETCHREGS(d,s);
               if(!G(s)){fputs("dvm: div/0\n",stderr);abort();}
               S(d,(uint64_t)((int64_t)G(d)/(int64_t)G(s))); NEXT; }
    op_mod:  { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)%G(s)); NEXT; }
    op_neg:  { uint8_t d,s; FETCHREGS(d,s); (void)s;
               S(d,(uint64_t)(-(int64_t)G(d))); NEXT; }

    // ── bitwise ───────────────────────────────────────────────────────────────
    op_and: { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)&G(s)); NEXT; }
    op_or:  { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)|G(s)); NEXT; }
    op_xor: { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)^G(s)); NEXT; }
    op_not: { uint8_t d,s; FETCHREGS(d,s); (void)s; S(d,~G(d)); NEXT; }
    op_shl: { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)<<(G(s)&63)); NEXT; }
    op_shr: { uint8_t d,s; FETCHREGS(d,s); S(d,G(d)>>(G(s)&63)); NEXT; }
    op_sar: { uint8_t d,s; FETCHREGS(d,s);
              S(d,(uint64_t)((int64_t)G(d)>>(G(s)&63))); NEXT; }

    // ── compare ───────────────────────────────────────────────────────────────
    op_cmp: {
        uint8_t d,s; FETCHREGS(d,s);
        int64_t a=(int64_t)G(d), b=(int64_t)G(s);
        rf->fl = 0;
        if (a==b) rf->fl |= FL_ZF;
        if (a< b) rf->fl |= FL_SF;
        NEXT;
    }
    op_test: {
        uint8_t d,s; FETCHREGS(d,s);
        rf->fl = (G(d)&G(s)) ? 0 : FL_ZF;
        NEXT;
    }

    // ── jumps (absolute offsets) ──────────────────────────────────────────────
    #define JABSOL(cond) {                                          \
        int32_t _o=FETCH32();                                       \
        if(cond) IP=(uint64_t)(bytecode+(uint32_t)_o);             \
        NEXT;                                                       \
    }
    op_jmp: { int32_t _o=FETCH32(); IP=(uint64_t)(bytecode+(uint32_t)_o); NEXT; }
    op_je:  JABSOL( (rf->fl & FL_ZF))
    op_jne: JABSOL(!(rf->fl & FL_ZF))
    op_jl:  JABSOL( (rf->fl & FL_SF))
    op_jle: JABSOL( (rf->fl & (FL_SF|FL_ZF)))
    op_jg:  JABSOL(!(rf->fl & (FL_SF|FL_ZF)))
    op_jge: JABSOL(!(rf->fl & FL_SF))
    #undef JABSOL

    // ── data stack ────────────────────────────────────────────────────────────
    op_push:  { uint8_t d,s; FETCHREGS(d,s); (void)s;
                rf->sp-=8; *(uint64_t*)rf->sp=G(d); NEXT; }
    op_pop:   { uint8_t d,s; FETCHREGS(d,s); (void)s;
                S(d,*(uint64_t*)rf->sp); rf->sp+=8; NEXT; }
    op_enter: { uint16_t sz=FETCH16();
                rf->sp-=8; *(uint64_t*)rf->sp=rf->bp;
                rf->bp=rf->sp; rf->sp-=sz; NEXT; }
    op_leave: { rf->sp=rf->bp;
                rf->bp=*(uint64_t*)rf->sp; rf->sp+=8; NEXT; }

    // ── call / ret / throw ────────────────────────────────────────────────────
    op_call: {
        int32_t ok_off  = FETCH32();
        int32_t err_off = FETCH32();
        spush((uint64_t)(bytecode+(uint32_t)err_off));  // err, deeper
        spush(IP);                                      // ok = return addr, top
        IP = (uint64_t)(bytecode+(uint32_t)ok_off);
        NEXT;
    }
    op_ret: {
        uint64_t ok = spop();
        spop();           // discard err
        IP = ok;
        NEXT;
    }
    op_throw: {
        spop();           // discard ok
        IP = spop();      // err addr
        NEXT;
    }

    // ── float ─────────────────────────────────────────────────────────────────
    op_fadd:    { uint8_t d,s; FETCHREGS(d,s); rf->fp[d]+=rf->fp[s]; NEXT; }
    op_fsub:    { uint8_t d,s; FETCHREGS(d,s); rf->fp[d]-=rf->fp[s]; NEXT; }
    op_fmul:    { uint8_t d,s; FETCHREGS(d,s); rf->fp[d]*=rf->fp[s]; NEXT; }
    op_fdiv:    { uint8_t d,s; FETCHREGS(d,s); rf->fp[d]/=rf->fp[s]; NEXT; }
    op_fcmp: {
        uint8_t d,s; FETCHREGS(d,s);
        rf->fl = 0;
        if (rf->fp[d]==rf->fp[s]) rf->fl |= FL_ZF;
        if (rf->fp[d]< rf->fp[s]) rf->fl |= FL_SF;
        NEXT;
    }
    op_fmov_rr: { uint8_t d,s; FETCHREGS(d,s); rf->fp[d]=rf->fp[s]; NEXT; }
    op_fmov_ri: { uint8_t d,s; FETCHREGS(d,s); (void)s;
                  uint64_t raw=FETCH64(); memcpy(&rf->fp[d],&raw,8); NEXT; }
    op_fmov_rm: { uint8_t d,s; FETCHREGS(d,s);
                  memcpy(&rf->fp[d],(void*)G(s),8); NEXT; }
    op_fmov_mr: { uint8_t d,s; FETCHREGS(d,s);
                  memcpy((void*)G(d),&rf->fp[s],8); NEXT; }
    op_itof:    { uint8_t d,s; FETCHREGS(d,s); rf->fp[d]=(double)(int64_t)G(s); NEXT; }
    op_ftoi:    { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)(int64_t)rf->fp[s]); NEXT; }

    // ── syscall ───────────────────────────────────────────────────────────────
    // ax=nr  bx=a1  cx=a2  dx=a3  di=a4  si=a5  →  ax=result
    op_syscall: {
        uint64_t result = 0;
        dvm_syscall(rf->gpr.ax, rf->gpr.bx, rf->gpr.cx,
                    rf->gpr.dx, rf->gpr.di, rf->gpr.si, &result);
        rf->gpr.ax = result;
        NEXT;
    }

    // ── sized loads (zero-extending) ────────────────────────────────────────────
    op_movzx_rm8:  { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)*( uint8_t*)G(s)); NEXT; }
    op_movzx_rm16: { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)*(uint16_t*)G(s)); NEXT; }
    op_movzx_rm32: { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)*(uint32_t*)G(s)); NEXT; }

    // ── sized loads (sign-extending) ─────────────────────────────────────────────
    op_movsx_rm8:  { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)(int64_t)*( int8_t*)G(s)); NEXT; }
    op_movsx_rm16: { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)(int64_t)*(int16_t*)G(s)); NEXT; }
    op_movsx_rm32: { uint8_t d,s; FETCHREGS(d,s); S(d,(uint64_t)(int64_t)*(int32_t*)G(s)); NEXT; }

    // ── sized stores (truncating) ─────────────────────────────────────────────────
    op_mov_mr8:  { uint8_t d,s; FETCHREGS(d,s); *( uint8_t*)G(d) = (uint8_t) G(s); NEXT; }
    op_mov_mr16: { uint8_t d,s; FETCHREGS(d,s); *(uint16_t*)G(d) = (uint16_t)G(s); NEXT; }
    op_mov_mr32: { uint8_t d,s; FETCHREGS(d,s); *(uint32_t*)G(d) = (uint32_t)G(s); NEXT; }

    // ── indexed 64-bit ────────────────────────────────────────────────────────────
    op_mov_rm_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                     S(d,*(uint64_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }
    op_mov_mr_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                     *(uint64_t*)(G(d)+(uint64_t)(int64_t)o) = G(s); NEXT; }

    // ── indexed sized loads (zero-extending) ─────────────────────────────────────
    op_movzx_rm8_off:  { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                          S(d,(uint64_t)*( uint8_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }
    op_movzx_rm16_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                          S(d,(uint64_t)*(uint16_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }
    op_movzx_rm32_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                          S(d,(uint64_t)*(uint32_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }

    // ── indexed sized loads (sign-extending) ─────────────────────────────────────
    op_movsx_rm8_off:  { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                          S(d,(uint64_t)(int64_t)*( int8_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }
    op_movsx_rm16_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                          S(d,(uint64_t)(int64_t)*(int16_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }
    op_movsx_rm32_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                          S(d,(uint64_t)(int64_t)*(int32_t*)(G(s)+(uint64_t)(int64_t)o)); NEXT; }

    // ── indexed sized stores ─────────────────────────────────────────────────────
    op_mov_mr8_off:  { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                        *( uint8_t*)(G(d)+(uint64_t)(int64_t)o) = (uint8_t) G(s); NEXT; }
    op_mov_mr16_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                        *(uint16_t*)(G(d)+(uint64_t)(int64_t)o) = (uint16_t)G(s); NEXT; }
    op_mov_mr32_off: { uint8_t d,s; FETCHREGS(d,s); int32_t o=FETCH32();
                        *(uint32_t*)(G(d)+(uint64_t)(int64_t)o) = (uint32_t)G(s); NEXT; }

    // ── indirect call ─────────────────────────────────────────────────────────────
    // callr dst, err_off:i32 — ip = GPR[dst]; ok=ip-after-fetch pushed on shadow stack
    op_callr: {
        uint8_t d,s; FETCHREGS(d,s); (void)s;
        int32_t err_off = FETCH32();
        uint64_t target = G(d);
        spush((uint64_t)(bytecode + (uint32_t)err_off));  // err, deeper
        spush(IP);                                        // ok = return addr
        IP = target;
        NEXT;
    }

    // ── dynamic stack ─────────────────────────────────────────────────────────────
    // alloca: sp -= (ax rounded up to multiple of 8), ax = new sp (ptr to block)
    op_alloca: {
        uint64_t sz = (rf->gpr.ax + 7) & ~(uint64_t)7;   // round up to 8
        rf->sp -= sz;
        rf->gpr.ax = rf->sp;
        NEXT;
    }

    // ── load effective address ────────────────────────────────────────────────────
    // lea dst, [base+off] — dst = GPR[base] + sign-extended imm32
    op_lea: {
        uint8_t d,s; FETCHREGS(d,s);
        int32_t off = FETCH32();
        S(d, G(s) + (uint64_t)(int64_t)off);
        NEXT;
    }

    // ── truncation ────────────────────────────────────────────────────────────────
    op_trunc8:  { uint8_t d,s; FETCHREGS(d,s); (void)s; S(d, G(d) & 0xFF);         NEXT; }
    op_trunc16: { uint8_t d,s; FETCHREGS(d,s); (void)s; S(d, G(d) & 0xFFFF);       NEXT; }
    op_trunc32: { uint8_t d,s; FETCHREGS(d,s); (void)s; S(d, G(d) & 0xFFFFFFFF);   NEXT; }

    op_halt:
        dvm_munmap(vm_stack, stack_size);
}
