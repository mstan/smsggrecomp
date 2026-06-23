/*
 * z80_sljit_cf.c — control-flow differential harness for the Z80 -> sljit emitter.
 *
 * Generates random TERMINATING control-flow functions (register-only simple ops +
 * forward JR cc / JP cc, RET cc, and bounded DJNZ loops) and asserts the emitted
 * shard matches the superzazu interpreter (regs + flags + cyc) run to the function's
 * RET. Register-only bodies (no memory writes) so nothing self-modifies the stream.
 */
#include "jit_abi.h"
#include "z80_sljit.h"
#include "sljitLir.h"
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_mem[0x10000];
static uint8_t bus_r8(void *c, uint16_t a){ return ((uint8_t*)c)[a]; }
static void    bus_w8(void *c, uint16_t a, uint8_t v){ ((uint8_t*)c)[a]=v; }
static uint8_t bus_in(void *c, uint8_t p){ (void)c;(void)p; return 0xFF; }
static void    bus_out(void *c, uint8_t p, uint8_t v){ (void)c;(void)p;(void)v; }
static uint8_t sz_in (z80 *z, uint8_t p){ (void)z;(void)p; return 0xFF; }
static void    sz_out(z80 *z, uint8_t p, uint8_t v){ (void)z;(void)p;(void)v; }
static uint8_t pack_f(const z80 *z){ return (uint8_t)((z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|(z->xf<<3)|(z->pf<<2)|(z->nf<<1)|(z->cf)); }
static void unpack_f(z80 *z, uint8_t f){ z->sf=(f>>7)&1;z->zf=(f>>6)&1;z->yf=(f>>5)&1;z->hf=(f>>4)&1;z->xf=(f>>3)&1;z->pf=(f>>2)&1;z->nf=(f>>1)&1;z->cf=f&1; }

static const int RG[7] = {0,1,2,3,4,5,7};
/* one register-only simple op into buf; returns size */
static int gen_simple(uint8_t *buf){
    switch (rand()%7){
        case 0: buf[0]=0x00; return 1;                                   /* NOP */
        case 1: buf[0]=(uint8_t)(0x40|(RG[rand()%7]<<3)|RG[rand()%7]); return 1; /* LD r,r' */
        case 2: buf[0]=(uint8_t)(0x06|(RG[rand()%7]<<3)); buf[1]=(uint8_t)rand(); return 2; /* LD r,n */
        case 3: buf[0]=(uint8_t)(0x04|(RG[rand()%7]<<3)); return 1;       /* INC r */
        case 4: buf[0]=(uint8_t)(0x05|(RG[rand()%7]<<3)); return 1;       /* DEC r */
        case 5: buf[0]=(uint8_t)(0x80|((rand()%8)<<3)|RG[rand()%7]); return 1; /* ALU A,r */
        default:buf[0]=(uint8_t)(0xC6|((rand()%8)<<3)); buf[1]=(uint8_t)rand(); return 2; /* ALU A,n */
    }
}

#define MARK 0xBEEF

int main(int argc, char **argv){
    long iters = (argc>1)?strtol(argv[1],NULL,10):300000;
    srand((argc>2)?(unsigned)strtoul(argv[2],NULL,10):1);
    const uint16_t base = 0x1000;
    long fails=0, declined=0;

    for (long it=0; it<iters; it++){
        /* layout: items[] with sizes + (for branches) a forward target item index */
        int N = 1 + rand()%10;
        enum { OP, JRCC, JPCC, RETCC, DJNZ };
        int  typ[24]; int tgt[24]; uint8_t body[24][3]; int sz[24]; int off[25];
        for (int i=0;i<N;i++){
            int roll = rand()%10;
            if (i==0 && (rand()%4)==0){ typ[i]=DJNZ; sz[i]=2; tgt[i]=0; /* placeholder; resolved below */ }
            else if (roll<2 && i+1<N){ typ[i]=JRCC; sz[i]=2; tgt[i]=i+1+rand()%(N-i); }
            else if (roll<4 && i+1<N){ typ[i]=JPCC; sz[i]=3; tgt[i]=i+1+rand()%(N-i); }
            else if (roll<5){ typ[i]=RETCC; sz[i]=1; }
            else { typ[i]=OP; sz[i]=gen_simple(body[i]); }
        }
        off[0]=0; for (int i=0;i<N;i++) off[i+1]=off[i]+sz[i];
        int end = off[N];   /* the final RET position */
        /* DJNZ at item 0 loops back to item 1 (a bounded backward loop) */
        for (int i=0;i<N;i++) if (typ[i]==DJNZ){ tgt[i] = (i+1<N)?(i+1):N; }

        size_t L=0;
        for (int i=0;i<N;i++){
            uint8_t *p=&g_mem[base+off[i]];
            int t = (typ[i]==OP||typ[i]==RETCC)?0:((tgt[i]<N)?off[tgt[i]]:end);
            switch (typ[i]){
                case OP:    memcpy(p, body[i], sz[i]); break;
                case RETCC: p[0]=(uint8_t)(0xC0|((rand()%8)<<3)); break;
                case JRCC:  p[0]=(uint8_t)(0x20|((rand()%4)<<3)); p[1]=(uint8_t)(t-(off[i]+2)); break;
                case DJNZ:  p[0]=0x10; p[1]=(uint8_t)(t-(off[i]+2)); break;
                case JPCC:  p[0]=(uint8_t)(0xC2|((rand()%8)<<3));
                            p[1]=(uint8_t)((base+t)&0xFF); p[2]=(uint8_t)((base+t)>>8); break;
            }
            L = off[i]+sz[i];
        }
        g_mem[base+end]=0xC9; L=end+1;   /* RET */

        Z80State seed; memset(&seed,0,sizeof seed);
        seed.a=rand(); seed.f=rand(); seed.b=1+rand()%6; seed.c=rand(); seed.d=rand();
        seed.e=rand(); seed.h=rand(); seed.l=rand(); seed.sp=0xCF00;
        g_mem[0xCF00]=(uint8_t)(MARK&0xFF); g_mem[0xCF01]=(uint8_t)(MARK>>8);

        ShardFn fn = z80_sljit_compile(&g_mem[base], 0x10000-base, base);
        if (!fn){ declined++; fprintf(stderr,"[cf] decline: %s @%04X\n", z80_sljit_last_decline.why, z80_sljit_last_decline.pc); continue; }
        Bus bus={bus_r8,bus_w8,bus_in,bus_out,g_mem};
        Z80State st=seed; st.cyc=0; fn(&st,&bus);

        z80 z; z80_init(&z);
        z.read_byte=bus_r8; z.write_byte=bus_w8; z.port_in=sz_in; z.port_out=sz_out; z.userdata=g_mem;
        z.a=seed.a;z.b=seed.b;z.c=seed.c;z.d=seed.d;z.e=seed.e;z.h=seed.h;z.l=seed.l;
        z.sp=seed.sp;z.pc=base;z.cyc=0; unpack_f(&z,seed.f);
        long guard=0; while (z.pc != MARK && ++guard < 200000) z80_step(&z);

        int bad = (st.a!=z.a)||(st.f!=pack_f(&z))||(st.b!=z.b)||(st.c!=z.c)||(st.d!=z.d)||
                  (st.e!=z.e)||(st.h!=z.h)||(st.l!=z.l)||(st.sp!=z.sp)||(st.cyc!=(uint64_t)z.cyc);
        if (bad){
            if (fails<8){ fprintf(stderr,"[cf] MISMATCH it=%ld N=%d:\n  shard A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X cyc=%llu\n  oracl A=%02X F=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X SP=%04X cyc=%lu (guard=%ld pc=%04X)\n",
                it,N, st.a,st.f,st.b,st.c,st.d,st.e,st.h,st.l,st.sp,(unsigned long long)st.cyc,
                z.a,pack_f(&z),z.b,z.c,z.d,z.e,z.h,z.l,z.sp,z.cyc, guard, z.pc); }
            fails++;
        }
        sljit_free_code((void*)fn,NULL);
    }
    printf("z80_sljit_cf: %ld cases, %ld declined, %ld FAILS\n", iters, declined, fails);
    printf("%s\n", (fails==0)?"PASS":"FAIL");
    return fails==0?0:1;
}
