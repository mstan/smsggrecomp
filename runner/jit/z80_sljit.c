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

/* flag-exact helpers (z80_sljit_helpers.c), backed by z80_ops.h */
extern long z80h_inc8(Z80State *s, long v);
extern long z80h_dec8(Z80State *s, long v);
extern void z80h_add(Z80State *s, long v), z80h_adc(Z80State *s, long v);
extern void z80h_sub(Z80State *s, long v), z80h_sbc(Z80State *s, long v);
extern void z80h_and(Z80State *s, long v), z80h_xor(Z80State *s, long v);
extern void z80h_or (Z80State *s, long v), z80h_cp (Z80State *s, long v);
extern void z80h_rlca(Z80State *s), z80h_rrca(Z80State *s);
extern void z80h_rla (Z80State *s), z80h_rra (Z80State *s);

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
            z80h_push(Z80State*,const Bus*,long), z80h_pop(Z80State*,const Bus*,long);

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

ShardFn z80_sljit_compile(const uint8_t *bytes, size_t len, uint16_t base){
    struct sljit_compiler *c = sljit_create_compiler(NULL);
    if (!c) return NULL;

    /* void shard(Z80State *s, const Bus *bus): args land in S0, S1 */
    sljit_emit_enter(c, 0, SLJIT_ARGS2V(P, P), 3, 2, 0);

    size_t off = 0; uint16_t pc = base; int ok = 1, ended = 0;
    while (off < len){
        Z80Insn in;
        int n = z80_decode(bytes + off, len - off, pc, &in);
        if (n <= 0 || in.illegal){ set_decl(pc, &in, "illegal/undecodable"); ok = 0; break; }
        if (!emit_one(c, &in)){                         /* declined */
            set_decl(pc, &in, in.cf != Z80_CF_NONE ? cf_reason(in.cf) : why_unsupported(&in));
            ok = 0; break;
        }
        if (in.cf == Z80_CF_RET){ ended = 1; break; }   /* function terminator */
        if (in.cf != Z80_CF_NONE){ set_decl(pc, &in, cf_reason(in.cf)); ok = 0; break; }
        off += (size_t)n; pc = (uint16_t)(pc + n);
    }

    if (!ok || !ended){ sljit_free_compiler(c); return NULL; }

    void *code = sljit_generate_code(c, 0, NULL);
    sljit_free_compiler(c);
    return (ShardFn)code;
}

#endif /* SMS_HAVE_JIT */
