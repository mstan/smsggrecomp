/*
 * code_generator.c - Z80 -> C emission for the SMS/GG recompiler.
 *
 * cg_probe: coverage report (what opcodes/CF shapes the ROM actually uses).
 * cg_emit:  the real translator - every discovered function becomes a C
 *           function; every instruction becomes faithful C built on the
 *           verified z80_ops.h semantic core. There are NO stubs: an opcode
 *           the emitter cannot translate is a hard generation error
 *           (cg_fail -> exit), per PRINCIPLES #12.
 *
 * Model (matches the nesrecomp/genesis sibling model):
 *   - One C function per (bank,addr) entry; intra-function jumps are C `goto`
 *     to L_<addr> labels; tail jumps to other entries are `callee(); return;`.
 *   - CALL pushes the Z80 return address onto the modeled stack and C-calls the
 *     callee; RET pops it and `return;`s. So s->sp stays the authentic Z80 SP
 *     (POP-the-return-address idioms work) while the C call stack mirrors the
 *     Z80 call graph.
 *   - Computed transfers (JP (HL)/(IX)/(IY), unknown targets) route through
 *     call_by_address(), which the runner resolves (generated function or
 *     hybrid interpreter) and which logs a dispatch miss on the unknown path.
 *   - Per-instruction T-states accumulate into s->cyc (drives line/frame
 *     timing in the runner).
 */
#include "code_generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define CG_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define CG_MKDIR(p) mkdir(p, 0755)
#endif

/* ======================================================================== *
 *  cg_probe - coverage report (unchanged)                                  *
 * ======================================================================== */

/* histogram key: prefix (0..6) << 8 | opcode */
static inline int hkey(const Z80Insn *in){ return (in->prefix << 8) | in->opcode; }

static const char *PFX[] = {"--","CB","ED","DD","FD","DDCB","FDCB"};

void cg_probe(const SmsRom *rom, const FuncList *fl){
    int n = fl->count;
    uint16_t *entries = (uint16_t*)malloc((size_t)(n>0?n:1)*sizeof(uint16_t));
    for (int i=0;i<n;i++) entries[i]=fl->items[i].addr;

    static int hist[7*256]; memset(hist,0,sizeof(hist));
    long total_insn=0, total_bytes=0;
    int  pfx_count[7]={0};
    int  n_call=0, n_call_cond=0, n_ret=0, n_jump=0, n_jump_cond=0;
    int  n_computed_jump=0;
    int  n_truncated=0, n_illegal=0;
    int  traced_funcs=0;
    uint16_t maxaddr=0;

    for (int i=0;i<n;i++){
        if (!fl->items[i].is_entry) continue;
        TraceResult tr;
        trace_function(rom, fl->items[i].addr, fl->items[i].bank, entries, n, &tr);
        traced_funcs++;
        if (tr.truncated) n_truncated++;
        for (int k=0;k<tr.insn_count;k++){
            const Z80Insn *in=&tr.insns[k].insn;
            hist[hkey(in)]++;
            pfx_count[in->prefix]++;
            total_insn++; total_bytes += in->length;
            if (tr.insns[k].addr > maxaddr) maxaddr = tr.insns[k].addr;
            if (in->illegal) n_illegal++;
            switch (in->cf){
                case Z80_CF_CALL: n_call++; break;
                case Z80_CF_CALL_COND: n_call_cond++; break;
                case Z80_CF_RET: case Z80_CF_RET_COND: n_ret++; break;
                case Z80_CF_JUMP: if (!in->has_target) n_computed_jump++; else n_jump++; break;
                case Z80_CF_JUMP_COND: n_jump_cond++; break;
                default: break;
            }
        }
        trace_free(&tr);
    }
    free(entries);

    int distinct=0; for (int k=0;k<7*256;k++) if (hist[k]) distinct++;

    int bankhist[64]={0}, fixed_entries=0;
    for (int i=0;i<n;i++){
        if (!fl->items[i].is_entry) continue;
        int b=fl->items[i].bank;
        if (b<0) fixed_entries++; else if (b<64) bankhist[b]++;
    }

    printf("\n===== coverage probe =====\n");
    printf("functions (entries):   %d\n", n);
    printf("entries by bank:       fixed(0/1)=%d", fixed_entries);
    for (int b=0;b<64;b++) if (bankhist[b]) printf(" b%d=%d", b, bankhist[b]);
    printf("\n");
    printf("functions traced:      %d\n", traced_funcs);
    printf("instructions decoded:  %ld  (%ld bytes)\n", total_insn, total_bytes);
    printf("distinct (pfx,opcode): %d\n", distinct);
    printf("max code address seen: 0x%04X\n", maxaddr);
    printf("prefix breakdown:      ");
    for (int p=0;p<7;p++) if (pfx_count[p]) printf("%s=%d ", PFX[p], pfx_count[p]);
    printf("\n");
    printf("control flow:          CALL=%d CALLcc=%d RET=%d JP/JR=%d JPcc/JRcc=%d computed-JP=%d\n",
           n_call, n_call_cond, n_ret, n_jump, n_jump_cond, n_computed_jump);
    printf("decode issues:         truncated-funcs=%d illegal-insns=%d\n", n_truncated, n_illegal);

    printf("\ntop opcodes:\n");
    for (int rank=0; rank<20; rank++){
        int best=-1, bestv=0;
        for (int k=0;k<7*256;k++) if (hist[k]>bestv){ bestv=hist[k]; best=k; }
        if (best<0) break;
        printf("  %-4s %02X  x%d\n", PFX[best>>8], best&0xFF, bestv);
        hist[best]=0;
    }
    printf("==========================\n\n");
}

/* ======================================================================== *
 *  cg_emit - the Z80 -> C translator                                       *
 * ======================================================================== */

static const FuncList   *G_FL;   /* function table (target name lookups) */
static const TraceResult*G_TR;   /* trace of the function being emitted  */
static const BankState  *G_BS;   /* live slot-bank state at the current insn */

/* rotating scratch buffers for composed operand expressions */
#define SBSZ 320
static char *sbuf(void){ static char pool[8][SBSZ]; static int i=0; i=(i+1)&7; return pool[i]; }

/* Hard generation error - never emit a stub (PRINCIPLES #12). */
static void cg_fail(const Z80Insn *in){
    fprintf(stderr,
        "[cg_emit] FATAL: unhandled opcode (pfx=%d op=0x%02X \"%s\") - "
        "no stub allowed (PRINCIPLES #12). Fix the emitter.\n",
        in->prefix, in->opcode, in->text);
    exit(3);
}

/* ---- register operand expressions ---- */
/* 8-bit register names by field index (6 = (HL)/(IX+d), handled separately). */
static const char *RN8[8] = {"s->b","s->c","s->d","s->e","s->h","s->l","","s->a"};

typedef struct {
    const char *idx;        /* "s->ix" / "s->iy" under DD/FD, else NULL */
    bool        uses_disp;  /* DD/FD memory form ((IX+d)) */
    int         disp;
} OpCtx;

/* Does this (unprefixed) base opcode reference (HL)? (Mirror of the decoder's
 * predicate; under DD/FD the same set become (IX+d)/(IY+d).) */
static bool uses_hlmem(uint8_t op){
    if (op==0x34 || op==0x35 || op==0x36) return true;   /* INC/DEC/LD (HL),n */
    if (op==0x76) return false;                           /* HALT */
    if ((op & 0xC7)==0x46) return true;                   /* LD r,(HL) */
    if ((op & 0xF8)==0x70) return true;                   /* LD (HL),r */
    if ((op & 0xC7)==0x86) return true;                   /* ALU A,(HL) */
    return false;
}

/* Read expression for 8-bit field f. Memory operands assume a local `ea`. */
static const char *r8r(const OpCtx *c, int f){
    char *b = sbuf();
    if (f==6) snprintf(b,SBSZ,"sms_read8(ea)");
    else if (c->idx && !c->uses_disp && f==4) snprintf(b,SBSZ,"(uint8_t)(%s >> 8)", c->idx);
    else if (c->idx && !c->uses_disp && f==5) snprintf(b,SBSZ,"(uint8_t)(%s)", c->idx);
    else snprintf(b,SBSZ,"%s", RN8[f]);
    return b;
}
/* Full write statement for 8-bit field f := (val). */
static const char *r8w(const OpCtx *c, int f, const char *val){
    char *b = sbuf();
    if (f==6) snprintf(b,SBSZ,"sms_write8(ea, (uint8_t)(%s));", val);
    else if (c->idx && !c->uses_disp && f==4)
        snprintf(b,SBSZ,"%s = (uint16_t)((%s & 0x00FFu) | ((uint16_t)(uint8_t)(%s) << 8));", c->idx,c->idx,val);
    else if (c->idx && !c->uses_disp && f==5)
        snprintf(b,SBSZ,"%s = (uint16_t)((%s & 0xFF00u) | (uint8_t)(%s));", c->idx,c->idx,val);
    else snprintf(b,SBSZ,"%s = (uint8_t)(%s);", RN8[f], val);
    return b;
}

/* 16-bit pair (rp table: 0 BC,1 DE,2 HL/IX,3 SP). */
static const char *rpr(const OpCtx *c, int p){
    char *b = sbuf();
    switch (p){
        case 0: snprintf(b,SBSZ,"z80_bc(s)"); break;
        case 1: snprintf(b,SBSZ,"z80_de(s)"); break;
        case 2: snprintf(b,SBSZ,"%s", c->idx ? c->idx : "z80_hl(s)"); break;
        default:snprintf(b,SBSZ,"s->sp"); break;
    }
    return b;
}
static const char *rpw(const OpCtx *c, int p, const char *val){
    char *b = sbuf();
    switch (p){
        case 0: snprintf(b,SBSZ,"z80_set_bc(s, %s);", val); break;
        case 1: snprintf(b,SBSZ,"z80_set_de(s, %s);", val); break;
        case 2:
            if (c->idx) snprintf(b,SBSZ,"%s = (uint16_t)(%s);", c->idx, val);
            else        snprintf(b,SBSZ,"z80_set_hl(s, %s);", val);
            break;
        default:snprintf(b,SBSZ,"s->sp = (uint16_t)(%s);", val); break;
    }
    return b;
}
/* 16-bit pair for PUSH/POP (rp2 table: 0 BC,1 DE,2 HL/IX,3 AF). */
static const char *rp2r(const OpCtx *c, int p){
    char *b = sbuf();
    switch (p){
        case 0: snprintf(b,SBSZ,"z80_bc(s)"); break;
        case 1: snprintf(b,SBSZ,"z80_de(s)"); break;
        case 2: snprintf(b,SBSZ,"%s", c->idx ? c->idx : "z80_hl(s)"); break;
        default:snprintf(b,SBSZ,"z80_af(s)"); break;
    }
    return b;
}
static const char *rp2w(const OpCtx *c, int p, const char *val){
    char *b = sbuf();
    switch (p){
        case 0: snprintf(b,SBSZ,"z80_set_bc(s, %s);", val); break;
        case 1: snprintf(b,SBSZ,"z80_set_de(s, %s);", val); break;
        case 2:
            if (c->idx) snprintf(b,SBSZ,"%s = (uint16_t)(%s);", c->idx, val);
            else        snprintf(b,SBSZ,"z80_set_hl(s, %s);", val);
            break;
        default:snprintf(b,SBSZ,"z80_set_af(s, %s);", val); break;
    }
    return b;
}

/* ---- condition codes ---- */
static const char *cond(int cc){
    static const char *C[8] = {
        "!(s->f & Z80_FLAG_Z)","(s->f & Z80_FLAG_Z)",
        "!(s->f & Z80_FLAG_C)","(s->f & Z80_FLAG_C)",
        "!(s->f & Z80_FLAG_P)","(s->f & Z80_FLAG_P)",
        "!(s->f & Z80_FLAG_S)","(s->f & Z80_FLAG_S)",
    };
    return C[cc & 7];
}

/* ---- T-state cost (NMOS Z80; conditional taken-extra added at the branch) -- */
static const uint8_t base_cyc[256] = {
/*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/*00*/   4,10, 7, 6, 4, 4, 7, 4, 4,11, 7, 6, 4, 4, 7, 4,
/*10*/   8,10, 7, 6, 4, 4, 7, 4,12,11, 7, 6, 4, 4, 7, 4,
/*20*/   7,10,16, 6, 4, 4, 7, 4, 7,11,16, 6, 4, 4, 7, 4,
/*30*/   7,10,13, 6,11,11,10, 4, 7,11,13, 6, 4, 4, 7, 4,
/*40*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*50*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*60*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*70*/   7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
/*80*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*90*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*A0*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*B0*/   4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
/*C0*/   5,10,10,10,10,11, 7,11, 5,10,10, 0,10,17, 7,11,
/*D0*/   5,10,10,11,10,11, 7,11, 5, 4,10,11,10, 0, 7,11,
/*E0*/   5,10,10,19,10,11, 7,11, 5, 4,10, 4,10, 0, 7,11,
/*F0*/   5,10,10, 4,10,11, 7,11, 5, 6,10, 4,10, 0, 7,11,
};

static int ed_cyc(uint8_t op){
    if (op==0xB0||op==0xB8||op==0xB1||op==0xB9||
        op==0xB2||op==0xBA||op==0xB3||op==0xBB) return 0;   /* repeating: loop adds */
    if ((op>=0xA0&&op<=0xA3)||(op>=0xA8&&op<=0xAB)) return 16; /* one-shot block */
    if ((op&0xC7)==0x40) return 12;   /* IN r,(C)  */
    if ((op&0xC7)==0x41) return 12;   /* OUT (C),r */
    if ((op&0xCF)==0x42) return 15;   /* SBC HL,rp */
    if ((op&0xCF)==0x4A) return 15;   /* ADC HL,rp */
    if ((op&0xCF)==0x43) return 20;   /* LD (nn),rp */
    if ((op&0xCF)==0x4B) return 20;   /* LD rp,(nn) */
    if ((op&0xC7)==0x44) return 8;    /* NEG */
    if ((op&0xC7)==0x45) return 14;   /* RETN/RETI */
    if ((op&0xC7)==0x46) return 8;    /* IM */
    if (op==0x47||op==0x4F||op==0x57||op==0x5F) return 9;   /* LD I/R/A,* */
    if (op==0x67||op==0x6F) return 18; /* RRD/RLD */
    return 8;
}

static int cyc_base(const Z80Insn *in){
    switch (in->prefix){
        case Z80_PFX_CB: { int z=in->opcode&7,x=in->opcode>>6; return (z==6)?(x==1?12:15):8; }
        case Z80_PFX_DDCB: case Z80_PFX_FDCB: return (in->opcode>>6)==1 ? 20 : 23;
        case Z80_PFX_DD: case Z80_PFX_FD:
            return base_cyc[in->opcode] + 4 + (in->uses_disp ? 8 : 0);
        case Z80_PFX_ED: return ed_cyc(in->opcode);
        default: return base_cyc[in->opcode];
    }
}

/* ---- target resolution ---- */
static bool tr_has(uint16_t a){
    for (int i=0;i<G_TR->insn_count;i++) if (G_TR->insns[i].addr==a) return true;
    return false;
}
static const char *name_for_target(uint16_t T){
    /* Resolve under the call site's bank context first (so a banked call like
     * `ld ($FFFE),3 ; call $4006` binds func_4006_b3, not the bank-1 reading). */
    int idx = -1;
    if (G_BS){
        int bank;
        if (bankstate_target(G_BS, T, &bank))
            idx = funclist_find(G_FL, T, bank);
    }
    if (idx < 0) idx = funclist_find(G_FL, T, -1);   /* fixed-region fallback */
    if (idx < 0)
        for (int i=0;i<G_FL->count;i++)
            if (G_FL->items[i].addr==T && G_FL->items[i].is_entry){ idx=i; break; }
    if (idx>=0 && G_FL->items[idx].is_entry) return G_FL->items[idx].name;
    return NULL;
}

/* ---- ALU A,(operand) ---- */
static void emit_alu(FILE *o, int y, const char *V){
    switch (y){
        case 0: fprintf(o,"        s->a = z80_add8(s, s->a, %s, 0);\n", V); break;
        case 1: fprintf(o,"        s->a = z80_add8(s, s->a, %s, z80f_c(s));\n", V); break;
        case 2: fprintf(o,"        s->a = z80_sub8(s, s->a, %s, 0);\n", V); break;
        case 3: fprintf(o,"        s->a = z80_sub8(s, s->a, %s, z80f_c(s));\n", V); break;
        case 4: fprintf(o,"        s->a = z80_and8(s, s->a, %s);\n", V); break;
        case 5: fprintf(o,"        s->a = z80_xor8(s, s->a, %s);\n", V); break;
        case 6: fprintf(o,"        s->a = z80_or8(s, s->a, %s);\n", V); break;
        default:fprintf(o,"        z80_cp8(s, s->a, %s);\n", V); break;
    }
}

/* ---- unprefixed + DD/FD plain group ---- */
static void emit_plain(FILE *o, const Z80Insn *in, const OpCtx *c){
    uint8_t op = in->opcode;
    int x=op>>6, y=(op>>3)&7, z=op&7, p=(op>>4)&3, q=(op>>3)&1;

    if (uses_hlmem(op)){
        if (c->idx) fprintf(o,"        uint16_t ea = (uint16_t)(%s + (%d));\n", c->idx, c->disp);
        else        fprintf(o,"        uint16_t ea = z80_hl(s);\n");
    }

    if (x==0){
        if (z==0){
            if (y==0) fprintf(o,"        ; /* nop */\n");
            else if (y==1) fprintf(o,"        { uint8_t t; t=s->a; s->a=s->a_; s->a_=t; t=s->f; s->f=s->f_; s->f_=t; }\n");
            else cg_fail(in);   /* y 2..7 are control flow */
            return;
        }
        if (z==1){
            if (q==0){ char v[32]; snprintf(v,sizeof v,"0x%04X", in->imm);
                       fprintf(o,"        %s\n", rpw(c,p,v)); }
            else { char v[160]; snprintf(v,sizeof v,"z80_add16(s, %s, %s)",
                       c->idx ? c->idx : "z80_hl(s)", rpr(c,p));
                   fprintf(o,"        %s\n", rpw(c,2,v)); }
            return;
        }
        if (z==2){
            if (q==0){
                if (p==0) fprintf(o,"        sms_write8(z80_bc(s), s->a);\n");
                else if (p==1) fprintf(o,"        sms_write8(z80_de(s), s->a);\n");
                else if (p==2) fprintf(o,"        sms_write16(0x%04X, %s);\n", in->imm, c->idx ? c->idx : "z80_hl(s)");
                else fprintf(o,"        sms_write8(0x%04X, s->a);\n", in->imm);
            } else {
                if (p==0) fprintf(o,"        s->a = sms_read8(z80_bc(s));\n");
                else if (p==1) fprintf(o,"        s->a = sms_read8(z80_de(s));\n");
                else if (p==2){ char v[40]; snprintf(v,sizeof v,"sms_read16(0x%04X)", in->imm);
                                fprintf(o,"        %s\n", rpw(c,2,v)); }
                else fprintf(o,"        s->a = sms_read8(0x%04X);\n", in->imm);
            }
            return;
        }
        if (z==3){ char v[160]; snprintf(v,sizeof v,"(uint16_t)(%s %s 1)", rpr(c,p), q==0?"+":"-");
                   fprintf(o,"        %s\n", rpw(c,p,v)); return; }
        if (z==4){ char v[160]; snprintf(v,sizeof v,"z80_inc8(s, %s)", r8r(c,y));
                   fprintf(o,"        %s\n", r8w(c,y,v)); return; }
        if (z==5){ char v[160]; snprintf(v,sizeof v,"z80_dec8(s, %s)", r8r(c,y));
                   fprintf(o,"        %s\n", r8w(c,y,v)); return; }
        if (z==6){ char v[32]; snprintf(v,sizeof v,"0x%02X", (uint8_t)in->imm);
                   fprintf(o,"        %s\n", r8w(c,y,v)); return; }
        /* z==7 */{
            static const char *R[8] = {"z80_rlca","z80_rrca","z80_rla","z80_rra",
                                       "z80_daa","z80_cpl","z80_scf","z80_ccf"};
            fprintf(o,"        %s(s);\n", R[y]); return;
        }
    }

    if (x==1){   /* LD r,r' (HALT 0x76 handled by caller) */
        char v[160]; snprintf(v,sizeof v,"%s", r8r(c,z));
        fprintf(o,"        %s\n", r8w(c,y,v)); return;
    }

    if (x==2){   /* ALU A,r */
        char v[160]; snprintf(v,sizeof v,"%s", r8r(c,z));
        emit_alu(o,y,v); return;
    }

    /* x==3 */
    if (z==0) cg_fail(in);                  /* RET cc (control flow) */
    if (z==1){
        if (q==0){ char v[40]; snprintf(v,sizeof v,"sms_read16(s->sp)");
                   fprintf(o,"        %s s->sp = (uint16_t)(s->sp + 2);\n", rp2w(c,p,v)); return; }
        if (p==1){ fprintf(o,"        { uint8_t t;"
                     " t=s->b;s->b=s->b_;s->b_=t; t=s->c;s->c=s->c_;s->c_=t;"
                     " t=s->d;s->d=s->d_;s->d_=t; t=s->e;s->e=s->e_;s->e_=t;"
                     " t=s->h;s->h=s->h_;s->h_=t; t=s->l;s->l=s->l_;s->l_=t; }\n"); return; }
        if (p==3){ fprintf(o,"        s->sp = %s;\n", c->idx ? c->idx : "z80_hl(s)"); return; }
        cg_fail(in);                        /* p0 RET, p2 JP (HL): control flow */
    }
    if (z==2) cg_fail(in);                  /* JP cc */
    if (z==3){
        if (y==0 || y==1) cg_fail(in);      /* JP nn / CB prefix */
        if (y==2){ fprintf(o,"        sms_io_out(0x%02X, s->a);\n", (uint8_t)in->imm); return; }
        if (y==3){ fprintf(o,"        s->a = sms_io_in(0x%02X);\n", (uint8_t)in->imm); return; }
        if (y==4){ const char *hl = c->idx ? c->idx : "z80_hl(s)";
                   fprintf(o,"        { uint16_t t = sms_read16(s->sp); sms_write16(s->sp, %s); %s s->wz = t; }\n",
                           hl, rpw(c,2,"t")); return; }
        if (y==5){ fprintf(o,"        { uint8_t t; t=s->d;s->d=s->h;s->h=t; t=s->e;s->e=s->l;s->l=t; }\n"); return; }
        if (y==6){ fprintf(o,"        s->iff1 = s->iff2 = false;\n"); return; }
        fprintf(o,"        s->iff1 = s->iff2 = true; s->ei_block = 1;\n"); return;   /* y==7 EI (1-insn IRQ delay) */
    }
    if (z==4) cg_fail(in);                  /* CALL cc */
    if (z==5){
        if (q==0){ fprintf(o,"        s->sp = (uint16_t)(s->sp - 2); sms_write16(s->sp, %s);\n", rp2r(c,p)); return; }
        cg_fail(in);                        /* CALL nn / DD/ED/FD prefix */
    }
    if (z==6){ char v[32]; snprintf(v,sizeof v,"0x%02X", (uint8_t)in->imm); emit_alu(o,y,v); return; }
    cg_fail(in);                            /* z==7 RST: control flow */
}

/* ---- CB group ---- */
static void emit_cb(FILE *o, const Z80Insn *in){
    uint8_t op=in->opcode; int x=op>>6, y=(op>>3)&7, z=op&7;
    OpCtx c = {NULL,false,0};
    if (z==6) fprintf(o,"        uint16_t ea = z80_hl(s);\n");
    static const char *R[8] = {"z80_rlc","z80_rrc","z80_rl","z80_rr","z80_sla","z80_sra","z80_sll","z80_srl"};
    if (x==0){ char v[160]; snprintf(v,sizeof v,"%s(s, %s)", R[y], r8r(&c,z));
               fprintf(o,"        %s\n", r8w(&c,z,v)); }
    else if (x==1){
        if (z==6) fprintf(o,"        z80_bit(s, %d, sms_read8(ea), (uint8_t)(z80_hl(s) >> 8));\n", y);
        else      fprintf(o,"        z80_bit(s, %d, %s, %s);\n", y, RN8[z], RN8[z]);
    }
    else if (x==2){ char v[160]; snprintf(v,sizeof v,"(uint8_t)(%s & ~(1u << %d))", r8r(&c,z), y);
                    fprintf(o,"        %s\n", r8w(&c,z,v)); }
    else { char v[160]; snprintf(v,sizeof v,"(uint8_t)(%s | (1u << %d))", r8r(&c,z), y);
           fprintf(o,"        %s\n", r8w(&c,z,v)); }
}

/* ---- DDCB / FDCB group: op on (IX+d)/(IY+d), undocumented register copy ---- */
static void emit_ddcb(FILE *o, const Z80Insn *in, const char *idx){
    uint8_t op=in->opcode; int x=op>>6, y=(op>>3)&7, z=op&7;
    fprintf(o,"        uint16_t ea = (uint16_t)(%s + (%d));\n", idx, in->disp);
    fprintf(o,"        uint8_t v = sms_read8(ea);\n");
    if (x==1){ fprintf(o,"        z80_bit(s, %d, v, (uint8_t)(ea >> 8));\n", y); return; }
    static const char *R[8] = {"z80_rlc","z80_rrc","z80_rl","z80_rr","z80_sla","z80_sra","z80_sll","z80_srl"};
    if (x==0)      fprintf(o,"        uint8_t r = %s(s, v);\n", R[y]);
    else if (x==2) fprintf(o,"        uint8_t r = (uint8_t)(v & ~(1u << %d));\n", y);
    else           fprintf(o,"        uint8_t r = (uint8_t)(v | (1u << %d));\n", y);
    fprintf(o,"        sms_write8(ea, r);\n");
    if (z!=6) fprintf(o,"        %s = r;\n", RN8[z]);   /* undoc: also store to reg z */
}

/* ---- ED block ops (LDI..OTDR), one body emitter for all 16 ---- */
static void emit_block_body(FILE *o, uint8_t op, const char *ind){
    int grp = op & 3;             /* 0 LD, 1 CP, 2 IN, 3 OUT */
    bool dec = (op & 0x08) != 0;
    const char *d = dec ? "- 1" : "+ 1";
    if (grp==0){
        fprintf(o,"%s{ uint8_t v = sms_read8(z80_hl(s)); sms_write8(z80_de(s), v);\n", ind);
        fprintf(o,"%s  z80_set_de(s, (uint16_t)(z80_de(s) %s)); z80_set_hl(s, (uint16_t)(z80_hl(s) %s));\n", ind, d, d);
        fprintf(o,"%s  z80_set_bc(s, (uint16_t)(z80_bc(s) - 1));\n", ind);
        fprintf(o,"%s  uint8_t n = (uint8_t)(s->a + v);\n", ind);
        fprintf(o,"%s  s->f = (uint8_t)((s->f & (Z80_FLAG_C|Z80_FLAG_Z|Z80_FLAG_S)) | (z80_bc(s)!=0?Z80_FLAG_P:0) | (n & 0x08) | ((n & 0x02)?Z80_FLAG_Y:0)); }\n", ind);
    } else if (grp==1){
        fprintf(o,"%s{ uint8_t v = sms_read8(z80_hl(s)); uint8_t r = (uint8_t)(s->a - v);\n", ind);
        fprintf(o,"%s  z80_set_hl(s, (uint16_t)(z80_hl(s) %s)); z80_set_bc(s, (uint16_t)(z80_bc(s) - 1));\n", ind, d);
        fprintf(o,"%s  uint8_t hc = ((s->a & 0x0F) < (v & 0x0F)) ? 1u : 0u;\n", ind);
        fprintf(o,"%s  uint8_t n = (uint8_t)(r - hc);\n", ind);
        fprintf(o,"%s  s->f = (uint8_t)((s->f & Z80_FLAG_C) | Z80_FLAG_N | (r & 0x80) | (r==0?Z80_FLAG_Z:0) | (hc?Z80_FLAG_H:0) | (z80_bc(s)!=0?Z80_FLAG_P:0) | (n & 0x08) | ((n & 0x02)?Z80_FLAG_Y:0)); }\n", ind);
    } else if (grp==2){
        const char *cd = dec ? "- 1" : "+ 1";
        fprintf(o,"%s{ uint8_t v = sms_io_in(s->c); sms_write8(z80_hl(s), v);\n", ind);
        fprintf(o,"%s  s->b = (uint8_t)(s->b - 1); z80_set_hl(s, (uint16_t)(z80_hl(s) %s));\n", ind, d);
        fprintf(o,"%s  uint16_t k = (uint16_t)v + (uint16_t)((uint8_t)(s->c %s));\n", ind, cd);
        fprintf(o,"%s  s->f = (uint8_t)(z80_szxy(s->b) | (v & 0x80 ? Z80_FLAG_N:0) | (k > 0xFF ? (Z80_FLAG_H|Z80_FLAG_C):0) | (z80_parity8((uint8_t)((k & 7) ^ s->b)) ? Z80_FLAG_P:0)); }\n", ind);
    } else {
        fprintf(o,"%s{ uint8_t v = sms_read8(z80_hl(s)); s->b = (uint8_t)(s->b - 1);\n", ind);
        fprintf(o,"%s  sms_io_out(s->c, v); z80_set_hl(s, (uint16_t)(z80_hl(s) %s));\n", ind, d);
        fprintf(o,"%s  uint16_t k = (uint16_t)v + (uint16_t)s->l;\n", ind);
        fprintf(o,"%s  s->f = (uint8_t)(z80_szxy(s->b) | (v & 0x80 ? Z80_FLAG_N:0) | (k > 0xFF ? (Z80_FLAG_H|Z80_FLAG_C):0) | (z80_parity8((uint8_t)((k & 7) ^ s->b)) ? Z80_FLAG_P:0)); }\n", ind);
    }
}
static void emit_block(FILE *o, uint8_t op){
    int grp = op & 3; bool rep = (op & 0x10) != 0;
    if (!rep){ emit_block_body(o, op, "        "); return; }
    const char *contc = (grp==0) ? "z80_bc(s) != 0"
                      : (grp==1) ? "z80_bc(s) != 0 && !(s->f & Z80_FLAG_Z)"
                                 : "s->b != 0";
    /* Repeating block ops are INTERRUPTIBLE per iteration on real Z80: the CPU
     * runs one iteration, accounts its cycles, and accepts a pending interrupt
     * between iterations (re-executing the opcode afterwards). Tick per iteration
     * (21 cyc continuing / 16 last) so the VDP advances and IRQs are sampled
     * mid-op exactly like the interpreter — otherwise a long LDIR/OTIR with
     * interrupts enabled defers every VBlank to after it and the timeline
     * diverges from the oracle. */
    fprintf(o,"        do {\n");
    emit_block_body(o, op, "            ");
    fprintf(o,"            sms_tick((%s) ? 21 : 16);\n", contc);
    fprintf(o,"        } while (%s);\n", contc);
}

/* ---- ED group ---- */
static void emit_ed(FILE *o, const Z80Insn *in){
    uint8_t op = in->opcode;
    OpCtx c0 = {NULL,false,0};
    /* IN r,(C) / IN (C) */
    if ((op&0xC7)==0x40){ int r=(op>>3)&7;
        fprintf(o,"        { uint8_t v = sms_io_in(s->c); ");
        if (r!=6) fprintf(o,"%s = v; ", RN8[r]);
        fprintf(o,"s->f = (uint8_t)((s->f & Z80_FLAG_C) | z80_szxy(v) | (z80_parity8(v)?Z80_FLAG_P:0)); }\n");
        return; }
    /* OUT (C),r / OUT (C),0 */
    if ((op&0xC7)==0x41){ int r=(op>>3)&7;
        fprintf(o,"        sms_io_out(s->c, %s);\n", r==6 ? "0" : RN8[r]); return; }
    /* SBC HL,rp */
    if ((op&0xCF)==0x42){ int p=(op>>4)&3;
        fprintf(o,"        z80_set_hl(s, z80_sbc16(s, z80_hl(s), %s));\n", rpr(&c0,p)); return; }
    /* ADC HL,rp */
    if ((op&0xCF)==0x4A){ int p=(op>>4)&3;
        fprintf(o,"        z80_set_hl(s, z80_adc16(s, z80_hl(s), %s));\n", rpr(&c0,p)); return; }
    /* LD (nn),rp */
    if ((op&0xCF)==0x43){ int p=(op>>4)&3;
        fprintf(o,"        sms_write16(0x%04X, %s);\n", in->imm, rpr(&c0,p)); return; }
    /* LD rp,(nn) */
    if ((op&0xCF)==0x4B){ int p=(op>>4)&3; char v[40]; snprintf(v,sizeof v,"sms_read16(0x%04X)", in->imm);
        fprintf(o,"        %s\n", rpw(&c0,p,v)); return; }
    /* NEG */
    if ((op&0xC7)==0x44){ fprintf(o,"        z80_neg(s);\n"); return; }
    /* IM */
    if ((op&0xC7)==0x46){ int m=(op>>3)&3; int im = (m==2)?1 : (m==3)?2 : 0;
        fprintf(o,"        s->im = %d;\n", im); return; }
    switch (op){
        case 0x47: fprintf(o,"        s->i = s->a;\n"); return;
        case 0x4F: fprintf(o,"        s->r = s->a;\n"); return;
        case 0x57: fprintf(o,"        s->a = s->i; s->f = (uint8_t)((s->f & Z80_FLAG_C) | z80_szxy(s->a) | (s->iff2?Z80_FLAG_P:0));\n"); return;
        case 0x5F: fprintf(o,"        s->a = s->r; s->f = (uint8_t)((s->f & Z80_FLAG_C) | z80_szxy(s->a) | (s->iff2?Z80_FLAG_P:0));\n"); return;
        case 0x67: /* RRD */
            fprintf(o,"        { uint8_t m = sms_read8(z80_hl(s)); uint8_t a = s->a;\n");
            fprintf(o,"          sms_write8(z80_hl(s), (uint8_t)((m >> 4) | (a << 4)));\n");
            fprintf(o,"          s->a = (uint8_t)((a & 0xF0) | (m & 0x0F));\n");
            fprintf(o,"          s->f = (uint8_t)((s->f & Z80_FLAG_C) | z80_szxy(s->a) | (z80_parity8(s->a)?Z80_FLAG_P:0)); }\n");
            return;
        case 0x6F: /* RLD */
            fprintf(o,"        { uint8_t m = sms_read8(z80_hl(s)); uint8_t a = s->a;\n");
            fprintf(o,"          sms_write8(z80_hl(s), (uint8_t)((m << 4) | (a & 0x0F)));\n");
            fprintf(o,"          s->a = (uint8_t)((a & 0xF0) | (m >> 4));\n");
            fprintf(o,"          s->f = (uint8_t)((s->f & Z80_FLAG_C) | z80_szxy(s->a) | (z80_parity8(s->a)?Z80_FLAG_P:0)); }\n");
            return;
        default: break;
    }
    if ((op>=0xA0&&op<=0xA3)||(op>=0xA8&&op<=0xAB)||
        (op>=0xB0&&op<=0xB3)||(op>=0xB8&&op<=0xBB)){
        emit_block(o, op); return;
    }
    /* Every remaining ED encoding is an architectural 2-byte no-op. */
    fprintf(o,"        ; /* undocumented ED no-op */\n");
}

/* ---- one non-control instruction ---- */
static void emit_op(FILE *o, const Z80Insn *in){
    if (in->is_halt){ fprintf(o,"        sms_halt();\n"); return; }
    switch (in->prefix){
        case Z80_PFX_CB:   emit_cb(o,in); break;
        case Z80_PFX_DDCB: emit_ddcb(o,in,"s->ix"); break;
        case Z80_PFX_FDCB: emit_ddcb(o,in,"s->iy"); break;
        case Z80_PFX_ED:   emit_ed(o,in); break;
        case Z80_PFX_DD:   { OpCtx c={"s->ix", in->uses_disp, in->disp}; emit_plain(o,in,&c); break; }
        case Z80_PFX_FD:   { OpCtx c={"s->iy", in->uses_disp, in->disp}; emit_plain(o,in,&c); break; }
        default:           { OpCtx c={NULL,false,0}; emit_plain(o,in,&c); break; }
    }
}

/* ---- jump target (goto for intra-function, tail call otherwise) ---- */
static void emit_jump_target(FILE *o, uint16_t T, const char *condexpr, int extra){
    if (tr_has(T)){
        if (condexpr){
            fprintf(o,"        if (%s) { ", condexpr);
            if (extra) fprintf(o,"s->cyc += %d; ", extra);
            fprintf(o,"goto L_%04X; }\n", T);
        } else {
            fprintf(o,"        goto L_%04X;\n", T);
        }
        return;
    }
    const char *nm = name_for_target(T);
    if (condexpr){
        fprintf(o,"        if (%s) { ", condexpr);
        if (extra) fprintf(o,"s->cyc += %d; ", extra);
        if (nm) fprintf(o,"%s(); return; }\n", nm);
        else    fprintf(o,"call_by_address(0x%04X); return; }\n", T);
    } else {
        if (nm) fprintf(o,"        %s(); return;\n", nm);
        else    fprintf(o,"        call_by_address(0x%04X); return;\n", T);
    }
}

/* ---- call target (push return addr + C-call; pop happens at the RET) ---- */
static void emit_call_target(FILE *o, uint16_t T, uint16_t ret, const char *condexpr, int extra){
    const char *nm = name_for_target(T);
    char call[64];
    if (nm) snprintf(call,sizeof call,"%s();", nm);
    else    snprintf(call,sizeof call,"call_by_address(0x%04X);", T);
    if (condexpr){
        fprintf(o,"        if (%s) { s->cyc += %d; s->sp = (uint16_t)(s->sp - 2); sms_write16(s->sp, 0x%04X); %s }\n",
                condexpr, extra, ret, call);
    } else {
        fprintf(o,"        s->sp = (uint16_t)(s->sp - 2); sms_write16(s->sp, 0x%04X); %s\n", ret, call);
    }
}

/* ---- control flow ---- */
static void emit_cf(FILE *o, const Z80Insn *in, uint16_t addr){
    uint16_t next = (uint16_t)(addr + in->length);
    switch (in->cf){
        case Z80_CF_RET:
            if (in->prefix==Z80_PFX_ED) fprintf(o,"        s->iff1 = s->iff2;\n");
            fprintf(o,"        s->sp = (uint16_t)(s->sp + 2); return;\n");
            break;
        case Z80_CF_RET_COND:{ int cc=(in->opcode>>3)&7;
            fprintf(o,"        if (%s) { s->cyc += 6; s->sp = (uint16_t)(s->sp + 2); return; }\n", cond(cc));
            break; }
        case Z80_CF_JUMP:
            if (!in->has_target){
                const char *idx = (in->prefix==Z80_PFX_DD) ? "s->ix"
                                : (in->prefix==Z80_PFX_FD) ? "s->iy" : "z80_hl(s)";
                uint16_t cont;
                if (in->opcode==0xE9 && G_TR && trace_computed_call(G_TR, addr, &cont) && tr_has(cont))
                    fprintf(o,"        call_by_address(%s); goto L_%04X;\n", idx, cont);  /* computed call */
                else
                    fprintf(o,"        call_by_address(%s); return;\n", idx);            /* tail jump */
            } else emit_jump_target(o, in->target, NULL, 0);
            break;
        case Z80_CF_JUMP_COND:
            if (in->prefix==Z80_PFX_NONE && in->opcode==0x10){   /* DJNZ */
                fprintf(o,"        s->b = (uint8_t)(s->b - 1);\n");
                emit_jump_target(o, in->target, "s->b != 0", 5);
            } else if (in->prefix==Z80_PFX_NONE && (in->opcode&0xE7)==0x20){ /* JR cc */
                emit_jump_target(o, in->target, cond((in->opcode>>3)&3), 5);
            } else {                                             /* JP cc */
                emit_jump_target(o, in->target, cond((in->opcode>>3)&7), 0);
            }
            break;
        case Z80_CF_CALL:
            emit_call_target(o, in->target, next, NULL, 0);
            break;
        case Z80_CF_CALL_COND:{ int cc=(in->opcode>>3)&7;
            emit_call_target(o, in->target, next, cond(cc), 7);
            break; }
        default: break;
    }
}

/* ---- one function ---- */
static void emit_function(FILE *o, const SmsRom *rom, const FuncEntry *fe,
                          const uint16_t *ents, int nent){
    TraceResult tr;
    trace_function(rom, fe->addr, fe->bank, ents, nent, &tr);
    G_TR = &tr;

    /* labels: any jump target that lands inside this function body */
    uint16_t *lbl = (uint16_t*)malloc((size_t)(tr.insn_count>0?tr.insn_count:1)*sizeof(uint16_t)+sizeof(uint16_t));
    int nlbl=0;
    for (int k=0;k<tr.insn_count;k++){
        const Z80Insn *in=&tr.insns[k].insn;
        uint16_t lt; bool have=false;
        if ((in->cf==Z80_CF_JUMP || in->cf==Z80_CF_JUMP_COND) && in->has_target){ lt=in->target; have=true; }
        else if (in->cf==Z80_CF_JUMP && !in->has_target && in->opcode==0xE9 &&
                 trace_computed_call(&tr, tr.insns[k].addr, &lt)) have=true;   /* computed-call continuation */
        if (have && tr_has(lt)){
            bool seen=false; for (int j=0;j<nlbl;j++) if (lbl[j]==lt){ seen=true; break; }
            if (!seen) lbl[nlbl++]=lt;
        }
    }

    /* Execution must begin at the ENTRY address. Instructions are emitted in
     * address order, so when the trace contains code below the entry (backward
     * jumps, or the tracer following a jump into lower memory), the first
     * emitted statement is NOT the entry. Jump to the entry label in that case;
     * also force the entry label so the goto resolves. */
    bool need_entry_jump = (tr.insn_count > 0 && tr.insns[0].addr != fe->addr);
    if (need_entry_jump){
        bool seen=false; for (int j=0;j<nlbl;j++) if (lbl[j]==fe->addr){ seen=true; break; }
        if (!seen) lbl[nlbl++]=fe->addr;
    }

    fprintf(o,"void %s(void){\n    Z80State *s = &g_z80; (void)s;\n", fe->name);
    fprintf(o,"    sms_enter(0x%04X);\n", fe->addr);
    if (tr.truncated)
        fprintf(o,"    /* NOTE: trace truncated (hit unmapped/illegal); body covers the\n"
                  "       statically decodable portion. Runtime resolves the rest. */\n");
    if (need_entry_jump)
        fprintf(o,"    goto L_%04X;   /* enter at the function's entry address */\n", fe->addr);

    /* Track slot banks in address order so direct CALL/JP targets resolve under
     * the right bank (mirrors the finder's discovery). */
    BankState bs; bankstate_init(&bs, fe->addr, fe->bank); G_BS = &bs;

    for (int k=0;k<tr.insn_count;k++){
        bankstate_step(&bs, &tr.insns[k], k>0 ? &tr.insns[k-1] : NULL);
        uint16_t a=tr.insns[k].addr; const Z80Insn *in=&tr.insns[k].insn;
        bool islbl=false; for (int j=0;j<nlbl;j++) if (lbl[j]==a){ islbl=true; break; }
        if (islbl) fprintf(o,"L_%04X:\n", a);
        fprintf(o,"    /* %04X  %s */\n", a, in->text);
        fprintf(o,"    SMS_PC(0x%04X);\n", a);
        int base=cyc_base(in);
        if (base>0) fprintf(o,"    sms_tick(%d);\n", base);
        if (in->cf==Z80_CF_NONE){
            fprintf(o,"    {\n");
            emit_op(o,in);
            fprintf(o,"    }\n");
        } else {
            emit_cf(o,in,a);
        }
    }
    fprintf(o,"}\n\n");
    G_BS = NULL;

    free(lbl);
    trace_free(&tr);
    G_TR = NULL;
}

/* ---- file writers ---- */
static void write_funcs_header(const char *path, const char *pfx, const FuncList *fl){
    FILE *h=fopen(path,"w");
    if (!h){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(h,
        "/* %s_funcs.h - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Prototypes for every recompiled function. call_by_address() (defined\n"
        " * in %s_dispatch.c) and the timing/IO surface come from sms_runtime.h. */\n"
        "#pragma once\n"
        "#include \"sms_runtime.h\"\n"
        "#include \"z80_ops.h\"\n\n", pfx, pfx);
    for (int i=0;i<fl->count;i++)
        if (fl->items[i].is_entry)
            fprintf(h,"void %s(void);\n", fl->items[i].name);
    fclose(h);
}

static void write_dispatch(const char *path, const char *pfx, const FuncList *fl){
    FILE *d=fopen(path,"w");
    if (!d){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(d,
        "/* %s_dispatch.c - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Resolves a Z80 address to its recompiled function, bank-aware: an\n"
        " * address with multiple bank variants switches on the runtime bank in\n"
        " * its slot (sms_slot_bank). On an unresolved target the runner's\n"
        " * sms_dispatch_miss() logs it (PRINCIPLES #13a). */\n"
        "#include \"%s_funcs.h\"\n\n"
        "void call_by_address(uint16_t addr){\n    switch (addr){\n", pfx, pfx);
    uint8_t *seen=(uint8_t*)calloc(0x10000,1);
    for (int i=0;i<fl->count;i++){
        if (!fl->items[i].is_entry) continue;
        uint16_t a=fl->items[i].addr;
        if (seen[a]) continue;
        seen[a]=1;
        /* gather all bank variants at this address */
        int vidx[64]; int nv=0;
        for (int j=0;j<fl->count && nv<64;j++)
            if (fl->items[j].is_entry && fl->items[j].addr==a) vidx[nv++]=j;
        if (nv == 1){
            fprintf(d,"    case 0x%04X: %s(); return;\n", a, fl->items[vidx[0]].name);
        } else {
            /* runtime bank for a variant: explicit bank, or the slot's default
             * (slot index) for a fixed (-1) entry. */
            fprintf(d,"    case 0x%04X: switch (sms_slot_bank(0x%04X)){\n", a, a);
            int emitted[64]; int ne=0;
            for (int v=0; v<nv; v++){
                int b = fl->items[vidx[v]].bank;
                int rb = (b >= 0) ? b : (a >> 14);
                bool dup=false; for (int e=0;e<ne;e++) if (emitted[e]==rb){ dup=true; break; }
                if (dup) continue;
                emitted[ne++]=rb;
                fprintf(d,"        case %d: %s(); return;\n", rb, fl->items[vidx[v]].name);
            }
            fprintf(d,"        default: break;\n    } break;\n");
        }
    }
    free(seen);
    fprintf(d,"    default: break;\n    }\n    sms_dispatch_miss(addr);\n}\n");
    fclose(d);
}

static void write_layout(const char *path, const char *pfx, const GameConfig *cfg){
    FILE *l=fopen(path,"w");
    if (!l){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(l,
        "/* %s_layout.c - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Per-game RAM layout (g_game_layout) from [ram_layout] in game.toml.\n"
        " * Compiled into the runner once it defines the GameLayout struct\n"
        " * (runner/game_layout.h) and SMSGG_HAVE_GAME_LAYOUT. */\n"
        "#if defined(SMSGG_HAVE_GAME_LAYOUT)\n"
        "#include \"game_layout.h\"\n"
        "const GameLayout g_game_layout = {\n", pfx);
    if (cfg->game_mode_addr     != GC_ADDR_UNSET) fprintf(l,"    .game_mode     = 0x%04X,\n", cfg->game_mode_addr);
    if (cfg->vblank_count_addr  != GC_ADDR_UNSET) fprintf(l,"    .vblank_count  = 0x%04X,\n", cfg->vblank_count_addr);
    if (cfg->player_object_addr != GC_ADDR_UNSET) fprintf(l,"    .player_object = 0x%04X,\n", cfg->player_object_addr);
    fprintf(l,"};\n#endif\n");
    fclose(l);
}

void cg_emit(const SmsRom *rom, const FuncList *fl, const GameConfig *cfg, const char *out_dir){
    G_FL = fl;
    CG_MKDIR(out_dir);   /* best-effort; generated/ usually already exists */

    const char *pfx = cfg->output_prefix;
    char path[600];

    int n = fl->count;
    uint16_t *ents=(uint16_t*)malloc((size_t)(n>0?n:1)*sizeof(uint16_t));
    for (int i=0;i<n;i++) ents[i]=fl->items[i].addr;

    snprintf(path,sizeof path,"%s/%s_funcs.h", out_dir, pfx);
    write_funcs_header(path, pfx, fl);

    snprintf(path,sizeof path,"%s/%s_full.c", out_dir, pfx);
    FILE *o=fopen(path,"w");
    if (!o){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(o,
        "/* %s_full.c - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Recompiled Z80 function bodies. */\n"
        "#include \"%s_funcs.h\"\n\n", pfx, pfx);
    int emitted=0;
    for (int i=0;i<n;i++){
        if (!fl->items[i].is_entry) continue;
        emit_function(o, rom, &fl->items[i], ents, n);
        emitted++;
    }
    fclose(o);

    snprintf(path,sizeof path,"%s/%s_dispatch.c", out_dir, pfx);
    write_dispatch(path, pfx, fl);

    snprintf(path,sizeof path,"%s/%s_layout.c", out_dir, pfx);
    write_layout(path, pfx, cfg);

    free(ents);
    printf("[cg_emit] wrote %d functions to %s/%s_{full,dispatch,layout}.c (+%s_funcs.h)\n",
           emitted, out_dir, pfx, pfx);
}

