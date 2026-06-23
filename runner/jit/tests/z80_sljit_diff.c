/*
 * z80_sljit_diff.c — differential harness for the Z80 -> sljit emitter.
 *
 * For each random case: synthesize a short sequence of SUPPORTED ops + RET, run it
 * through (a) the emitted shard and (b) the superzazu interpreter from the SAME
 * seeded state, each over its OWN memory copy, and assert the architectural
 * registers + flags + cyc + memory are byte-identical. pc is excluded (the recomp
 * model carries control via the C stack). Pointer registers are seeded into a safe
 * RAM region so writes don't self-modify the code stream.
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

static uint8_t g_img[0x10000];   /* base image (code) */
static uint8_t g_A[0x10000], g_B[0x10000];   /* shard / superzazu working copies */

static uint8_t bus_r8(void *c, uint16_t a){ return ((uint8_t*)c)[a]; }
static void    bus_w8(void *c, uint16_t a, uint8_t v){ ((uint8_t*)c)[a] = v; }
static uint8_t bus_in(void *c, uint8_t p){ (void)c; (void)p; return 0xFF; }
static void    bus_out(void *c, uint8_t p, uint8_t v){ (void)c; (void)p; (void)v; }
static uint8_t sz_in (z80 *z, uint8_t p){ (void)z; (void)p; return 0xFF; }
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
    long iters = (argc > 1) ? strtol(argv[1], NULL, 10) : 500000;
    srand((argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10) : 1);

    const uint16_t base = 0x1000;
    long fails = 0, declined = 0;

    for (long it = 0; it < iters; it++){
        int count = rand() % 9;
        size_t len = 0;
        int dr, sr, g, pp;
        for (int k = 0; k < count; k++){
            switch (rand() % 21){
                case 0:  g_img[base+len++]=0x00; break;                                  /* NOP       */
                case 1:  dr=SUP_REG[rand()%7]; sr=SUP_REG[rand()%7];
                         g_img[base+len++]=(uint8_t)(0x40|(dr<<3)|sr); break;             /* LD r,r'   */
                case 2:  dr=SUP_REG[rand()%7]; g_img[base+len++]=(uint8_t)(0x06|(dr<<3));
                         g_img[base+len++]=(uint8_t)rand(); break;                        /* LD r,n    */
                case 3:  dr=SUP_REG[rand()%7]; g_img[base+len++]=(uint8_t)(0x04|(dr<<3)); break; /* INC r */
                case 4:  dr=SUP_REG[rand()%7]; g_img[base+len++]=(uint8_t)(0x05|(dr<<3)); break; /* DEC r */
                case 5:  g=rand()%8; sr=SUP_REG[rand()%7];
                         g_img[base+len++]=(uint8_t)(0x80|(g<<3)|sr); break;              /* ALU A,r   */
                case 6:  g=rand()%8; g_img[base+len++]=(uint8_t)(0xC6|(g<<3));
                         g_img[base+len++]=(uint8_t)rand(); break;                        /* ALU A,n   */
                case 7:  { static const uint8_t R[4]={0x07,0x0F,0x17,0x1F};
                           g_img[base+len++]=R[rand()%4]; } break;                        /* rotates   */
                case 8:  dr=SUP_REG[rand()%7]; g_img[base+len++]=(uint8_t)(0x46|(dr<<3)); break; /* LD r,(HL) */
                case 9:  sr=SUP_REG[rand()%7]; g_img[base+len++]=(uint8_t)(0x70|sr); break;      /* LD (HL),r */
                case 10: g_img[base+len++]=0x36; g_img[base+len++]=(uint8_t)rand(); break;       /* LD (HL),n */
                case 11: g_img[base+len++]= (rand()&1)?0x0A:0x1A; break;                  /* LD A,(BC/DE) */
                case 12: g_img[base+len++]= (rand()&1)?0x02:0x12; break;                  /* LD (BC/DE),A */
                case 13: g_img[base+len++]=0x3A; g_img[base+len++]=(uint8_t)(rand()&0xFF);
                         g_img[base+len++]=0xD3; break;                                   /* LD A,(D3xx) */
                case 14: g_img[base+len++]=0x32; g_img[base+len++]=(uint8_t)(rand()&0xFF);
                         g_img[base+len++]=0xD3; break;                                   /* LD (D3xx),A */
                case 15: pp=rand()%3; g_img[base+len++]=(uint8_t)(0x01|(pp<<4));          /* LD BC/DE/HL,nn */
                         g_img[base+len++]=(uint8_t)(rand()&0xFF);
                         g_img[base+len++]=(uint8_t)(0xD0+pp); break;                     /* -> safe Dx xx  */
                case 16: pp=rand()%4; g_img[base+len++]=(uint8_t)(0x03|(pp<<4)); break;   /* INC rr    */
                case 17: pp=rand()%4; g_img[base+len++]=(uint8_t)(0x0B|(pp<<4)); break;   /* DEC rr    */
                case 18: pp=rand()%4; g_img[base+len++]=(uint8_t)(0x09|(pp<<4)); break;   /* ADD HL,rr */
                case 19: pp=rand()%4; g_img[base+len++]=(uint8_t)(0xC5|(pp<<4)); break;   /* PUSH rr   */
                case 20: pp=rand()%4; g_img[base+len++]=(uint8_t)(0xC1|(pp<<4)); break;   /* POP rr    */
            }
        }
        g_img[base+len++] = 0xC9;        /* RET */

        /* seed: pointer regs into safe RAM (HL=D0xx, BC=D1xx, DE=D2xx), sp=CFxx */
        Z80State seed; memset(&seed, 0, sizeof seed);
        seed.a=rand(); seed.f=rand();
        seed.b=0xD1; seed.c=rand();
        seed.d=0xD2; seed.e=rand();
        seed.h=0xD0; seed.l=rand();
        seed.ix=rand(); seed.iy=rand();
        seed.sp=0xCF00;

        memcpy(g_A, g_img, sizeof g_A);
        memcpy(g_B, g_img, sizeof g_B);

        /* shard */
        ShardFn fn = z80_sljit_compile(&g_img[base], len, base);
        if (!fn){ declined++; fprintf(stderr,"[diff] unexpected decline (len=%zu)\n", len); continue; }
        Bus busA = { bus_r8, bus_w8, bus_in, bus_out, g_A };
        Z80State st = seed; st.cyc = 0;
        fn(&st, &busA);

        /* oracle: superzazu over its own copy, count+1 instructions (no branches) */
        z80 z; z80_init(&z);
        z.read_byte=bus_r8; z.write_byte=bus_w8; z.port_in=sz_in; z.port_out=sz_out; z.userdata=g_B;
        z.a=seed.a; z.b=seed.b; z.c=seed.c; z.d=seed.d; z.e=seed.e; z.h=seed.h; z.l=seed.l;
        z.ix=seed.ix; z.iy=seed.iy; z.sp=seed.sp; z.pc=base; z.cyc=0;
        unpack_f(&z, seed.f);
        for (int s = 0; s < count + 1; s++) z80_step(&z);

        /* neutralize the code region in both copies (ignore any self-modify) */
        memset(g_A+base, 0, len); memset(g_B+base, 0, len);

        int bad = (st.a!=z.a)||(st.f!=pack_f(&z))||(st.b!=z.b)||(st.c!=z.c)||
                  (st.d!=z.d)||(st.e!=z.e)||(st.h!=z.h)||(st.l!=z.l)||
                  (st.ix!=z.ix)||(st.iy!=z.iy)||(st.sp!=z.sp)||
                  (st.cyc!=(uint64_t)z.cyc)||
                  (memcmp(g_A, g_B, sizeof g_A) != 0);
        if (bad){
            if (fails < 10){
                fprintf(stderr,"[diff] MISMATCH it=%ld count=%d:\n", it, count);
                fprintf(stderr,"  shard A=%02X F=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X cyc=%llu mem=%s\n",
                        st.a,st.f,st.b,st.c,st.d,st.e,st.h,st.l,st.sp,(unsigned long long)st.cyc,
                        memcmp(g_A,g_B,sizeof g_A)?"DIFF":"ok");
                fprintf(stderr,"  oracl A=%02X F=%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X cyc=%lu\n",
                        z.a,pack_f(&z),z.b,z.c,z.d,z.e,z.h,z.l,z.sp,z.cyc);
            }
            fails++;
        }
        sljit_free_code((void*)fn, NULL);
    }

    printf("z80_sljit_diff: %ld cases, %ld declined, %ld FAILS\n", iters, declined, fails);
    printf("%s\n", (fails==0 && declined==0) ? "PASS" : "FAIL");
    return (fails==0 && declined==0) ? 0 : 1;
}
