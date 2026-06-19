/*
 * z80_decoder.c — Z80 instruction decoder. See z80_decoder.h.
 *
 * Length table verified against the Zilog Z80 opcode map; prefix handling
 * (CB / ED / DD / FD / DDCB / FDCB, repeated-prefix "last wins", and the
 * (IX+d)/(IY+d) displacement insertion) verified against superzazu/z80.c.
 */
#include "z80_decoder.h"
#include <string.h>
#include <stdio.h>

/* Length of the UNPREFIXED instruction (opcode + its own operands), NOT
 * counting any DD/FD/CB/ED prefix. Prefix bytes (CB/DD/ED/FD) are 0 sentinels
 * and handled in code. */
static const uint8_t base_len[256] = {
/*       0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F */
/*00*/   1, 3, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 1, 2, 1,
/*10*/   2, 3, 1, 1, 1, 1, 2, 1, 2, 1, 1, 1, 1, 1, 2, 1,
/*20*/   2, 3, 3, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,
/*30*/   2, 3, 3, 1, 1, 1, 2, 1, 2, 1, 3, 1, 1, 1, 2, 1,
/*40*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*50*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*60*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*70*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*80*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*90*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*A0*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*B0*/   1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/*C0*/   1, 1, 3, 3, 3, 1, 2, 1, 1, 1, 3, 0, 3, 3, 2, 1,
/*D0*/   1, 1, 3, 2, 3, 1, 2, 1, 1, 1, 3, 2, 3, 0, 2, 1,
/*E0*/   1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 0, 2, 1,
/*F0*/   1, 1, 3, 1, 3, 1, 2, 1, 1, 1, 3, 1, 3, 0, 2, 1,
};

/* Under DD/FD, does this base opcode reference (HL) and therefore take a
 * displacement byte (becoming (IX+d)/(IY+d))? */
static bool op_uses_hl_mem(uint8_t op) {
    if (op == 0x34 || op == 0x35 || op == 0x36) return true;   /* INC/DEC/LD (HL),n */
    if (op == 0x76) return false;                               /* HALT, not LD (HL),(HL) */
    if ((op & 0xC7) == 0x46) return true;                       /* LD r,(HL) */
    if ((op & 0xF8) == 0x70) return true;                       /* LD (HL),r */
    if ((op & 0xC7) == 0x86) return true;                       /* ALU A,(HL) */
    return false;
}

/* ---- disassembly helpers (best-effort, for comments/debugging) ---- */
static const char *R8[8]  = {"b","c","d","e","h","l","(hl)","a"};
static const char *RP[4]  = {"bc","de","hl","sp"};
static const char *RP2[4] = {"bc","de","hl","af"};
static const char *CC[8]  = {"nz","z","nc","c","po","pe","p","m"};
static const char *ALU[8] = {"add a,","adc a,","sub ","sbc a,","and ","xor ","or ","cp "};
static const char *ROT[8] = {"rlc","rrc","rl","rr","sla","sra","sll","srl"};

static void fmt_base(Z80Insn *in, const uint8_t *p) {
    uint8_t op = in->opcode;
    char *t = in->text; size_t n = sizeof(in->text);
    uint16_t nn = (in->imm_bits == 16) ? in->imm : 0;
    uint8_t  d8 = (uint8_t)in->imm;
    int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) { snprintf(t, n, "halt"); return; }
        snprintf(t, n, "ld %s,%s", R8[y], R8[z]); return;
    }
    if (op >= 0x80 && op < 0xC0) { snprintf(t, n, "%s%s", ALU[y], R8[z]); return; }
    switch (op) {
        case 0x00: snprintf(t,n,"nop"); return;
        case 0x76: snprintf(t,n,"halt"); return;
        case 0xC3: snprintf(t,n,"jp $%04X", nn); return;
        case 0xCD: snprintf(t,n,"call $%04X", nn); return;
        case 0xC9: snprintf(t,n,"ret"); return;
        case 0x18: snprintf(t,n,"jr $%04X", in->target); return;
        case 0x10: snprintf(t,n,"djnz $%04X", in->target); return;
        case 0xE9: snprintf(t,n,"jp (hl)"); return;
        case 0xF3: snprintf(t,n,"di"); return;
        case 0xFB: snprintf(t,n,"ei"); return;
        case 0xD3: snprintf(t,n,"out ($%02X),a", d8); return;
        case 0xDB: snprintf(t,n,"in a,($%02X)", d8); return;
        case 0xD9: snprintf(t,n,"exx"); return;
        case 0x08: snprintf(t,n,"ex af,af'"); return;
        case 0xEB: snprintf(t,n,"ex de,hl"); return;
        case 0xE3: snprintf(t,n,"ex (sp),hl"); return;
        case 0xF9: snprintf(t,n,"ld sp,hl"); return;
    }
    if ((op & 0xC7) == 0xC2) { snprintf(t,n,"jp %s,$%04X", CC[y], nn); return; }
    if ((op & 0xC7) == 0xC4) { snprintf(t,n,"call %s,$%04X", CC[y], nn); return; }
    if ((op & 0xC7) == 0xC0) { snprintf(t,n,"ret %s", CC[y]); return; }
    if ((op & 0xC7) == 0xC7) { snprintf(t,n,"rst $%02X", op & 0x38); return; }
    if ((op & 0xE7) == 0x20) { snprintf(t,n,"jr %s,$%04X", CC[(op>>3)&3], in->target); return; }
    if ((op & 0xCF) == 0x01) { snprintf(t,n,"ld %s,$%04X", RP[(op>>4)&3], nn); return; }
    if ((op & 0xCF) == 0xC5) { snprintf(t,n,"push %s", RP2[(op>>4)&3]); return; }
    if ((op & 0xCF) == 0xC1) { snprintf(t,n,"pop %s", RP2[(op>>4)&3]); return; }
    if ((op & 0xC7) == 0x06) { snprintf(t,n,"ld %s,$%02X", R8[y], d8); return; }
    if ((op & 0xC7) == 0xC6) { snprintf(t,n,"%s$%02X", ALU[y], d8); return; }
    if ((op & 0xCF) == 0x03) { snprintf(t,n,"inc %s", RP[(op>>4)&3]); return; }
    if ((op & 0xCF) == 0x0B) { snprintf(t,n,"dec %s", RP[(op>>4)&3]); return; }
    if ((op & 0xC7) == 0x04) { snprintf(t,n,"inc %s", R8[y]); return; }
    if ((op & 0xC7) == 0x05) { snprintf(t,n,"dec %s", R8[y]); return; }
    (void)x; (void)z; (void)p;
    snprintf(t, n, "op $%02X", op);
}

static void classify_base(Z80Insn *in, uint16_t z80_pc) {
    uint8_t op = in->opcode;
    /* relative jumps: target = address_of_next_insn + signed offset.
     * The offset byte is the instruction's 8-bit immediate. */
    int8_t e = (int8_t)(uint8_t)in->imm;
    if (op == 0x18) { in->cf = Z80_CF_JUMP;      in->has_target = true;
                      in->target = (uint16_t)(z80_pc + in->length + e); return; }
    if (op == 0x10) { in->cf = Z80_CF_JUMP_COND; in->has_target = true;
                      in->target = (uint16_t)(z80_pc + in->length + e); return; }
    if ((op & 0xE7) == 0x20) { in->cf = Z80_CF_JUMP_COND; in->has_target = true;
                      in->target = (uint16_t)(z80_pc + in->length + e); return; }
    /* absolute */
    if (op == 0xC3) { in->cf = Z80_CF_JUMP;      in->has_target = true; in->target = in->imm; return; }
    if ((op & 0xC7) == 0xC2) { in->cf = Z80_CF_JUMP_COND; in->has_target = true; in->target = in->imm; return; }
    if (op == 0xCD) { in->cf = Z80_CF_CALL;      in->has_target = true; in->target = in->imm; return; }
    if ((op & 0xC7) == 0xC4) { in->cf = Z80_CF_CALL_COND; in->has_target = true; in->target = in->imm; return; }
    if ((op & 0xC7) == 0xC7) { in->cf = Z80_CF_CALL;      in->has_target = true; in->target = op & 0x38; return; }
    if (op == 0xC9) { in->cf = Z80_CF_RET; return; }
    if ((op & 0xC7) == 0xC0) { in->cf = Z80_CF_RET_COND; return; }
    if (op == 0xE9) { in->cf = Z80_CF_JUMP; in->has_target = false; return; } /* jp (hl) */
    in->cf = Z80_CF_NONE;
}

int z80_decode(const uint8_t *p, size_t avail, uint16_t z80_pc, Z80Insn *out) {
    memset(out, 0, sizeof(*out));
    if (avail == 0) return 0;

    size_t i = 0;          /* index into p */
    Z80Prefix pfx = Z80_PFX_NONE;

    /* Consume DD/FD prefixes; on a repeated DD/FD, "last wins" — the earlier
     * one is a 1-cycle effective no-op. ED/CB after DD/FD form their groups. */
    while (i < avail && (p[i] == 0xDD || p[i] == 0xFD)) {
        pfx = (p[i] == 0xDD) ? Z80_PFX_DD : Z80_PFX_FD;
        i++;
        if (i < avail && (p[i] == 0xDD || p[i] == 0xFD)) continue; /* last wins */
        break;
    }

    if (i >= avail) { out->illegal = true; out->length = (uint8_t)avail; return out->length; }

    uint8_t b = p[i];

    if (b == 0xCB) {
        /* CB or (DD/FD)CB */
        if (pfx == Z80_PFX_DD || pfx == Z80_PFX_FD) {
            /* DD CB d op : 4 bytes total counting the DD/FD already at i-1 */
            out->prefix  = (pfx == Z80_PFX_DD) ? Z80_PFX_DDCB : Z80_PFX_FDCB;
            out->disp    = (i + 1 < avail) ? (int8_t)p[i + 1] : 0;
            out->uses_disp = true;
            out->opcode  = (i + 2 < avail) ? p[i + 2] : 0;
            out->length  = (uint8_t)(i + 3);     /* prefix(1) + CB + d + op */
        } else {
            out->prefix = Z80_PFX_CB;
            out->opcode = (i + 1 < avail) ? p[i + 1] : 0;
            out->length = (uint8_t)(i + 2);
        }
        out->cf = Z80_CF_NONE; /* CB group is bit/rot/shift only */
        uint8_t cop = out->opcode;
        int cy = (cop >> 3) & 7, cz = cop & 7, cx = cop >> 6;
        const char *tgt = out->uses_disp ? "(ix+d)" : R8[cz];
        if (cx == 0)      snprintf(out->text, sizeof(out->text), "%s %s", ROT[cy], tgt);
        else if (cx == 1) snprintf(out->text, sizeof(out->text), "bit %d,%s", cy, tgt);
        else if (cx == 2) snprintf(out->text, sizeof(out->text), "res %d,%s", cy, tgt);
        else              snprintf(out->text, sizeof(out->text), "set %d,%s", cy, tgt);
        goto finish;
    }

    if (b == 0xED) {
        out->prefix = Z80_PFX_ED;
        out->opcode = (i + 1 < avail) ? p[i + 1] : 0;
        uint8_t eop = out->opcode;
        /* LD (nn),rp and LD rp,(nn) (ED 43/4B/53/5B/63/6B/73/7B) carry a 16-bit
         * operand and are 4 bytes total; all other ED instructions are 2.
         * (Getting this length wrong misaligns every following instruction.) */
        if ((eop & 0xC7) == 0x43) {
            uint8_t lo = (i + 2 < avail) ? p[i + 2] : 0;
            uint8_t hi = (i + 3 < avail) ? p[i + 3] : 0;
            out->imm = (uint16_t)(lo | (hi << 8));
            out->imm_bits = 16;
            out->length = (uint8_t)(i + 4);
        } else {
            out->length = (uint8_t)(i + 2);
        }
        if (eop == 0x4D) { out->cf = Z80_CF_RET; snprintf(out->text,sizeof(out->text),"reti"); }
        else if (eop == 0x45 || eop == 0x55 || eop == 0x65 || eop == 0x75) {
            out->cf = Z80_CF_RET; snprintf(out->text,sizeof(out->text),"retn");
        } else {
            out->cf = Z80_CF_NONE;
            snprintf(out->text, sizeof(out->text), "ed $%02X", eop);
        }
        goto finish;
    }

    /* Plain opcode (possibly under DD/FD). */
    {
        out->prefix = pfx;            /* NONE / DD / FD */
        out->opcode = b;
        uint8_t bl = base_len[b];
        if (bl == 0) {                /* should not happen here: CB/ED handled, DD/FD consumed */
            out->illegal = true; out->length = (uint8_t)(i + 1); goto finish;
        }
        bool disp = (pfx != Z80_PFX_NONE) && op_uses_hl_mem(b);
        size_t opstart = i;           /* opcode byte index */

        /* Capture operand bytes from the opcode's own stream (after the opcode
         * byte, after the displacement byte if present). */
        size_t operand_at = opstart + 1 + (disp ? 1 : 0);
        if (bl == 2) {                /* one operand byte (n / e / port) */
            out->imm = (operand_at < avail) ? p[operand_at] : 0;
            out->imm_bits = 8;
        } else if (bl == 3) {         /* 16-bit immediate */
            uint8_t lo = (operand_at     < avail) ? p[operand_at]     : 0;
            uint8_t hi = (operand_at + 1 < avail) ? p[operand_at + 1] : 0;
            out->imm = (uint16_t)(lo | (hi << 8));
            out->imm_bits = 16;
        }
        if (disp) { out->disp = (int8_t)((opstart + 1 < avail) ? p[opstart + 1] : 0);
                    out->uses_disp = true; }

        out->is_halt = (b == 0x76 && pfx == Z80_PFX_NONE);
        out->length  = (uint8_t)(opstart + 1 + (disp ? 1 : 0) + (bl - 1));
        /* bl already includes the opcode's own operand bytes (bl-1 of them). */

        classify_base(out, z80_pc);
        fmt_base(out, p + opstart);
    }

finish:
    if (out->length == 0) out->length = 1;
    if (out->length > 4) out->length = 4; /* defensive: Z80 max is 4 bytes */
    for (int k = 0; k < out->length && (size_t)k < avail && k < 4; k++)
        out->raw[k] = p[k];
    return out->length;
}
