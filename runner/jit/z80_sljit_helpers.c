/*
 * z80_sljit_helpers.c — flag-exact helpers the emitted shards CALL.
 *
 * Each wraps the SAME z80_ops.h semantic core the generated static C uses, so a
 * shard's result (value + flags on Z80State) is identical to the static path by
 * construction — and the static path is validated against superzazu. Word-sized
 * signatures match the sljit call ABI (SLJIT_ARGS*(W/V, P, W)).
 */
#ifdef SMS_HAVE_JIT

#include "jit_abi.h"
#include "z80_ops.h"

/* INC/DEC r: return the new value (emitter stores it); flags set on s. */
long z80h_inc8(Z80State *s, long v){ return (long)z80_inc8(s, (uint8_t)v); }
long z80h_dec8(Z80State *s, long v){ return (long)z80_dec8(s, (uint8_t)v); }

/* 8-bit ALU on the accumulator: read A from s, compute, write A (+flags) back.
 * CP writes flags only. Indexed by the Z80 ALU group (y field 0..7). */
void z80h_add(Z80State *s, long v){ s->a = z80_add8(s, s->a, (uint8_t)v, 0); }
void z80h_adc(Z80State *s, long v){ s->a = z80_add8(s, s->a, (uint8_t)v, z80f_c(s)); }
void z80h_sub(Z80State *s, long v){ s->a = z80_sub8(s, s->a, (uint8_t)v, 0); }
void z80h_sbc(Z80State *s, long v){ s->a = z80_sub8(s, s->a, (uint8_t)v, z80f_c(s)); }
void z80h_and(Z80State *s, long v){ s->a = z80_and8(s, s->a, (uint8_t)v); }
void z80h_xor(Z80State *s, long v){ s->a = z80_xor8(s, s->a, (uint8_t)v); }
void z80h_or (Z80State *s, long v){ s->a = z80_or8 (s, s->a, (uint8_t)v); }
void z80h_cp (Z80State *s, long v){ z80_cp8(s, s->a, (uint8_t)v); }

/* accumulator rotates (RLCA/RRCA/RLA/RRA) */
void z80h_rlca(Z80State *s){ z80_rlca(s); }
void z80h_rrca(Z80State *s){ z80_rrca(s); }
void z80h_rla (Z80State *s){ z80_rla(s); }
void z80h_rra (Z80State *s){ z80_rra(s); }

/* ---- memory + 16-bit + stack (the bus carries all guest memory access) ----
 * "Fat" helpers: addressing + bus access live in C so the emitter stays simple
 * and the same code path runs live or on a validation snapshot. ri = 8-bit
 * r-table index; pp = 16-bit pair index (0=BC 1=DE 2=HL 3=SP); p2 = push/pop pair
 * index (0=BC 1=DE 2=HL 3=AF). */
static uint8_t *regp(Z80State *s, long i){
    switch (i){ case 0:return &s->b; case 1:return &s->c; case 2:return &s->d;
                case 3:return &s->e; case 4:return &s->h; case 5:return &s->l;
                default:return &s->a; }
}
void z80h_ld_r_hl(Z80State *s, const Bus *b, long ri){ *regp(s,ri) = b->read8(b->ctx, z80_hl(s)); }
void z80h_ld_hl_r(Z80State *s, const Bus *b, long ri){ b->write8(b->ctx, z80_hl(s), *regp(s,ri)); }
void z80h_ld_hl_n(Z80State *s, const Bus *b, long n ){ b->write8(b->ctx, z80_hl(s), (uint8_t)n); }
void z80h_ld_a_bc(Z80State *s, const Bus *b){ s->a = b->read8(b->ctx, z80_bc(s)); }
void z80h_ld_a_de(Z80State *s, const Bus *b){ s->a = b->read8(b->ctx, z80_de(s)); }
void z80h_ld_a_nn(Z80State *s, const Bus *b, long nn){ s->a = b->read8(b->ctx, (uint16_t)nn); }
void z80h_ld_bc_a(Z80State *s, const Bus *b){ b->write8(b->ctx, z80_bc(s), s->a); }
void z80h_ld_de_a(Z80State *s, const Bus *b){ b->write8(b->ctx, z80_de(s), s->a); }
void z80h_ld_nn_a(Z80State *s, const Bus *b, long nn){ b->write8(b->ctx, (uint16_t)nn, s->a); }

void z80h_ld_rr (Z80State *s, long pp, long nn){ uint16_t v=(uint16_t)nn;
    switch(pp){case 0:z80_set_bc(s,v);break;case 1:z80_set_de(s,v);break;case 2:z80_set_hl(s,v);break;default:s->sp=v;break;} }
void z80h_inc_rr(Z80State *s, long pp){
    switch(pp){case 0:z80_set_bc(s,(uint16_t)(z80_bc(s)+1));break;case 1:z80_set_de(s,(uint16_t)(z80_de(s)+1));break;
               case 2:z80_set_hl(s,(uint16_t)(z80_hl(s)+1));break;default:s->sp++;break;} }
void z80h_dec_rr(Z80State *s, long pp){
    switch(pp){case 0:z80_set_bc(s,(uint16_t)(z80_bc(s)-1));break;case 1:z80_set_de(s,(uint16_t)(z80_de(s)-1));break;
               case 2:z80_set_hl(s,(uint16_t)(z80_hl(s)-1));break;default:s->sp--;break;} }
void z80h_add_hl(Z80State *s, long pp){ uint16_t o;
    switch(pp){case 0:o=z80_bc(s);break;case 1:o=z80_de(s);break;case 2:o=z80_hl(s);break;default:o=s->sp;break;}
    z80_set_hl(s, z80_add16(s, z80_hl(s), o)); }
void z80h_push(Z80State *s, const Bus *b, long p2){ uint16_t v;
    switch(p2){case 0:v=z80_bc(s);break;case 1:v=z80_de(s);break;case 2:v=z80_hl(s);break;default:v=z80_af(s);break;}
    s->sp=(uint16_t)(s->sp-2); b->write8(b->ctx,s->sp,(uint8_t)v); b->write8(b->ctx,(uint16_t)(s->sp+1),(uint8_t)(v>>8)); }
void z80h_pop(Z80State *s, const Bus *b, long p2){
    uint16_t lo=b->read8(b->ctx,s->sp), hi=b->read8(b->ctx,(uint16_t)(s->sp+1)), v=(uint16_t)(lo|(hi<<8));
    s->sp=(uint16_t)(s->sp+2);
    switch(p2){case 0:z80_set_bc(s,v);break;case 1:z80_set_de(s,v);break;case 2:z80_set_hl(s,v);break;default:z80_set_af(s,v);break;} }

#endif /* SMS_HAVE_JIT */
