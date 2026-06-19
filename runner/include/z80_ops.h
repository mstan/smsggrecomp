/*
 * z80_ops.h — Z80 ALU / flag / rotate / shift semantic helpers.
 *
 * The single verified home for Z80 flag semantics. Both the recompiled game
 * code (the generated TUs) and the runner's hybrid interpreter call these; the
 * logic is ported verbatim from the vendored MIT superzazu/z80.c (which the
 * decoder self-test and, later, the oracle diff validate against).
 *
 * Operates on Z80State with the packed F byte (S Z Y H X P N C = bits 7..0).
 * All are static inline so they cost nothing across translation units.
 */
#ifndef Z80_OPS_H
#define Z80_OPS_H

#include "sms_runtime.h"

/* ---- flag access ---- */
static inline void z80f_set(Z80State *s, uint8_t mask, bool v) {
    if (v) s->f |= mask; else s->f &= (uint8_t)~mask;
}
static inline bool z80f_get(const Z80State *s, uint8_t mask) { return (s->f & mask) != 0; }
static inline bool z80f_c(const Z80State *s) { return (s->f & Z80_FLAG_C) != 0; }

#define GB(n, v) (((v) >> (n)) & 1)

/* carry out of bit `bit` for a + b + cy */
static inline bool z80_carry(int bit, uint16_t a, uint16_t b, bool cy) {
    int32_t r = (int32_t)a + (int32_t)b + (int32_t)cy;
    int32_t c = r ^ a ^ b;
    return (c & (1 << bit)) != 0;
}
static inline bool z80_parity8(uint8_t v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (v & 1) == 0;
}
/* Compose the S, Z, X(bit3), Y(bit5) flag bits from a result byte. The X/Y
 * flag masks (0x08/0x20) coincide with result bits 3/5, so `v & 0x28` lifts
 * them directly. */
static inline uint8_t z80_szxy(uint8_t v) {
    uint8_t f = (uint8_t)(v & 0x28);
    if (v & 0x80) f |= Z80_FLAG_S;
    if (v == 0)   f |= Z80_FLAG_Z;
    return f;
}

/* ---- 8-bit add / sub / inc / dec / cp ---- */
static inline uint8_t z80_add8(Z80State *s, uint8_t a, uint8_t b, bool cy) {
    uint8_t r = (uint8_t)(a + b + cy);
    uint8_t f = 0;
    if (r & 0x80) f |= Z80_FLAG_S;
    if (r == 0)   f |= Z80_FLAG_Z;
    if (z80_carry(4, a, b, cy)) f |= Z80_FLAG_H;
    if (z80_carry(7, a, b, cy) != z80_carry(8, a, b, cy)) f |= Z80_FLAG_P;
    if (z80_carry(8, a, b, cy)) f |= Z80_FLAG_C;
    if (r & 0x08) f |= Z80_FLAG_X;
    if (r & 0x20) f |= Z80_FLAG_Y;
    s->f = f; /* N = 0 */
    return r;
}
static inline uint8_t z80_sub8(Z80State *s, uint8_t a, uint8_t b, bool cy) {
    uint8_t r = z80_add8(s, a, (uint8_t)~b, !cy);
    s->f ^= (Z80_FLAG_C | Z80_FLAG_H);   /* invert C and H */
    s->f |= Z80_FLAG_N;
    return r;
}
static inline uint8_t z80_inc8(Z80State *s, uint8_t a) {
    bool cf = z80f_c(s); uint8_t r = z80_add8(s, a, 1, 0); z80f_set(s, Z80_FLAG_C, cf); return r;
}
static inline uint8_t z80_dec8(Z80State *s, uint8_t a) {
    bool cf = z80f_c(s); uint8_t r = z80_sub8(s, a, 1, 0); z80f_set(s, Z80_FLAG_C, cf); return r;
}
/* CP: like SUB but discard result; X/Y come from the operand, not the result. */
static inline void z80_cp8(Z80State *s, uint8_t a, uint8_t b) {
    z80_sub8(s, a, b, 0);
    z80f_set(s, Z80_FLAG_X, GB(3, b));
    z80f_set(s, Z80_FLAG_Y, GB(5, b));
}

/* ---- logic (target is always A) ---- */
static inline uint8_t z80_and8(Z80State *s, uint8_t a, uint8_t b) {
    uint8_t r = a & b;
    uint8_t f = Z80_FLAG_H | z80_szxy(r);
    if (z80_parity8(r)) f |= Z80_FLAG_P;
    s->f = f; return r;
}
static inline uint8_t z80_or8(Z80State *s, uint8_t a, uint8_t b) {
    uint8_t r = a | b;
    uint8_t f = z80_szxy(r);
    if (z80_parity8(r)) f |= Z80_FLAG_P;
    s->f = f; return r;
}
static inline uint8_t z80_xor8(Z80State *s, uint8_t a, uint8_t b) {
    uint8_t r = a ^ b;
    uint8_t f = z80_szxy(r);
    if (z80_parity8(r)) f |= Z80_FLAG_P;
    s->f = f; return r;
}

/* ---- 16-bit ADD / ADC / SBC ---- */
/* ADD HL/IX/IY, rr : sets H,C,N,X,Y from the add; preserves S,Z,P. */
static inline uint16_t z80_add16(Z80State *s, uint16_t a, uint16_t b) {
    uint8_t keep = s->f & (Z80_FLAG_S | Z80_FLAG_Z | Z80_FLAG_P);
    uint8_t lo = z80_add8(s, (uint8_t)a, (uint8_t)b, 0);
    uint8_t hi = z80_add8(s, (uint8_t)(a >> 8), (uint8_t)(b >> 8), z80f_c(s));
    uint16_t r = (uint16_t)((hi << 8) | lo);
    s->f = (uint8_t)((s->f & (Z80_FLAG_C | Z80_FLAG_H | Z80_FLAG_X | Z80_FLAG_Y)) | keep);
    s->wz = (uint16_t)(a + 1);
    return r;
}
/* ADC HL, rr : full flags. */
static inline uint16_t z80_adc16(Z80State *s, uint16_t a, uint16_t b) {
    bool cy = z80f_c(s);
    uint8_t lo = z80_add8(s, (uint8_t)a, (uint8_t)b, cy);
    uint8_t hi = z80_add8(s, (uint8_t)(a >> 8), (uint8_t)(b >> 8), z80f_c(s));
    uint16_t r = (uint16_t)((hi << 8) | lo);
    z80f_set(s, Z80_FLAG_Z, r == 0);
    z80f_set(s, Z80_FLAG_S, (r >> 15) != 0);
    s->wz = (uint16_t)(a + 1);
    return r;
}
/* SBC HL, rr : full flags. */
static inline uint16_t z80_sbc16(Z80State *s, uint16_t a, uint16_t b) {
    bool cy = z80f_c(s);
    uint8_t lo = z80_sub8(s, (uint8_t)a, (uint8_t)b, cy);
    uint8_t hi = z80_sub8(s, (uint8_t)(a >> 8), (uint8_t)(b >> 8), z80f_c(s));
    uint16_t r = (uint16_t)((hi << 8) | lo);
    z80f_set(s, Z80_FLAG_Z, r == 0);
    z80f_set(s, Z80_FLAG_S, (r >> 15) != 0);
    s->wz = (uint16_t)(a + 1);
    return r;
}

/* ---- accumulator rotates (RLCA/RRCA/RLA/RRA): only H,N,C,X,Y change ---- */
/* Accumulator rotates clear N,H and set C,X,Y; S,Z,P are preserved. */
static inline void z80_arot_flags(Z80State *s, bool c) {
    s->f &= (uint8_t)~(Z80_FLAG_N | Z80_FLAG_H | Z80_FLAG_C | Z80_FLAG_X | Z80_FLAG_Y);
    if (c) s->f |= Z80_FLAG_C;
    s->f |= (uint8_t)(s->a & 0x28);
}
static inline void z80_rlca(Z80State *s) {
    bool c = (s->a >> 7) != 0; s->a = (uint8_t)((s->a << 1) | c); z80_arot_flags(s, c);
}
static inline void z80_rrca(Z80State *s) {
    bool c = (s->a & 1) != 0; s->a = (uint8_t)((s->a >> 1) | (c << 7)); z80_arot_flags(s, c);
}
static inline void z80_rla(Z80State *s) {
    bool old = z80f_c(s); bool c = (s->a >> 7) != 0; s->a = (uint8_t)((s->a << 1) | old); z80_arot_flags(s, c);
}
static inline void z80_rra(Z80State *s) {
    bool old = z80f_c(s); bool c = (s->a & 1) != 0; s->a = (uint8_t)((s->a >> 1) | (old << 7)); z80_arot_flags(s, c);
}

/* ---- CB rotates/shifts: full SZ P, H=N=0, X/Y from result ---- */
static inline uint8_t z80_cb_szp(Z80State *s, uint8_t v, bool c) {
    uint8_t f = z80_szxy(v);
    if (z80_parity8(v)) f |= Z80_FLAG_P;
    if (c) f |= Z80_FLAG_C;
    s->f = f; return v;
}
static inline uint8_t z80_rlc(Z80State *s, uint8_t v){ bool c=v>>7; return z80_cb_szp(s,(uint8_t)((v<<1)|c),c); }
static inline uint8_t z80_rrc(Z80State *s, uint8_t v){ bool c=v&1;  return z80_cb_szp(s,(uint8_t)((v>>1)|(c<<7)),c); }
static inline uint8_t z80_rl (Z80State *s, uint8_t v){ bool old=z80f_c(s); bool c=v>>7; return z80_cb_szp(s,(uint8_t)((v<<1)|old),c); }
static inline uint8_t z80_rr (Z80State *s, uint8_t v){ bool old=z80f_c(s); bool c=v&1;  return z80_cb_szp(s,(uint8_t)((v>>1)|(old<<7)),c); }
static inline uint8_t z80_sla(Z80State *s, uint8_t v){ bool c=v>>7; return z80_cb_szp(s,(uint8_t)(v<<1),c); }
static inline uint8_t z80_sll(Z80State *s, uint8_t v){ bool c=v>>7; return z80_cb_szp(s,(uint8_t)((v<<1)|1),c); }
static inline uint8_t z80_sra(Z80State *s, uint8_t v){ bool c=v&1;  return z80_cb_szp(s,(uint8_t)((v>>1)|(v&0x80)),c); }
static inline uint8_t z80_srl(Z80State *s, uint8_t v){ bool c=v&1;  return z80_cb_szp(s,(uint8_t)(v>>1),c); }

/* BIT n,r : Z/P from tested bit, H=1, N=0, S if bit7 tested & set, X/Y from v.
 * (For BIT n,(HL)/(IX+d) the X/Y come from WZ/addr high byte — codegen passes
 * that as `xy` where it differs; for register form pass v.) */
static inline void z80_bit(Z80State *s, uint8_t n, uint8_t v, uint8_t xy) {
    uint8_t r = v & (uint8_t)(1 << n);
    uint8_t f = (uint8_t)(s->f & Z80_FLAG_C); /* C preserved */
    f |= Z80_FLAG_H;
    if (r == 0) f |= (Z80_FLAG_Z | Z80_FLAG_P);
    if (r & 0x80) f |= Z80_FLAG_S;
    f |= (uint8_t)(xy & 0x28);
    s->f = f;
}

/* ---- misc accumulator ops ---- */
static inline void z80_daa(Z80State *s) {
    uint8_t corr = 0;
    bool cf = z80f_c(s), hf = z80f_get(s, Z80_FLAG_H), nf = z80f_get(s, Z80_FLAG_N);
    bool newc = false;
    if ((s->a & 0x0F) > 0x09 || hf) corr += 0x06;
    if (s->a > 0x99 || cf) { corr += 0x60; newc = true; }
    bool newh;
    if (nf) { newh = hf && ((s->a & 0x0F) < 0x06); s->a = (uint8_t)(s->a - corr); }
    else    { newh = (s->a & 0x0F) > 0x09;        s->a = (uint8_t)(s->a + corr); }
    uint8_t f = (uint8_t)(nf ? Z80_FLAG_N : 0);
    if (newc) f |= Z80_FLAG_C;
    if (newh) f |= Z80_FLAG_H;
    f |= z80_szxy(s->a);
    if (z80_parity8(s->a)) f |= Z80_FLAG_P;
    s->f = f;
}
static inline void z80_cpl(Z80State *s) {
    s->a = (uint8_t)~s->a;
    s->f |= (Z80_FLAG_N | Z80_FLAG_H);
    z80f_set(s, Z80_FLAG_X, GB(3, s->a)); z80f_set(s, Z80_FLAG_Y, GB(5, s->a));
}
static inline void z80_neg(Z80State *s) { s->a = z80_sub8(s, 0, s->a, 0); }
static inline void z80_scf(Z80State *s) {
    s->f &= (uint8_t)~(Z80_FLAG_N | Z80_FLAG_H); s->f |= Z80_FLAG_C;
    z80f_set(s, Z80_FLAG_X, GB(3, s->a)); z80f_set(s, Z80_FLAG_Y, GB(5, s->a));
}
static inline void z80_ccf(Z80State *s) {
    z80f_set(s, Z80_FLAG_H, z80f_c(s));
    z80f_set(s, Z80_FLAG_C, !z80f_c(s));
    s->f &= (uint8_t)~Z80_FLAG_N;
    z80f_set(s, Z80_FLAG_X, GB(3, s->a)); z80f_set(s, Z80_FLAG_Y, GB(5, s->a));
}

#undef GB
#endif /* Z80_OPS_H */
