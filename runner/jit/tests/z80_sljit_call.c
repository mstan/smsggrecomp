/*
 * z80_sljit_call.c — focused differential test of the CALL emit path.
 *
 * main: [reg ops] CALL sub [reg ops] RET ; sub: [reg ops] RET.
 * bus->call runs the callee under superzazu on the same memory (mirrors the
 * off-thread validation handoff). Asserts the shard matches superzazu (regs +
 * flags + cyc) run to the main RET. Register-only bodies (no self-modify).
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
static void to_z(z80 *z, const Z80State *s){
    z->a=s->a;z->b=s->b;z->c=s->c;z->d=s->d;z->e=s->e;z->h=s->h;z->l=s->l; unpack_f(z,s->f);
    z->ix=s->ix;z->iy=s->iy;z->sp=s->sp;z->i=s->i;z->r=s->r;z->iff1=s->iff1;z->iff2=s->iff2;z->interrupt_mode=s->im;z->halted=s->halted; z->mem_ptr=s->wz;
}
static void from_z(Z80State *s, const z80 *z){
    s->a=z->a;s->b=z->b;s->c=z->c;s->d=z->d;s->e=z->e;s->h=z->h;s->l=z->l; s->f=pack_f(z);
    s->ix=z->ix;s->iy=z->iy;s->sp=z->sp;s->i=z->i;s->r=z->r;s->iff1=z->iff1;s->iff2=z->iff2;s->im=z->interrupt_mode;s->halted=z->halted; s->wz=z->mem_ptr;
}
static const uint64_t g_dl=(uint64_t)-1; static void h_sync(Z80State *s){ (void)s; }
/* bus->call: run the callee under superzazu on g_mem until it RETs */
static void sb_call(void *ctx, Z80State *s, uint16_t target){
    z80 z; z80_init(&z);
    z.read_byte=bus_r8; z.write_byte=bus_w8; z.port_in=sz_in; z.port_out=sz_out; z.userdata=ctx;
    to_z(&z, s); z.pc=target; z.cyc=0;
    uint16_t esp=s->sp; long g=0; while (!(z.sp>esp) && ++g<100000) z80_step(&z);
    from_z(s, &z); s->cyc += z.cyc;
}

static const int RG[7]={0,1,2,3,4,5,7};
static int gen_simple(uint8_t *buf){
    switch (rand()%6){
        case 0: buf[0]=0x00; return 1;
        case 1: buf[0]=(uint8_t)(0x40|(RG[rand()%7]<<3)|RG[rand()%7]); return 1;
        case 2: buf[0]=(uint8_t)(0x06|(RG[rand()%7]<<3)); buf[1]=(uint8_t)rand(); return 2;
        case 3: buf[0]=(uint8_t)(0x04|(RG[rand()%7]<<3)); return 1;
        case 4: buf[0]=(uint8_t)(0x80|((rand()%8)<<3)|RG[rand()%7]); return 1;
        default:buf[0]=(uint8_t)(0xC6|((rand()%8)<<3)); buf[1]=(uint8_t)rand(); return 2;
    }
}

int main(int argc, char **argv){
    long iters=(argc>1)?strtol(argv[1],NULL,10):200000;
    srand((argc>2)?(unsigned)strtoul(argv[2],NULL,10):1);
    const uint16_t main_a=0x1000, sub_a=0x1800;
    long fails=0;

    for (long it=0; it<iters; it++){
        /* sub: k ops + RET at sub_a */
        size_t sl=0; int ks=rand()%5; for (int i=0;i<ks;i++) sl+=gen_simple(&g_mem[sub_a+sl]); g_mem[sub_a+sl++]=0xC9;
        /* main: k ops, CALL sub, k ops, RET at main_a */
        size_t ml=0; int k1=rand()%4; for (int i=0;i<k1;i++) ml+=gen_simple(&g_mem[main_a+ml]);
        g_mem[main_a+ml++]=0xCD; g_mem[main_a+ml++]=(uint8_t)(sub_a&0xFF); g_mem[main_a+ml++]=(uint8_t)(sub_a>>8);
        int k2=rand()%4; for (int i=0;i<k2;i++) ml+=gen_simple(&g_mem[main_a+ml]); g_mem[main_a+ml++]=0xC9;

        Z80State seed; memset(&seed,0,sizeof seed);
        seed.a=rand();seed.f=rand();seed.b=rand();seed.c=rand();seed.d=rand();seed.e=rand();seed.h=rand();seed.l=rand();
        seed.sp=0xCF00; g_mem[0xCF00]=0xEF; g_mem[0xCF01]=0xBE;   /* return marker 0xBEEF for the oracle */

        ShardFn fn=z80_sljit_compile(&g_mem[main_a], 0x10000-main_a, main_a);
        if (!fn){ fprintf(stderr,"[call] decline: %s\n", z80_sljit_last_decline.why); fails++; continue; }
        Bus bus={bus_r8,bus_w8,bus_in,bus_out,sb_call,&g_dl,h_sync,g_mem};
        Z80State st=seed; st.cyc=0; fn(&st,&bus);

        z80 z; z80_init(&z);
        z.read_byte=bus_r8;z.write_byte=bus_w8;z.port_in=sz_in;z.port_out=sz_out;z.userdata=g_mem;
        to_z(&z,&seed); z.pc=main_a; z.cyc=0;
        long g=0; while (z.pc!=0xBEEF && ++g<200000) z80_step(&z);

        int bad=(st.a!=z.a)||(st.f!=pack_f(&z))||(st.b!=z.b)||(st.c!=z.c)||(st.d!=z.d)||(st.e!=z.e)||(st.h!=z.h)||(st.l!=z.l)||(st.sp!=z.sp)||(st.cyc!=(uint64_t)z.cyc);
        if (bad){ if (fails<8) fprintf(stderr,"[call] MISMATCH it=%ld:\n shard A=%02X F=%02X BC=%02X%02X HL=%02X%02X SP=%04X cyc=%llu\n oracl A=%02X F=%02X BC=%02X%02X HL=%02X%02X SP=%04X cyc=%lu\n",
            it, st.a,st.f,st.b,st.c,st.h,st.l,st.sp,(unsigned long long)st.cyc, z.a,pack_f(&z),z.b,z.c,z.h,z.l,z.sp,z.cyc); fails++; }
        sljit_free_code((void*)fn,NULL);
    }
    printf("z80_sljit_call: %ld cases, %ld FAILS\n%s\n", iters, fails, fails?"FAIL":"PASS");
    return fails?1:0;
}
