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

/* INC/DEC (HL): read-modify-write the byte at HL through the bus (flags as INC/DEC r). */
void z80h_inc_hlm(Z80State *s, const Bus *b){ uint16_t a=z80_hl(s); b->write8(b->ctx,a,z80_inc8(s,b->read8(b->ctx,a))); }
void z80h_dec_hlm(Z80State *s, const Bus *b){ uint16_t a=z80_hl(s); b->write8(b->ctx,a,z80_dec8(s,b->read8(b->ctx,a))); }

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

static uint8_t *regp(Z80State *s, long i);   /* defined below (8-bit reg index -> ptr) */
/* CB-prefixed op (rotate/shift, BIT, RES, SET) on register or (HL). `cb` is the byte
 * after the CB prefix; reg index = cb&7 (6 = (HL), via the bus). All flag behavior comes
 * from z80_ops.h so it matches the static path. BIT is read-only; the rest write back. */
void z80h_cb(Z80State *s, const Bus *b, long cb_byte){
    uint8_t cb = (uint8_t)cb_byte; int reg = cb & 7;
    uint16_t a = z80_hl(s);
    uint8_t v = (reg == 6) ? b->read8(b->ctx, a) : *regp(s, reg);
    uint8_t res = v; int wb = 1;
    if (cb < 0x40){
        switch ((cb>>3)&7){
            case 0: res=z80_rlc(s,v); break; case 1: res=z80_rrc(s,v); break;
            case 2: res=z80_rl (s,v); break; case 3: res=z80_rr (s,v); break;
            case 4: res=z80_sla(s,v); break; case 5: res=z80_sra(s,v); break;
            case 6: res=z80_sll(s,v); break; default: res=z80_srl(s,v); break;
        }
    } else if (cb < 0x80){                                   /* BIT n,r — read-only */
        uint8_t xy = (reg == 6) ? (uint8_t)(s->wz >> 8) : v; /* (HL): X/Y from WZ high */
        z80_bit(s, (cb>>3)&7, v, xy); wb = 0;
    } else if (cb < 0xC0){ res = (uint8_t)(v & ~(1u << ((cb>>3)&7))); } /* RES n,r */
    else                 { res = (uint8_t)(v |  (1u << ((cb>>3)&7))); } /* SET n,r */
    if (wb){ if (reg == 6) b->write8(b->ctx, a, res); else *regp(s, reg) = res; }
}

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

/* ---- accumulator/flag misc + register exchanges ---- */
void z80h_cpl(Z80State *s){ z80_cpl(s); }
void z80h_daa(Z80State *s){ z80_daa(s); }
void z80h_scf(Z80State *s){ z80_scf(s); }
void z80h_ccf(Z80State *s){ z80_ccf(s); }
void z80h_ex_af  (Z80State *s){ uint8_t t; t=s->a;s->a=s->a_;s->a_=t; t=s->f;s->f=s->f_;s->f_=t; }
void z80h_ex_dehl(Z80State *s){ uint8_t t; t=s->d;s->d=s->h;s->h=t; t=s->e;s->e=s->l;s->l=t; }
void z80h_exx(Z80State *s){ uint8_t t;
    t=s->b;s->b=s->b_;s->b_=t; t=s->c;s->c=s->c_;s->c_=t; t=s->d;s->d=s->d_;s->d_=t;
    t=s->e;s->e=s->e_;s->e_=t; t=s->h;s->h=s->h_;s->h_=t; t=s->l;s->l=s->l_;s->l_=t; }

/* ---- I/O + CALL ---- */
void z80h_out_n(Z80State *s, const Bus *b, long n){ b->io_out(b->ctx, (uint8_t)n, s->a); }

/* CALL/RST: push the return address, run the callee via the bus, and report whether
 * the callee unwound PAST this frame (the shard must then return too — the computed-
 * call propagation contract). The bus->call runs the callee on the live dispatcher
 * (game thread) or under superzazu on the snapshot (off-thread validation). */
long z80h_call(Z80State *s, const Bus *b, long target_ret){
    uint16_t target = (uint16_t)(target_ret & 0xFFFF);
    uint16_t ret    = (uint16_t)((target_ret >> 16) & 0xFFFF);
    uint16_t csp = s->sp;
    s->sp = (uint16_t)(s->sp - 2);
    b->write8(b->ctx, s->sp, (uint8_t)ret);
    b->write8(b->ctx, (uint16_t)(s->sp + 1), (uint8_t)(ret >> 8));
    b->call(b->ctx, s, target);
    return (int16_t)(s->sp - csp) > 0;   /* nonzero => callee returned past us */
}

/* JP (HL): computed TAIL dispatch (the jump-table dispatcher idiom). No return is
 * pushed — control transfers to HL and the eventual RET unwinds to OUR caller, so
 * the shard re-enters the dispatcher at HL and then returns. If a particular site
 * were actually a computed CALL (push <ret>; jp (hl)), the target would return into
 * THIS function instead of past it — the off-thread validation gate catches that
 * divergence and declines the shard, so emitting the tail form is precision-safe. */
void z80h_jp_hl(Z80State *s, const Bus *b){
    b->call(b->ctx, s, (uint16_t)((s->h << 8) | s->l));
}

/* JP/JR nn whose static target lies OUTSIDE this shard's function window: a tail jump
 * to another function. Same tail semantics as z80h_jp_hl — dispatch to the target and
 * return (the eventual RET unwinds to our caller). Precision-safe via the same gate. */
void z80h_jp_to(Z80State *s, const Bus *b, long target){
    b->call(b->ctx, s, (uint16_t)target);
}

#endif /* SMS_HAVE_JIT */
