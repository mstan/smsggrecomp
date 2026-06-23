/*
 * z80_sljit.c — Z80 -> sljit shard emitter (P1a subset).
 *
 * Parallels the static C codegen and is validated byte-for-byte against the
 * superzazu interpreter (the differential harness, tests/z80_sljit_diff.c).
 * Anything with non-trivial flags is emitted as a CALL to a helper that runs the
 * SAME z80_ops.h core the generated C uses, so flag results are identical by
 * construction. Truly-trivial ops are inlined.
 *
 * P1a supported subset: NOP, LD r,r' (no (HL)), LD r,n, INC r, DEC r, RET.
 * Everything else -> decline (return NULL).
 */
#ifdef SMS_HAVE_JIT

#include "z80_sljit.h"
#include "z80_decoder.h"
#include "sljitLir.h"
#include <string.h>

/* flag-exact helpers (z80_sljit_helpers.c), backed by z80_ops.h */
extern long z80h_inc8(Z80State *s, long v);
extern long z80h_dec8(Z80State *s, long v);
extern void z80h_add(Z80State *s, long v), z80h_adc(Z80State *s, long v);
extern void z80h_sub(Z80State *s, long v), z80h_sbc(Z80State *s, long v);
extern void z80h_and(Z80State *s, long v), z80h_xor(Z80State *s, long v);
extern void z80h_or (Z80State *s, long v), z80h_cp (Z80State *s, long v);
extern void z80h_rlca(Z80State *s), z80h_rrca(Z80State *s);
extern void z80h_rla (Z80State *s), z80h_rra (Z80State *s);
extern void z80h_cpl(Z80State*), z80h_daa(Z80State*), z80h_scf(Z80State*), z80h_ccf(Z80State*);
extern void z80h_ex_af(Z80State*), z80h_ex_dehl(Z80State*), z80h_exx(Z80State*);

/* call void h(Z80State*) */
static void emit_call_s(struct sljit_compiler *c, void *fn){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS1V(P), SLJIT_IMM, (sljit_sw)fn);
}

/* indexed by the Z80 ALU group (y field): ADD ADC SUB SBC AND XOR OR CP */
static void *const ALU_HELP[8] = {
    (void*)z80h_add,(void*)z80h_adc,(void*)z80h_sub,(void*)z80h_sbc,
    (void*)z80h_and,(void*)z80h_xor,(void*)z80h_or ,(void*)z80h_cp
};

/* call void helper(Z80State*, long): caller must pre-load the operand into R1. */
static void emit_alu_call(struct sljit_compiler *c, void *fn){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2V(P, W), SLJIT_IMM, (sljit_sw)fn);
}

/* memory/16-bit helper call shapes (S0=Z80State*, S1=Bus*) */
extern void z80h_ld_r_hl(Z80State*,const Bus*,long), z80h_ld_hl_r(Z80State*,const Bus*,long),
            z80h_ld_hl_n(Z80State*,const Bus*,long), z80h_ld_a_bc(Z80State*,const Bus*),
            z80h_ld_a_de(Z80State*,const Bus*), z80h_ld_a_nn(Z80State*,const Bus*,long),
            z80h_ld_bc_a(Z80State*,const Bus*), z80h_ld_de_a(Z80State*,const Bus*),
            z80h_ld_nn_a(Z80State*,const Bus*,long), z80h_ld_rr(Z80State*,long,long),
            z80h_inc_rr(Z80State*,long), z80h_dec_rr(Z80State*,long), z80h_add_hl(Z80State*,long),
            z80h_push(Z80State*,const Bus*,long), z80h_pop(Z80State*,const Bus*,long),
            z80h_out_n(Z80State*,const Bus*,long);
extern long z80h_call(Z80State*,const Bus*,long);

/* void h(Z80State*, const Bus*, long w) */
static void emit_call_sbw(struct sljit_compiler *c, void *fn, sljit_sw w){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S1, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, w);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS3V(P, P, W), SLJIT_IMM, (sljit_sw)fn);
}
/* void h(Z80State*, const Bus*) */
static void emit_call_sb(struct sljit_compiler *c, void *fn){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S1, 0);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2V(P, P), SLJIT_IMM, (sljit_sw)fn);
}
/* void h(Z80State*, long w) */
static void emit_call_sw(struct sljit_compiler *c, void *fn, sljit_sw w){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, w);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2V(P, W), SLJIT_IMM, (sljit_sw)fn);
}
/* void h(Z80State*, long w1, long w2) */
static void emit_call_sww(struct sljit_compiler *c, void *fn, sljit_sw w1, sljit_sw w2){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, w1);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, w2);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS3V(P, W, W), SLJIT_IMM, (sljit_sw)fn);
}

#define OFF(field) ((sljit_sw)offsetof(Z80State, field))

/* Z80 r-table index (0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A) -> Z80State byte offset,
 * or -1 for (HL) (memory; not in the P1a subset). */
static sljit_sw reg8_off(int idx){
    switch (idx){
        case 0: return OFF(b);  case 1: return OFF(c);
        case 2: return OFF(d);  case 3: return OFF(e);
        case 4: return OFF(h);  case 5: return OFF(l);
        case 7: return OFF(a);
        default: return -1;
    }
}

/* cyc += n  (load word, add, store) */
static void emit_tick(struct sljit_compiler *c, int n){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), OFF(cyc));
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, n);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_MEM1(SLJIT_S0), OFF(cyc), SLJIT_R0, 0);
}

#define OFFB(field) ((sljit_sw)offsetof(Bus, field))

/* sync-first, emitted BEFORE every instruction body (mirrors sms_tick):
 *   if (s->cyc >= *bus->sync_deadline) bus->sync(s);   s->ei_block = 0;
 * Live, bus->sync advances the VDP and accepts a pending IRQ at this boundary;
 * off-thread validation freezes it (deadline = MAX, sync = no-op). */
static void emit_sync(struct sljit_compiler *c){
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), OFF(cyc));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S1), OFFB(sync_deadline));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_R1), 0);   /* *sync_deadline */
    struct sljit_jump *Jskip = sljit_emit_cmp(c, SLJIT_LESS, SLJIT_R0, 0, SLJIT_R1, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_MEM1(SLJIT_S1), OFFB(sync));
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS1V(P), SLJIT_R2, 0);
    sljit_set_label(Jskip, sljit_emit_label(c));
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), OFF(ei_block), SLJIT_IMM, 0);
}

/* CALL/RST: tick, push-ret + dispatch via z80h_call(s,bus,(ret<<16)|target); if the
 * callee unwound past this frame (return != 0) the shard returns, else falls through. */
static void emit_call(struct sljit_compiler *c, uint16_t target, uint16_t ret, int cyc){
    emit_tick(c, cyc);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_S1, 0);
    sljit_emit_op1(c, SLJIT_MOV, SLJIT_R2, 0, SLJIT_IMM, ((sljit_sw)ret << 16) | target);
    sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS3(W, P, P, W), SLJIT_IMM, (sljit_sw)z80h_call);
    sljit_emit_op2(c, SLJIT_AND | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_R0, 0);
    struct sljit_jump *Jc = sljit_emit_jump(c, SLJIT_ZERO);   /* no propagation -> continue */
    sljit_emit_return_void(c);                                /* propagation -> return */
    sljit_set_label(Jc, sljit_emit_label(c));
}

/* Emit one instruction. Returns 1 if emitted, 0 to decline the whole function. */
static int emit_one(struct sljit_compiler *c, const Z80Insn *in){
    if (in->prefix != Z80_PFX_NONE) return 0;          /* P1a: no prefix groups */
    uint8_t op = in->opcode;

    if (op == 0x00){ emit_tick(c, 4); return 1; }      /* NOP */

    if (op == 0xC9){                                   /* RET (unconditional) */
        sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), OFF(sp));
        sljit_emit_op2(c, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 2);
        sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0), OFF(sp), SLJIT_R0, 0);
        emit_tick(c, 10);
        sljit_emit_return_void(c);
        return 1;
    }

    if (op >= 0x40 && op <= 0x7F && op != 0x76){       /* LD r,r' / r,(HL) / (HL),r */
        int dst = (op >> 3) & 7, src = op & 7;
        if (dst == 6){ emit_call_sbw(c, (void*)z80h_ld_hl_r, src); emit_tick(c, 7); return 1; }
        if (src == 6){ emit_call_sbw(c, (void*)z80h_ld_r_hl, dst); emit_tick(c, 7); return 1; }
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), reg8_off(src));
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), reg8_off(dst), SLJIT_R0, 0);
        emit_tick(c, 4);
        return 1;
    }

    if ((op & 0xC7) == 0x06 && op != 0x36){            /* LD r,n */
        sljit_sw o = reg8_off((op >> 3) & 7);
        if (o < 0) return 0;
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), o, SLJIT_IMM, (sljit_sw)(in->imm & 0xFF));
        emit_tick(c, 7);
        return 1;
    }

    if ((op & 0xC7) == 0x04 || (op & 0xC7) == 0x05){   /* INC r / DEC r */
        sljit_sw o = reg8_off((op >> 3) & 7);
        if (o < 0) return 0;                           /* INC/DEC (HL) */
        int isdec = ((op & 0xC7) == 0x05);
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);                 /* arg0 = Z80State* */
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), o);  /* arg1 = reg value */
        sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS2(W, P, W),
                         SLJIT_IMM, (sljit_sw)(isdec ? (void*)z80h_dec8 : (void*)z80h_inc8));
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), o, SLJIT_R0, 0);  /* store result */
        emit_tick(c, 4);
        return 1;
    }

    if (op >= 0x80 && op <= 0xBF){                     /* ALU A,r (ADD..CP) */
        sljit_sw o = reg8_off(op & 7);
        if (o < 0) return 0;                           /* ALU A,(HL): memory */
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R1, 0, SLJIT_MEM1(SLJIT_S0), o);
        emit_alu_call(c, ALU_HELP[(op >> 3) & 7]);
        emit_tick(c, 4);
        return 1;
    }

    if ((op & 0xC7) == 0xC6){                          /* ALU A,n */
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R1, 0, SLJIT_IMM, (sljit_sw)(in->imm & 0xFF));
        emit_alu_call(c, ALU_HELP[(op >> 3) & 7]);
        emit_tick(c, 7);
        return 1;
    }

    if (op == 0x07 || op == 0x0F || op == 0x17 || op == 0x1F){   /* RLCA/RRCA/RLA/RRA */
        void *fn = (op==0x07) ? (void*)z80h_rlca : (op==0x0F) ? (void*)z80h_rrca
                 : (op==0x17) ? (void*)z80h_rla  : (void*)z80h_rra;
        sljit_emit_op1(c, SLJIT_MOV, SLJIT_R0, 0, SLJIT_S0, 0);
        sljit_emit_icall(c, SLJIT_CALL, SLJIT_ARGS1V(P), SLJIT_IMM, (sljit_sw)fn);
        emit_tick(c, 4);
        return 1;
    }

    /* ---- P1c: memory + 16-bit + stack (all via the Bus) ---- */
    if (op == 0x36){ emit_call_sbw(c,(void*)z80h_ld_hl_n, in->imm & 0xFF); emit_tick(c,10); return 1; }
    if (op == 0x0A){ emit_call_sb (c,(void*)z80h_ld_a_bc); emit_tick(c,7);  return 1; }
    if (op == 0x1A){ emit_call_sb (c,(void*)z80h_ld_a_de); emit_tick(c,7);  return 1; }
    if (op == 0x02){ emit_call_sb (c,(void*)z80h_ld_bc_a); emit_tick(c,7);  return 1; }
    if (op == 0x12){ emit_call_sb (c,(void*)z80h_ld_de_a); emit_tick(c,7);  return 1; }
    if (op == 0x3A){ emit_call_sbw(c,(void*)z80h_ld_a_nn, in->imm); emit_tick(c,13); return 1; }
    if (op == 0x32){ emit_call_sbw(c,(void*)z80h_ld_nn_a, in->imm); emit_tick(c,13); return 1; }

    if ((op & 0xCF) == 0x01){ emit_call_sww(c,(void*)z80h_ld_rr,(op>>4)&3, in->imm); emit_tick(c,10); return 1; } /* LD rr,nn */
    if ((op & 0xCF) == 0x03){ emit_call_sw (c,(void*)z80h_inc_rr,(op>>4)&3); emit_tick(c,6);  return 1; }        /* INC rr   */
    if ((op & 0xCF) == 0x0B){ emit_call_sw (c,(void*)z80h_dec_rr,(op>>4)&3); emit_tick(c,6);  return 1; }        /* DEC rr   */
    if ((op & 0xCF) == 0x09){ emit_call_sw (c,(void*)z80h_add_hl,(op>>4)&3); emit_tick(c,11); return 1; }        /* ADD HL,rr*/
    if ((op & 0xCF) == 0xC5){ emit_call_sbw(c,(void*)z80h_push,(op>>4)&3); emit_tick(c,11); return 1; }          /* PUSH rr  */
    if ((op & 0xCF) == 0xC1){ emit_call_sbw(c,(void*)z80h_pop ,(op>>4)&3); emit_tick(c,10); return 1; }          /* POP rr   */

    /* OUT (0xD3) emittable + gate-correct, but re-declined: even with the P1f sync model,
     * 0x8000 (the OUT-bearing frame routine) still diverges live — a DEEPER cause than sync
     * (identical divergence with/without sync; suspect bank-aliased code or 1-pass-validation
     * insufficiency). Keep OUT/CALL gated until that is root-caused. See SLJIT.md §8.5. */

    if (op == 0x2F){ emit_call_s(c,(void*)z80h_cpl);     emit_tick(c,4); return 1; }  /* CPL */
    if (op == 0x27){ emit_call_s(c,(void*)z80h_daa);     emit_tick(c,4); return 1; }  /* DAA */
    if (op == 0x37){ emit_call_s(c,(void*)z80h_scf);     emit_tick(c,4); return 1; }  /* SCF */
    if (op == 0x3F){ emit_call_s(c,(void*)z80h_ccf);     emit_tick(c,4); return 1; }  /* CCF */
    if (op == 0x08){ emit_call_s(c,(void*)z80h_ex_af);   emit_tick(c,4); return 1; }  /* EX AF,AF' */
    if (op == 0xEB){ emit_call_s(c,(void*)z80h_ex_dehl); emit_tick(c,4); return 1; }  /* EX DE,HL */
    if (op == 0xD9){ emit_call_s(c,(void*)z80h_exx);     emit_tick(c,4); return 1; }  /* EXX */
    if (op == 0xF3){ sljit_emit_op1(c,SLJIT_MOV_U8,SLJIT_MEM1(SLJIT_S0),OFF(iff1),SLJIT_IMM,0);  /* DI */
                     sljit_emit_op1(c,SLJIT_MOV_U8,SLJIT_MEM1(SLJIT_S0),OFF(iff2),SLJIT_IMM,0);
                     emit_tick(c,4); return 1; }
    if (op == 0xFB){ sljit_emit_op1(c,SLJIT_MOV_U8,SLJIT_MEM1(SLJIT_S0),OFF(iff1),SLJIT_IMM,1);  /* EI */
                     sljit_emit_op1(c,SLJIT_MOV_U8,SLJIT_MEM1(SLJIT_S0),OFF(iff2),SLJIT_IMM,1);
                     sljit_emit_op1(c,SLJIT_MOV_U8,SLJIT_MEM1(SLJIT_S0),OFF(ei_block),SLJIT_IMM,1);
                     emit_tick(c,4); return 1; }

    return 0;                                          /* unsupported -> decline */
}

ZjitDecline z80_sljit_last_decline;

static const char *cf_reason(int cf){
    switch (cf){
        case Z80_CF_JUMP:      return "control flow: JP/JR/JP(HL)";
        case Z80_CF_JUMP_COND: return "control flow: cond JP/JR/DJNZ";
        case Z80_CF_CALL:      return "control flow: CALL/RST";
        case Z80_CF_CALL_COND: return "control flow: cond CALL";
        case Z80_CF_RET_COND:  return "control flow: cond RET";
        default:               return "control flow";
    }
}
static const char *why_unsupported(const Z80Insn *in){
    if (in->prefix == Z80_PFX_CB) return "CB prefix (bit/rot/shift)";
    if (in->prefix == Z80_PFX_ED) return "ED prefix (block ops / 16-bit ld(nn) / etc.)";
    if (in->prefix == Z80_PFX_DD || in->prefix == Z80_PFX_FD) return "DD/FD prefix (IX/IY)";
    uint8_t op = in->opcode;
    if (op == 0xD3 || op == 0xDB) return "I/O: OUT (n),A / IN A,(n)";
    if (op == 0xF3 || op == 0xFB) return "DI/EI (interrupt enable)";
    if (op == 0x76)               return "HALT";
    if (op == 0x27)               return "DAA";
    if (op == 0x2F)               return "CPL";
    if (op == 0x37 || op == 0x3F) return "SCF/CCF";
    if (op == 0x08)               return "EX AF,AF'";
    if (op == 0xD9)               return "EXX";
    if (op == 0xEB)               return "EX DE,HL";
    if (op == 0xE3)               return "EX (SP),HL";
    if (op == 0xF9)               return "LD SP,HL";
    return "other unsupported opcode";
}
static void set_decl(uint16_t pc, const Z80Insn *in, const char *why){
    z80_sljit_last_decline.pc = pc;
    z80_sljit_last_decline.prefix = (uint8_t)in->prefix;
    z80_sljit_last_decline.opcode = in->opcode;
    z80_sljit_last_decline.why = why;
    for (int i = 0; i < (int)sizeof z80_sljit_last_decline.text; i++)
        z80_sljit_last_decline.text[i] = in->text[i];
}

/* ---- P1d: control flow ---- */
#define SPAN 2048    /* max function span (bytes) the shard CFG covers */

/* Emit the Z80 condition test for cc (0..7): leaves sljit Z reflecting the masked
 * flag and returns the sljit jump type that is TAKEN iff the Z80 condition holds. */
static sljit_s32 emit_cc(struct sljit_compiler *c, int cc){
    static const uint8_t MASK[4] = { 0x40, 0x01, 0x04, 0x80 };   /* Z, C, P/V, S */
    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), OFF(f));
    sljit_emit_op2(c, SLJIT_AND | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, MASK[cc>>1]);
    return (cc & 1) ? SLJIT_NOT_ZERO : SLJIT_ZERO;   /* odd cc = "flag set" */
}
static void emit_ret(struct sljit_compiler *c, int cyc){
    sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), OFF(sp));
    sljit_emit_op2(c, SLJIT_ADD, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 2);
    sljit_emit_op1(c, SLJIT_MOV_U16, SLJIT_MEM1(SLJIT_S0), OFF(sp), SLJIT_R0, 0);
    emit_tick(c, cyc);
    sljit_emit_return_void(c);
}

typedef struct { struct sljit_jump *j; uint16_t target; } PendJump;

/* worker/harness are single-threaded; compile is not re-entrant -> file-static scratch */
static uint8_t              g_seen[SPAN];
static struct sljit_label  *g_lab[SPAN];
static uint16_t             g_work[SPAN];
static PendJump             g_pend[2*SPAN];

ShardFn z80_sljit_compile(const uint8_t *bytes, size_t len, uint16_t base){
    /* ---- pass 1: reachability (follow control flow; bound to [base, base+SPAN)) ---- */
    memset(g_seen, 0, sizeof g_seen);
    int wn = 0, maxrel = 0;
    g_work[wn++] = base;
    while (wn){
        uint16_t a = g_work[--wn];
        int rel = (int)a - (int)base;
        if (rel < 0 || rel >= SPAN){
            Z80Insn d = {0}; set_decl(a, &d, "branch target outside function window"); return NULL;
        }
        if (g_seen[rel]) continue;
        if ((size_t)rel >= len) return NULL;
        Z80Insn in;
        int n = z80_decode(bytes + rel, len - rel, a, &in);
        if (n <= 0 || in.illegal){ set_decl(a, &in, "illegal/undecodable"); return NULL; }
        g_seen[rel] = 1;
        if (rel + n > maxrel) maxrel = rel + n;

        switch (in.cf){
            case Z80_CF_NONE: case Z80_CF_RET_COND:
                g_work[wn++] = (uint16_t)(a + n);                       /* fall through */
                if (in.cf == Z80_CF_RET_COND) {}                       /* also returns; no succ for that edge */
                break;
            case Z80_CF_JUMP:                                          /* JP nn / JR e / JP(HL) */
                if (!in.has_target){ set_decl(a, &in, "computed jump JP(HL/IX/IY)"); return NULL; }
                g_work[wn++] = in.target;
                break;
            case Z80_CF_JUMP_COND:                                     /* JR cc / JP cc / DJNZ */
                if (!in.has_target){ set_decl(a, &in, cf_reason(in.cf)); return NULL; }
                g_work[wn++] = in.target;
                g_work[wn++] = (uint16_t)(a + n);
                break;
            case Z80_CF_CALL: case Z80_CF_CALL_COND:
                /* re-declined with OUT: frame-spanning routines (0x8000) still diverge live
                 * for a cause deeper than sync — see the OUT note above and SLJIT.md §8.5. */
                set_decl(a, &in, "CALL (frame routine diverges - deeper than sync; see SLJIT.md)"); return NULL;
            case Z80_CF_RET: default: break;                           /* terminator */
        }
    }

    /* ---- pass 2: emit in ascending address order, a label before every insn ---- */
    struct sljit_compiler *c = sljit_create_compiler(NULL);
    if (!c) return NULL;
    sljit_emit_enter(c, 0, SLJIT_ARGS2V(P, P), 3, 2, 0);   /* void shard(Z80State* S0, Bus* S1) */
    memset(g_lab, 0, sizeof g_lab);
    int np = 0;

    for (int rel = 0; rel < maxrel; rel++){
        if (!g_seen[rel]) continue;
        uint16_t a = (uint16_t)(base + rel);
        Z80Insn in;
        z80_decode(bytes + rel, len - rel, a, &in);
        g_lab[rel] = sljit_emit_label(c);                  /* label every reachable insn */
        uint16_t ft = (uint16_t)(a + in.length);
        uint8_t op = in.opcode;

        emit_sync(c);                                      /* sync-first, before the body (SLJIT.md §8.5) */

        if (in.cf == Z80_CF_NONE){
            if (!emit_one(c, &in)){ set_decl(a, &in, why_unsupported(&in)); sljit_free_compiler(c); return NULL; }
            continue;
        }
        switch (in.cf){
            case Z80_CF_RET:
                emit_ret(c, 10);
                break;
            case Z80_CF_RET_COND: {
                sljit_s32 jt = emit_cc(c, (op>>3)&7);
                struct sljit_jump *Jt = sljit_emit_jump(c, jt);
                emit_tick(c, 5);                            /* not-taken */
                struct sljit_jump *Js = sljit_emit_jump(c, SLJIT_JUMP);
                g_pend[np].j = Js; g_pend[np++].target = ft;
                sljit_set_label(Jt, sljit_emit_label(c));
                emit_ret(c, 11);                            /* taken: sp+=2, return */
                break;
            }
            case Z80_CF_JUMP: {
                emit_tick(c, op == 0x18 ? 12 : 10);         /* JR e : JP nn */
                struct sljit_jump *Ju = sljit_emit_jump(c, SLJIT_JUMP);
                g_pend[np].j = Ju; g_pend[np++].target = in.target;
                break;
            }
            case Z80_CF_JUMP_COND: {
                if (op == 0x10){                            /* DJNZ: B--, jump if B!=0 (13/8) */
                    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), OFF(b));
                    sljit_emit_op2(c, SLJIT_SUB | SLJIT_SET_Z, SLJIT_R0, 0, SLJIT_R0, 0, SLJIT_IMM, 1);
                    sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), OFF(b), SLJIT_R0, 0);
                    struct sljit_jump *Jt = sljit_emit_jump(c, SLJIT_NOT_ZERO);
                    emit_tick(c, 8);
                    struct sljit_jump *Js = sljit_emit_jump(c, SLJIT_JUMP);
                    sljit_set_label(Jt, sljit_emit_label(c)); emit_tick(c, 13);
                    struct sljit_jump *Jto = sljit_emit_jump(c, SLJIT_JUMP);
                    g_pend[np].j = Jto; g_pend[np++].target = in.target;
                    g_pend[np].j = Js;  g_pend[np++].target = ft;
                } else if ((op & 0xE7) == 0x20){            /* JR cc (12/7) */
                    sljit_s32 jt = emit_cc(c, (op>>3)&3);
                    struct sljit_jump *Jt = sljit_emit_jump(c, jt);
                    emit_tick(c, 7);
                    struct sljit_jump *Js = sljit_emit_jump(c, SLJIT_JUMP);
                    sljit_set_label(Jt, sljit_emit_label(c)); emit_tick(c, 12);
                    struct sljit_jump *Jto = sljit_emit_jump(c, SLJIT_JUMP);
                    g_pend[np].j = Jto; g_pend[np++].target = in.target;
                    g_pend[np].j = Js;  g_pend[np++].target = ft;
                } else {                                    /* JP cc (10, taken or not) */
                    emit_tick(c, 10);
                    sljit_s32 jt = emit_cc(c, (op>>3)&7);
                    struct sljit_jump *Jt = sljit_emit_jump(c, jt);
                    g_pend[np].j = Jt; g_pend[np++].target = in.target;  /* fall through naturally */
                }
                break;
            }
            case Z80_CF_CALL:                                   /* CALL nn / RST n */
                emit_call(c, in.target, (uint16_t)(a + in.length), op == 0xCD ? 17 : 11);
                break;
            case Z80_CF_CALL_COND: {                            /* CALL cc,nn (17 taken / 10 not-taken) */
                sljit_s32 jt = emit_cc(c, (op>>3)&7);
                struct sljit_jump *Jt = sljit_emit_jump(c, jt);
                emit_tick(c, 10);
                struct sljit_jump *Js = sljit_emit_jump(c, SLJIT_JUMP);
                g_pend[np].j = Js; g_pend[np++].target = ft;
                sljit_set_label(Jt, sljit_emit_label(c));
                emit_call(c, in.target, (uint16_t)(a + in.length), 17);
                break;
            }
            default:
                set_decl(a, &in, cf_reason(in.cf)); sljit_free_compiler(c); return NULL;
        }
    }

    /* wire every recorded jump to its target's label */
    for (int i = 0; i < np; i++){
        int trel = (int)g_pend[i].target - (int)base;
        if (trel < 0 || trel >= SPAN || !g_lab[trel]){ sljit_free_compiler(c); return NULL; }
        sljit_set_label(g_pend[i].j, g_lab[trel]);
    }

    void *code = sljit_generate_code(c, 0, NULL);
    sljit_free_compiler(c);
    return (ShardFn)code;
}

#endif /* SMS_HAVE_JIT */
