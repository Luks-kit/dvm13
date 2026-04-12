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

    op_halt:
        dvm_munmap(vm_stack, stack_size);
}
