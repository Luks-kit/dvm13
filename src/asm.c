#include "asm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ─── Buf helpers ─────────────────────────────────────────────────────────────

static void buf_u8 (Buf *b, uint8_t  v) { b->buf[b->len++] = v; }
static void buf_u16(Buf *b, uint16_t v) { memcpy(b->buf+b->len,&v,2); b->len+=2; }
static void buf_u32(Buf *b, uint32_t v) { memcpy(b->buf+b->len,&v,4); b->len+=4; }
static void buf_i32(Buf *b, int32_t  v) { memcpy(b->buf+b->len,&v,4); b->len+=4; }
static void buf_u64(Buf *b, uint64_t v) { memcpy(b->buf+b->len,&v,8); b->len+=8; }

static Buf *sec_buf(AsmCtx *ctx, DvmSection sec) {
    switch (sec) {
    case DVM_SEC_CNST: return &ctx->cnst;
    case DVM_SEC_DATA: return &ctx->data;
    default:           return &ctx->text;
    }
}

// ─── Label table ─────────────────────────────────────────────────────────────

AsmLabel *asm_label_find(LabelTable *lt, const char *name) {
    for (size_t i = 0; i < lt->nlabels; i++)
        if (!strcmp(lt->labels[i].name, name))
            return &lt->labels[i];
    return NULL;
}

static AsmEqu *equ_find(LabelTable *lt, const char *name) {
    for (size_t i = 0; i < lt->nequs; i++)
        if (!strcmp(lt->equs[i].name, name))
            return &lt->equs[i];
    return NULL;
}

static AsmLabel *label_define(LabelTable *lt, const char *name,
                               uint8_t flags, DvmSection sec, int32_t off) {
    AsmLabel *l = asm_label_find(lt, name);
    if (l) {
        // Resolve a previously-declared extern/forward ref
        if (l->flags == DVM_SYM_EXTERN && flags != DVM_SYM_EXTERN) {
            l->flags   = flags;
            l->section = sec;
            l->offset  = off;
        } else if (l->offset < 0) {
            l->flags = flags; l->section = sec; l->offset = off;
        }
        return l;
    }
    if (lt->nlabels >= ASM_MAX_LABELS) {
        fputs("asm: label table full\n", stderr); exit(1);
    }
    l = &lt->labels[lt->nlabels++];
    snprintf(l->name, ASM_LABEL_LEN, "%s", name);
    l->flags   = flags;
    l->section = sec;
    l->offset  = off;
    return l;
}

// Get or create a symbol index (for reloc emission)
static uint32_t sym_index(AsmCtx *ctx, const char *name) {
    LabelTable *lt = &ctx->lt;
    for (size_t i = 0; i < lt->nlabels; i++)
        if (!strcmp(lt->labels[i].name, name))
            return (uint32_t)i;
    // Create a forward/extern placeholder
    AsmLabel *l = label_define(lt, name, DVM_SYM_EXTERN, DVM_SEC_NONE, -1);
    return (uint32_t)(l - lt->labels);
}

// Emit a 4-byte absolute code offset reference (jumps/calls)
static void label_ref_code(AsmCtx *ctx, const char *name, int line) {
    LabelTable *lt = &ctx->lt;
    AsmLabel   *l  = asm_label_find(lt, name);
    if (l && l->offset >= 0 && l->section == DVM_SEC_CODE) {
        buf_i32(&ctx->text, l->offset);
        return;
    }
    if (lt->npatches >= ASM_MAX_PATCHES) {
        fputs("asm: patch table full\n", stderr); exit(1);
    }
    AsmPatch *p = &lt->patches[lt->npatches++];
    snprintf(p->name, ASM_LABEL_LEN, "%s", name);
    p->patch_at = ctx->text.len;
    p->is64     = 0;
    p->line     = line;
    buf_i32(&ctx->text, 0);
}

// Emit an 8-byte data/extern reference → produces a reloc entry
static void label_ref_data(AsmCtx *ctx, const char *name, int line) {
    if (ctx->nrels >= DVM_MAX_RELS) {
        fputs("asm: reloc table full\n", stderr); exit(1);
    }
    DvmRel *r    = &ctx->rels[ctx->nrels++];
    r->code_offset = (uint32_t)ctx->text.len;
    r->sym_index   = sym_index(ctx, name);

    // If symbol is already defined, we're done; placeholder patched at load
    // If undefined (extern/forward), recorded and resolved in patches_resolve
    AsmLabel *l = asm_label_find(&ctx->lt, name);
    if (!l || l->offset < 0) {
        // Forward ref: also add to patch list so we can verify it's defined
        if (ctx->lt.npatches >= ASM_MAX_PATCHES) {
            fputs("asm: patch table full\n", stderr); exit(1);
        }
        AsmPatch *p = &ctx->lt.patches[ctx->lt.npatches++];
        snprintf(p->name, ASM_LABEL_LEN, "%s", name);
        p->patch_at = ctx->text.len;
        p->is64     = 1;
        p->line     = line;
    }
    buf_u64(&ctx->text, 0); // placeholder; loader/linker patches
}

static int patches_resolve(AsmCtx *ctx) {
    LabelTable *lt = &ctx->lt;
    int ok = 1;
    for (size_t i = 0; i < lt->npatches; i++) {
        AsmPatch *p = &lt->patches[i];
        AsmLabel *l = asm_label_find(lt, p->name);
        if (!l) {
            fprintf(stderr,"asm: undefined label '%s' (line %d)\n",p->name,p->line);
            ok = 0; continue;
        }
        if (p->is64) {
            // Data/extern ref — if still EXTERN, it's fine (linker resolves)
            // If it was supposed to be local and is still undefined → error
            if (l->flags != DVM_SYM_EXTERN && l->offset < 0) {
                fprintf(stderr,"asm: undefined data label '%s' (line %d)\n",p->name,p->line);
                ok = 0;
            }
            // sym_index already set in the DvmRel; nothing else to patch here
        } else {
            // Code offset — must be a defined local code label
            if (l->section != DVM_SEC_CODE || l->offset < 0) {
                fprintf(stderr,"asm: undefined/non-code label '%s' (line %d)\n",p->name,p->line);
                ok = 0; continue;
            }
            memcpy(ctx->text.buf + p->patch_at, &l->offset, 4);
        }
    }
    return ok;
}

// ─── asm_to_prog ──────────────────────────────────────────────────────────────

void asm_to_prog(const AsmCtx *ctx, DvmProg *prog) {
    memset(prog, 0, sizeof(*prog));
    prog->cnst = (uint8_t*)ctx->cnst.buf; prog->cnst_len = ctx->cnst.len;
    prog->data = (uint8_t*)ctx->data.buf; prog->data_len = ctx->data.len;
    prog->code = (uint8_t*)ctx->text.buf; prog->code_len = ctx->text.len;

    for (size_t i = 0; i < ctx->lt.nlabels && prog->nsyms < DVM_MAX_SYMS; i++) {
        const AsmLabel *l = &ctx->lt.labels[i];
        if (l->offset < 0 && l->flags != DVM_SYM_EXTERN) continue;
        DvmSym *s = &prog->syms[prog->nsyms++];
        snprintf(s->name, DVM_SYM_NAME, "%s", l->name);
        s->flags   = l->flags;
        s->section = (l->flags == DVM_SYM_EXTERN) ? DVM_SEC_NONE : l->section;
        s->offset  = (l->offset >= 0) ? (uint32_t)l->offset : 0;
    }

    prog->nrels = ctx->nrels;
    memcpy(prog->rels, ctx->rels, ctx->nrels * sizeof(DvmRel));
}

// ─── Tokenizer ────────────────────────────────────────────────────────────────

static char *skip_ws(char *p) { while (*p==' '||*p=='\t') p++; return p; }

static char *read_tok(char *p, char *out, size_t max) {
    p = skip_ws(p);
    size_t i = 0;
    while (*p && *p!=',' && *p!=' ' && *p!='\t' &&
           *p!='\n' && *p!=';' && *p!=':' && i < max-1)
        out[i++] = *p++;
    out[i] = 0;
    return p;
}

static char *expect_comma(char *p, int line) {
    p = skip_ws(p);
    if (*p != ',') { fprintf(stderr,"asm line %d: expected ','\n",line); exit(1); }
    return p+1;
}

static int parse_reg(const char *s) {
    if (!strcmp(s,"ax")) return REG_AX; 
    if (!strcmp(s,"cx")) return REG_CX; 
    if (!strcmp(s,"di")) return REG_DI; 
    if (!strcmp(s,"ex")) return REG_EX;
    if (!strcmp(s,"bx")) return REG_BX;
    if (!strcmp(s,"dx")) return REG_DX;
    if (!strcmp(s,"si")) return REG_SI;
    if (!strcmp(s,"fx")) return REG_FX;

    return -1;
}

static int parse_fpreg(const char *s) {
    if (s[0]=='f'&&s[1]=='p'&&s[2]>='0'&&s[2]<='7'&&!s[3]) return s[2]-'0';
    return -1;
}

static int parse_mem(const char *s) {
    if (s[0]!='[') return -1;
    char inner[16]; size_t i=0;
    const char *p=s+1;
    while (*p&&*p!=']'&&i<15) inner[i++]=*p++;
    inner[i]=0;
    return parse_reg(inner);
}

// Parse immediate or .equ constant.  Returns value, sets *ok=0 on failure.
static uint64_t parse_imm(LabelTable *lt, const char *s, int line, int *ok) {
    *ok = 1;
    // try .equ lookup first
    AsmEqu *e = equ_find(lt, s);
    if (e) return e->value;
    // numeric
    char *end;
    uint64_t v = (s[0]=='0'&&(s[1]=='x'||s[1]=='X'))
        ? strtoull(s,&end,16) : strtoull(s,&end,10);
    if (*end) { *ok=0; }
    return v;
}

static size_t parse_string(const char *s, int line, uint8_t *out, size_t maxout) {
    if (*s!='"') { fprintf(stderr,"asm line %d: expected string\n",line); exit(1); }
    s++;
    size_t n=0;
    while (*s && *s!='"') {
        if (n>=maxout-1) { fprintf(stderr,"asm line %d: string too long\n",line); exit(1); }
        if (*s=='\\') {
            s++;
            switch(*s){
            case 'n': out[n++]='\n'; break; case 't': out[n++]='\t'; break;
            case 'r': out[n++]='\r'; break; case '0': out[n++]='\0'; break;
            case '\\':out[n++]='\\'; break; case '"': out[n++]='"'; break;
            default:  out[n++]=(uint8_t)*s; break;
            }
        } else { out[n++]=(uint8_t)*s; }
        s++;
    }
    return n;
}

// Detect whether a token looks like a label name (not a number, not a register)
static int is_label_ref(LabelTable *lt, const char *s) {
    if (!s[0]) return 0;
    if (s[0]=='0'&&(s[1]=='x'||s[1]=='X')) return 0;  // hex number
    if (isdigit((unsigned char)s[0])) return 0;         // decimal number
    if (parse_reg(s)>=0 || parse_fpreg(s)>=0) return 0; // register name
    if (parse_mem(s)>=0) return 0;                       // [reg]
    if (equ_find(lt,s)) return 0;                        // .equ constant
    return 1;
}

#define REQ_REG(s,ln) ({ int _r=parse_reg(s); \
    if(_r<0){fprintf(stderr,"asm line %d: expected register, got '%s'\n",(ln),(s));exit(1);} _r; })
#define REQ_FPREG(s,ln) ({ int _r=parse_fpreg(s); \
    if(_r<0){fprintf(stderr,"asm line %d: expected fp register, got '%s'\n",(ln),(s));exit(1);} _r; })
#define RB(d,s) ((uint8_t)(((d)<<4)|((s)&0xf)))

// ─── asm_compile ──────────────────────────────────────────────────────────────

int asm_compile(const char *src, AsmCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    char       line_buf[512];
    const char *cursor = src;
    int         lineno = 0;
    DvmSection  cursec = DVM_SEC_CODE;

    while (*cursor) {
        size_t i=0;
        while (*cursor && *cursor!='\n' && i<510) line_buf[i++]=*cursor++;
        if (*cursor=='\n') cursor++;
        line_buf[i]=0;
        lineno++;

        char *p = skip_ws(line_buf);
        if (!*p || *p==';') continue;

        // ── Section / directive ───────────────────────────────────────────────
        if (p[0]=='.') {
            char dir[64]; char *ap = read_tok(p,dir,64);
            if (!strcmp(dir,".text"))  { cursec=DVM_SEC_CODE; continue; }
            if (!strcmp(dir,".data"))  { cursec=DVM_SEC_DATA; continue; }
            if (!strcmp(dir,".cnst") || !strcmp(dir,".rodata")) {
                cursec=DVM_SEC_CNST; continue;
            }
            if (!strcmp(dir,".equ")) {
                // .equ name, value
                char name[64]; ap=read_tok(ap,name,64); ap=expect_comma(ap,lineno);
                char val[64];  ap=read_tok(ap,val,64);
                if (ctx->lt.nequs >= ASM_MAX_EQUS) {
                    fputs("asm: too many .equ definitions\n",stderr); return 0;
                }
                int ok; uint64_t v = parse_imm(&ctx->lt,val,lineno,&ok);
                if (!ok) {
                    fprintf(stderr,"asm line %d: bad .equ value '%s'\n",lineno,val);
                    return 0;
                }
                AsmEqu *e = &ctx->lt.equs[ctx->lt.nequs++];
                snprintf(e->name,ASM_LABEL_LEN,"%s",name);
                e->value = v;
                continue;
            }
            fprintf(stderr,"asm line %d: unknown directive '%s'\n",lineno,dir);
            return 0;
        }

        // ── Symbol visibility ─────────────────────────────────────────────────
        char tok[64]; char *after = read_tok(p,tok,64); after=skip_ws(after);

        if (!strcmp(tok,"global")) {
            char name[64]; read_tok(after,name,64);
            AsmLabel *l = asm_label_find(&ctx->lt, name);
            if (l) l->flags = DVM_SYM_GLOBAL;
            else   label_define(&ctx->lt,name,DVM_SYM_GLOBAL,cursec,-1);
            continue;
        }
        if (!strcmp(tok,"extern")) {
            char name[64]; read_tok(after,name,64);
            label_define(&ctx->lt,name,DVM_SYM_EXTERN,DVM_SEC_NONE,-1);
            continue;
        }

        // ── Label definition ──────────────────────────────────────────────────
        if (*after==':') {
            // Determine flags: if previously declared global, keep it
            AsmLabel *existing = asm_label_find(&ctx->lt, tok);
            uint8_t flags = (existing && existing->flags==DVM_SYM_GLOBAL)
                            ? DVM_SYM_GLOBAL : DVM_SYM_LOCAL;
            label_define(&ctx->lt, tok, flags, cursec,
                         (int32_t)sec_buf(ctx,cursec)->len);
            p = skip_ws(after+1);
            if (!*p || *p==';') continue;
            after = read_tok(p,tok,64); after=skip_ws(after);
        }

        char mnem[64]; snprintf(mnem,64,"%s",tok); p=after;
        Buf *sec = sec_buf(ctx, cursec);

        // ── Data pseudo-ops ───────────────────────────────────────────────────
        if (cursec != DVM_SEC_CODE) {
            int ok; uint64_t v;
            char a[64];
            if (!strcmp(mnem,"db")) {
                p=read_tok(p,a,64); v=parse_imm(&ctx->lt,a,lineno,&ok);
                if(!ok){fprintf(stderr,"asm line %d: bad db value\n",lineno);return 0;}
                buf_u8(sec,(uint8_t)v); continue;
            }
            if (!strcmp(mnem,"dw")) {
                p=read_tok(p,a,64); v=parse_imm(&ctx->lt,a,lineno,&ok);
                buf_u16(sec,(uint16_t)v); continue;
            }
            if (!strcmp(mnem,"dd")) {
                p=read_tok(p,a,64); v=parse_imm(&ctx->lt,a,lineno,&ok);
                buf_u32(sec,(uint32_t)v); continue;
            }
            if (!strcmp(mnem,"dq")) {
                p=read_tok(p,a,64); v=parse_imm(&ctx->lt,a,lineno,&ok);
                buf_u64(sec,v); continue;
            }
            if (!strcmp(mnem,"ds")||!strcmp(mnem,"dsz")) {
                p=skip_ws(p);
                uint8_t sbuf[256];
                size_t slen=parse_string(p,lineno,sbuf,sizeof(sbuf));
                for(size_t j=0;j<slen;j++) buf_u8(sec,sbuf[j]);
                if(!strcmp(mnem,"dsz")) buf_u8(sec,0);
                continue;
            }
            fprintf(stderr,"asm line %d: invalid pseudo-op '%s' in data section\n",lineno,mnem);
            return 0;
        }

        // ── Code instructions ─────────────────────────────────────────────────

        if (!strcmp(mnem,"mov")) {
            char a[64],b[64];
            p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64);
            int mem_d=parse_mem(a), mem_s=parse_mem(b);
            if (mem_d>=0) {
                buf_u8(sec,OP_MOV_MR); buf_u8(sec,RB(mem_d,REQ_REG(b,lineno)));
            } else if (mem_s>=0) {
                buf_u8(sec,OP_MOV_RM); buf_u8(sec,RB(REQ_REG(a,lineno),mem_s));
            } else {
                int rd=REQ_REG(a,lineno);
                int rs=parse_reg(b);
                if (rs>=0) {
                    buf_u8(sec,OP_MOV_RR); buf_u8(sec,RB(rd,rs));
                } else if (is_label_ref(&ctx->lt,b)) {
                    buf_u8(sec,OP_MOV_RI); buf_u8(sec,RB(rd,0));
                    label_ref_data(ctx,b,lineno);
                } else {
                    int ok; uint64_t v=parse_imm(&ctx->lt,b,lineno,&ok);
                    if(!ok){fprintf(stderr,"asm line %d: bad immediate '%s'\n",lineno,b);return 0;}
                    buf_u8(sec,OP_MOV_RI); buf_u8(sec,RB(rd,0)); buf_u64(sec,v);
                }
            }
            continue;
        }

        #define ALU2(nm,op) if(!strcmp(mnem,nm)){ \
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64); \
            buf_u8(sec,op); buf_u8(sec,RB(REQ_REG(a,lineno),REQ_REG(b,lineno))); continue; }
        ALU2("add",OP_ADD)   ALU2("sub",OP_SUB)
        ALU2("mul",OP_MUL)   ALU2("imul",OP_IMUL)
        ALU2("div",OP_DIV)   ALU2("idiv",OP_IDIV)  ALU2("mod",OP_MOD)
        ALU2("and",OP_AND)   ALU2("or",OP_OR)       ALU2("xor",OP_XOR)
        ALU2("shl",OP_SHL)   ALU2("shr",OP_SHR)    ALU2("sar",OP_SAR)
        ALU2("cmp",OP_CMP)   ALU2("test",OP_TEST)
        #undef ALU2

        #define ALU1(nm,op) if(!strcmp(mnem,nm)){ \
            char a[64]; p=read_tok(p,a,64); \
            buf_u8(sec,op); buf_u8(sec,RB(REQ_REG(a,lineno),0)); continue; }
        ALU1("neg",OP_NEG) ALU1("not",OP_NOT)
        ALU1("push",OP_PUSH) ALU1("pop",OP_POP)
        #undef ALU1

        #define JMP1(nm,op) if(!strcmp(mnem,nm)){ \
            char a[64]; p=read_tok(p,a,64); \
            buf_u8(sec,op); label_ref_code(ctx,a,lineno); continue; }
        JMP1("jmp",OP_JMP) JMP1("je",OP_JE)   JMP1("jne",OP_JNE)
        JMP1("jl",OP_JL)   JMP1("jle",OP_JLE)
        JMP1("jg",OP_JG)   JMP1("jge",OP_JGE)
        #undef JMP1

        if (!strcmp(mnem,"enter")) {
            char a[64]; p=read_tok(p,a,64);
            int ok; uint64_t v=parse_imm(&ctx->lt,a,lineno,&ok);
            buf_u8(sec,OP_ENTER); buf_u16(sec,(uint16_t)v); continue;
        }
        if (!strcmp(mnem,"leave"))   { buf_u8(sec,OP_LEAVE);   continue; }
        if (!strcmp(mnem,"ret"))     { buf_u8(sec,OP_RET);     continue; }
        if (!strcmp(mnem,"throw"))   { buf_u8(sec,OP_THROW);   continue; }
        if (!strcmp(mnem,"halt"))    { buf_u8(sec,OP_HALT);    continue; }
        if (!strcmp(mnem,"syscall")) { buf_u8(sec,OP_SYSCALL); continue; }

        if (!strcmp(mnem,"call")) {
            char ok_l[64],err_l[64];
            p=read_tok(p,ok_l,64); p=expect_comma(p,lineno); p=read_tok(p,err_l,64);
            buf_u8(sec,OP_CALL);
            label_ref_code(ctx,ok_l,lineno);
            label_ref_code(ctx,err_l,lineno);
            continue;
        }

        #define FALU2(nm,op) if(!strcmp(mnem,nm)){ \
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64); \
            buf_u8(sec,op); buf_u8(sec,RB(REQ_FPREG(a,lineno),REQ_FPREG(b,lineno))); continue; }
        FALU2("fadd",OP_FADD) FALU2("fsub",OP_FSUB)
        FALU2("fmul",OP_FMUL) FALU2("fdiv",OP_FDIV)
        FALU2("fcmp",OP_FCMP) FALU2("fmov",OP_FMOV_RR)
        #undef FALU2

        if (!strcmp(mnem,"fmovi")) {
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64);
            double dv=strtod(b,NULL); uint64_t raw; memcpy(&raw,&dv,8);
            buf_u8(sec,OP_FMOV_RI); buf_u8(sec,RB(REQ_FPREG(a,lineno),0)); buf_u64(sec,raw);
            continue;
        }
        if (!strcmp(mnem,"fmovm")) {
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64);
            int rs=parse_mem(b);
            if(rs<0){fprintf(stderr,"asm line %d: fmovm expects [reg] src\n",lineno);exit(1);}
            buf_u8(sec,OP_FMOV_RM); buf_u8(sec,RB(REQ_FPREG(a,lineno),rs)); continue;
        }
        if (!strcmp(mnem,"fmovs")) {
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64);
            int rd=parse_mem(a);
            if(rd<0){fprintf(stderr,"asm line %d: fmovs expects [reg] dst\n",lineno);exit(1);}
            buf_u8(sec,OP_FMOV_MR); buf_u8(sec,RB(rd,REQ_FPREG(b,lineno))); continue;
        }
        if (!strcmp(mnem,"itof")) {
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64);
            buf_u8(sec,OP_ITOF); buf_u8(sec,RB(REQ_FPREG(a,lineno),REQ_REG(b,lineno))); continue;
        }
        if (!strcmp(mnem,"ftoi")) {
            char a[64],b[64]; p=read_tok(p,a,64); p=expect_comma(p,lineno); p=read_tok(p,b,64);
            buf_u8(sec,OP_FTOI); buf_u8(sec,RB(REQ_REG(a,lineno),REQ_FPREG(b,lineno))); continue;
        }

        fprintf(stderr,"asm line %d: unknown mnemonic '%s'\n",lineno,mnem);
        return 0;
    }
    return patches_resolve(ctx);
}
