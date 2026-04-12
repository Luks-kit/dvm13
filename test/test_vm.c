#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void vm_run(uint8_t *bytecode, size_t stack_size);

// ─── Result sink ─────────────────────────────────────────────────────────────
// Tests store their result here before HALT.
static uint64_t R[8];

// ─── Hand-assembly helpers ───────────────────────────────────────────────────

typedef struct { uint8_t buf[4096]; size_t len; } Prog;

static void pu8 (Prog *p, uint8_t  v) { p->buf[p->len++] = v; }
static void pu16(Prog *p, uint16_t v) { memcpy(p->buf+p->len,&v,2); p->len+=2; }
static void pi32(Prog *p, int32_t  v) { memcpy(p->buf+p->len,&v,4); p->len+=4; }
static void pu64(Prog *p, uint64_t v) { memcpy(p->buf+p->len,&v,8); p->len+=8; }

// reg-byte: dst high nibble, src low nibble
#define RB(d,s) ((uint8_t)(((d)<<4)|(s)))

// hand-emit: store src_reg into R[slot]
static void store_result(Prog *p, uint8_t src, int slot) {
    // mov di, &R[slot]
    pu8(p, OP_MOV_RI); pu8(p, RB(REG_DI,0)); pu64(p,(uint64_t)&R[slot]);
    // mov [di], src
    pu8(p, OP_MOV_MR); pu8(p, RB(REG_DI, src));
}

// ─── Interpreter tests ───────────────────────────────────────────────────────

static void test_add(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,10);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,32);
    pu8(&p,OP_ADD);    pu8(&p,RB(REG_AX,REG_BX));
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    vm_run(p.buf, 64*1024);
    assert(R[0]==42);
    printf("PASS  test_add            ax=%llu\n",(unsigned long long)R[0]);
}

static void test_loop(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0);   // acc
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,1);   // counter
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_CX,0)); pu64(&p,11);  // limit

    size_t loop_top = p.len;
    pu8(&p,OP_ADD);  pu8(&p,RB(REG_AX,REG_BX));
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_DX,0)); pu64(&p,1);
    pu8(&p,OP_ADD);  pu8(&p,RB(REG_BX,REG_DX));
    pu8(&p,OP_CMP);  pu8(&p,RB(REG_BX,REG_CX));
    pu8(&p,OP_JL);
    size_t patch = p.len; pi32(&p,0);
    int32_t off = (int32_t)loop_top;  // absolute offset
    memcpy(p.buf+patch, &off, 4);

    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    vm_run(p.buf, 64*1024);
    assert(R[0]==55);
    printf("PASS  test_loop           ax=%llu\n",(unsigned long long)R[0]);
}

static void test_call_ret(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,21);

    size_t call_site = p.len;
    pu8(&p,OP_CALL); pi32(&p,0); pi32(&p,0);  // patched below

    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    size_t err_off = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xDEAD);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    size_t fn_off = p.len;
    pu8(&p,OP_MOV_RR); pu8(&p,RB(REG_BX,REG_AX));
    pu8(&p,OP_ADD);    pu8(&p,RB(REG_AX,REG_BX));
    pu8(&p,OP_RET);

    int32_t ok=(int32_t)fn_off, er=(int32_t)err_off;
    memcpy(p.buf+call_site+1, &ok, 4);
    memcpy(p.buf+call_site+5, &er, 4);

    vm_run(p.buf, 64*1024);
    assert(R[0]==42);
    printf("PASS  test_call_ret       ax=%llu\n",(unsigned long long)R[0]);
}

static void test_throw(void) {
    Prog p = {0};
    size_t call_site = p.len;
    pu8(&p,OP_CALL); pi32(&p,0); pi32(&p,0);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xBAD);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    size_t err_off = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xE4404);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    size_t fn_off = p.len;
    pu8(&p,OP_THROW);

    int32_t ok=(int32_t)fn_off, er=(int32_t)err_off;
    memcpy(p.buf+call_site+1, &ok, 4);
    memcpy(p.buf+call_site+5, &er, 4);

    vm_run(p.buf, 64*1024);
    assert(R[0]==0xE4404);
    printf("PASS  test_throw          ax=0x%llX\n",(unsigned long long)R[0]);
}

static void test_nested(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0);

    size_t main_call = p.len;
    pu8(&p,OP_CALL); pi32(&p,0); pi32(&p,0);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    size_t err_off = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xDEAD);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    // inner: ax += 1
    size_t inner_off = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,1);
    pu8(&p,OP_ADD); pu8(&p,RB(REG_AX,REG_BX));
    pu8(&p,OP_RET);

    // outer: ax+=10, call inner, ax+=100, ret
    size_t outer_off = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,10);
    pu8(&p,OP_ADD); pu8(&p,RB(REG_AX,REG_BX));
    size_t outer_call = p.len;
    pu8(&p,OP_CALL); pi32(&p,0); pi32(&p,0);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_BX,0)); pu64(&p,100);
    pu8(&p,OP_ADD); pu8(&p,RB(REG_AX,REG_BX));
    pu8(&p,OP_RET);

    int32_t v;
    v=(int32_t)outer_off; memcpy(p.buf+main_call +1,&v,4);
    v=(int32_t)err_off;   memcpy(p.buf+main_call +5,&v,4);
    v=(int32_t)inner_off; memcpy(p.buf+outer_call+1,&v,4);
    v=(int32_t)err_off;   memcpy(p.buf+outer_call+5,&v,4);

    vm_run(p.buf, 64*1024);
    assert(R[0]==111);
    printf("PASS  test_nested         ax=%llu\n",(unsigned long long)R[0]);
}

static void test_rethrow(void) {
    Prog p = {0};
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0);
    size_t main_call = p.len;
    pu8(&p,OP_CALL); pi32(&p,0); pi32(&p,0);
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xBAD);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    size_t main_err = p.len;
    pu8(&p,OP_MOV_RI); pu8(&p,RB(REG_AX,0)); pu64(&p,0xCAFE);
    store_result(&p, REG_AX, 0);
    pu8(&p,OP_HALT);

    // inner: throw immediately
    size_t inner_off = p.len;
    pu8(&p,OP_THROW);

    // outer: call inner; ok→ret; err→rethrow
    size_t outer_off = p.len;
    size_t outer_call = p.len;
    pu8(&p,OP_CALL); pi32(&p,0); pi32(&p,0);
    pu8(&p,OP_RET);
    size_t outer_err = p.len;
    pu8(&p,OP_THROW);

    int32_t v;
    v=(int32_t)outer_off; memcpy(p.buf+main_call  +1,&v,4);
    v=(int32_t)main_err;  memcpy(p.buf+main_call  +5,&v,4);
    v=(int32_t)inner_off; memcpy(p.buf+outer_call +1,&v,4);
    v=(int32_t)outer_err; memcpy(p.buf+outer_call +5,&v,4);

    vm_run(p.buf, 64*1024);
    assert(R[0]==0xCAFE);
    printf("PASS  test_rethrow        ax=0x%llX\n",(unsigned long long)R[0]);
}

int main(void) {
    printf("─── interpreter ────────────────────────────\n");
    test_add();
    test_loop();
    test_call_ret();
    test_throw();
    test_nested();
    test_rethrow();

    printf("────────────────────────────────────────────\n");
    printf("All tests passed.\n");
    return 0;
}
