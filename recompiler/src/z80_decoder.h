/*
 * z80_decoder.h — Zilog Z80 instruction decoder for the SMS/GG recompiler.
 *
 * The decoder's hard contract (correctness-critical, see PRINCIPLES #16):
 *   - INSTRUCTION LENGTH for every opcode incl. CB/ED/DD/FD/DDCB/FDCB prefixes.
 *   - CONTROL-FLOW class + statically-known target (for the function finder).
 *   - Operand extraction (displacement, 8/16-bit immediate).
 *
 * Semantics are anchored to the vendored MIT superzazu/z80.c. The decoder
 * yields a structured Z80Insn; the code generator switches on
 * (prefix, opcode, disp, imm) to emit C. A best-effort disassembly string is
 * provided for comments/debugging.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    Z80_PFX_NONE = 0,
    Z80_PFX_CB,     /* bit/rotate/shift                       (2 bytes) */
    Z80_PFX_ED,     /* extended                               (2 bytes) */
    Z80_PFX_DD,     /* IX                                     (var)     */
    Z80_PFX_FD,     /* IY                                     (var)     */
    Z80_PFX_DDCB,   /* DD CB d op : bit/rot/shift on (IX+d)   (4 bytes) */
    Z80_PFX_FDCB,   /* FD CB d op : bit/rot/shift on (IY+d)   (4 bytes) */
} Z80Prefix;

typedef enum {
    Z80_CF_NONE = 0,    /* falls through to next instruction */
    Z80_CF_JUMP,        /* unconditional terminator: JP nn, JR e, JP (HL/IX/IY) */
    Z80_CF_JUMP_COND,   /* conditional: JP cc,nn / JR cc,e / DJNZ (also falls through) */
    Z80_CF_CALL,        /* CALL nn / RST n (falls through on return) */
    Z80_CF_CALL_COND,   /* CALL cc,nn */
    Z80_CF_RET,         /* RET / RETI / RETN — terminator */
    Z80_CF_RET_COND,    /* RET cc — also falls through */
} Z80CtrlFlow;

typedef struct {
    uint8_t     length;        /* total bytes consumed (1..4) */
    Z80Prefix   prefix;
    uint8_t     opcode;        /* primary opcode AFTER any prefix */
    int8_t      disp;          /* (IX/IY+d) displacement; valid iff uses_disp */
    bool        uses_disp;
    uint16_t    imm;           /* 8- or 16-bit immediate (see imm_bits) */
    uint8_t     imm_bits;      /* 0, 8, or 16 */

    Z80CtrlFlow cf;
    bool        has_target;    /* cf target statically known */
    uint16_t    target;        /* absolute Z80 target address */
    bool        is_halt;       /* HALT (0x76) */
    bool        illegal;       /* not a valid encoding (should not happen on real ROMs) */

    uint8_t     raw[4];        /* raw bytes */
    char        text[40];      /* best-effort disassembly */
} Z80Insn;

/*
 * Decode one instruction.
 *   p      : pointer to instruction bytes
 *   avail  : bytes readable at p (>=1; decoder reads at most 4)
 *   z80_pc : the instruction's Z80 address (for JR/DJNZ/CALL/JP target calc)
 * Returns the number of bytes consumed (== out->length), or 0 on avail==0.
 */
int z80_decode(const uint8_t *p, size_t avail, uint16_t z80_pc, Z80Insn *out);

/* True if a control-flow class ends a basic block / can terminate a function. */
static inline bool z80_cf_is_terminator(Z80CtrlFlow cf) {
    return cf == Z80_CF_JUMP || cf == Z80_CF_RET;
}
/* True if execution can continue to the next instruction (fall-through). */
static inline bool z80_cf_falls_through(Z80CtrlFlow cf) {
    return cf == Z80_CF_NONE || cf == Z80_CF_JUMP_COND ||
           cf == Z80_CF_CALL || cf == Z80_CF_CALL_COND ||
           cf == Z80_CF_RET_COND;
}
