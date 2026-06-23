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

    if (op >= 0x40 && op <= 0x7F && op != 0x76){       /* LD r,r' */
        sljit_sw od = reg8_off((op >> 3) & 7), os = reg8_off(op & 7);
        if (od < 0 || os < 0) return 0;                /* (HL) operand */
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_R0, 0, SLJIT_MEM1(SLJIT_S0), os);
        sljit_emit_op1(c, SLJIT_MOV_U8, SLJIT_MEM1(SLJIT_S0), od, SLJIT_R0, 0);
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

    return 0;                                          /* unsupported -> decline */
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
        if (n <= 0 || in.illegal){ ok = 0; break; }
        if (!emit_one(c, &in)){ ok = 0; break; }       /* declined */
        if (in.cf == Z80_CF_RET){ ended = 1; break; }  /* function terminator */
        if (in.cf != Z80_CF_NONE){ ok = 0; break; }    /* P1a: fall-through + final RET only */
        off += (size_t)n; pc = (uint16_t)(pc + n);
    }

    if (!ok || !ended){ sljit_free_compiler(c); return NULL; }

    void *code = sljit_generate_code(c, 0, NULL);
    sljit_free_compiler(c);
    return (ShardFn)code;
}

#endif /* SMS_HAVE_JIT */
