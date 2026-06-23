/*
 * z80_sljit_diff.c — P1 differential harness for the Z80 -> sljit emitter.
 *
 * For each random case: synthesize a short sequence of SUPPORTED ops + RET, run it
 * (a) through the superzazu interpreter (the oracle) and (b) through the emitted
 * shard, from the SAME seeded Z80State, and assert the architectural register file
 * + flags + cyc are byte-identical. pc is excluded (the recomp model carries control
 * via the C stack, not the Z80 pc).
 *
 * Standalone bring-up test — not part of any game build.
 */
#include "jit_abi.h"
#include "z80_sljit.h"
#include "sljitLir.h"            /* sljit_free_code */
#include "z80.h"                 /* superzazu */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_mem[0x10000];

/* ---- sandbox bus (shard side) + superzazu callbacks over the same g_mem ---- */
static uint8_t sb_r8 (void *c, uint16_t a){ (void)c; return g_mem[a]; }
static void    sb_w8 (void *c, uint16_t a, uint8_t v){ (void)c; g_mem[a] = v; }
static uint8_t sb_in (void *c, uint8_t p){ (void)c; (void)p; return 0xFF; }
static void    sb_out(void *c, uint8_t p, uint8_t v){ (void)c; (void)p; (void)v; }
static const Bus g_bus = { sb_r8, sb_w8, sb_in, sb_out, NULL };

static uint8_t sz_r8(void *u, uint16_t a){ (void)u; return g_mem[a]; }
static void    sz_w8(void *u, uint16_t a, uint8_t v){ (void)u; g_mem[a] = v; }
static uint8_t sz_in(z80 *z, uint8_t p){ (void)z; (void)p; return 0xFF; }
static void    sz_out(z80 *z, uint8_t p, uint8_t v){ (void)z; (void)p; (void)v; }

static uint8_t pack_f(const z80 *z){
    return (uint8_t)((z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|
                     (z->xf<<3)|(z->pf<<2)|(z->nf<<1)|(z->cf));
}
static void unpack_f(z80 *z, uint8_t f){
    z->sf=(f>>7)&1; z->zf=(f>>6)&1; z->yf=(f>>5)&1; z->hf=(f>>4)&1;
    z->xf=(f>>3)&1; z->pf=(f>>2)&1; z->nf=(f>>1)&1; z->cf=f&1;
}

static const int SUP_REG[7] = {0,1,2,3,4,5,7};   /* B C D E H L A (no (HL)) */

int main(int argc, char **argv){
    long iters = (argc > 1) ? strtol(argv[1], NULL, 10) : 200000;
    unsigned seed = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10) : 1;
    srand(seed);

    const uint16_t base = 0x1000;
    long fails = 0, declined = 0;

    for (long it = 0; it < iters; it++){
        /* --- synthesize a supported op sequence + RET at g_mem[base] --- */
        int count = rand() % 9;          /* 0..8 ops, then RET */
        size_t len = 0;
        for (int k = 0; k < count; k++){
            static const uint8_t ROT[4] = {0x07,0x0F,0x17,0x1F};
            int kind = rand() % 8;
            int dr = SUP_REG[rand() % 7], sr = SUP_REG[rand() % 7], g = rand() % 8;
            switch (kind){
                case 0: g_mem[base+len++] = 0x00; break;                         /* NOP */
                case 1: g_mem[base+len++] = (uint8_t)(0x40 | (dr<<3) | sr); break;/* LD r,r' */
                case 2: g_mem[base+len++] = (uint8_t)(0x06 | (dr<<3));
                        g_mem[base+len++] = (uint8_t)(rand() & 0xFF); break;      /* LD r,n */
                case 3: g_mem[base+len++] = (uint8_t)(0x04 | (dr<<3)); break;     /* INC r */
                case 4: g_mem[base+len++] = (uint8_t)(0x05 | (dr<<3)); break;     /* DEC r */
                case 5: g_mem[base+len++] = (uint8_t)(0x80 | (g<<3) | sr); break; /* ALU A,r */
                case 6: g_mem[base+len++] = (uint8_t)(0xC6 | (g<<3));
                        g_mem[base+len++] = (uint8_t)(rand() & 0xFF); break;      /* ALU A,n */
                case 7: g_mem[base+len++] = ROT[rand() % 4]; break;              /* RLCA/RRCA/RLA/RRA */
            }
        }
        g_mem[base+len++] = 0xC9;        /* RET */

        /* --- random seed state --- */
        Z80State seed_st; memset(&seed_st, 0, sizeof seed_st);
        seed_st.a=rand(); seed_st.f=rand(); seed_st.b=rand(); seed_st.c=rand();
        seed_st.d=rand(); seed_st.e=rand(); seed_st.h=rand(); seed_st.l=rand();
        seed_st.ix=rand(); seed_st.iy=rand();
        seed_st.sp=(uint16_t)(0x4000 + (rand()&0x1FF));   /* clear of the code at base */
        seed_st.cyc=0;

        /* --- oracle: superzazu --- */
        z80 z; z80_init(&z);
        z.read_byte=sz_r8; z.write_byte=sz_w8; z.port_in=sz_in; z.port_out=sz_out; z.userdata=NULL;
        z.a=seed_st.a; z.b=seed_st.b; z.c=seed_st.c; z.d=seed_st.d;
        z.e=seed_st.e; z.h=seed_st.h; z.l=seed_st.l;
        z.ix=seed_st.ix; z.iy=seed_st.iy; z.sp=seed_st.sp; z.pc=base; z.cyc=0;
        unpack_f(&z, seed_st.f);
        for (int s = 0; s < count + 1; s++) z80_step(&z);   /* ops + RET */

        /* --- shard --- */
        ShardFn fn = z80_sljit_compile(&g_mem[base], len, base);
        if (!fn){ declined++; fprintf(stderr, "[diff] unexpected decline (len=%zu)\n", len); continue; }
        Z80State st = seed_st;
        fn(&st, &g_bus);

        /* --- compare (exclude pc) --- */
        int bad = (st.a!=z.a)||(st.f!=pack_f(&z))||(st.b!=z.b)||(st.c!=z.c)||
                  (st.d!=z.d)||(st.e!=z.e)||(st.h!=z.h)||(st.l!=z.l)||
                  (st.ix!=z.ix)||(st.iy!=z.iy)||(st.sp!=z.sp)||
                  (st.cyc!=(uint64_t)z.cyc);
        if (bad){
            if (fails < 10){
                fprintf(stderr, "[diff] MISMATCH (it=%ld, count=%d):\n", it, count);
                fprintf(stderr, "  shard: A=%02X F=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X cyc=%llu\n",
                        st.a, st.f, st.b, st.c, st.d, st.e, st.h, st.l, st.sp, (unsigned long long)st.cyc);
                fprintf(stderr, "  oracl: A=%02X F=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X cyc=%lu\n",
                        z.a, pack_f(&z), z.b, z.c, z.d, z.e, z.h, z.l, z.sp, z.cyc);
            }
            fails++;
        }
        sljit_free_code((void*)fn, NULL);
    }

    printf("z80_sljit_diff: %ld cases, %ld declined, %ld FAILS\n", iters, declined, fails);
    printf("%s\n", (fails == 0 && declined == 0) ? "PASS" : "FAIL");
    return (fails == 0 && declined == 0) ? 0 : 1;
}
