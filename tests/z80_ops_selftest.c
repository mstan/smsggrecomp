/* Sanity test for z80_ops.h flag semantics. Build via PowerShell:
 *   gcc -I runner/include tests/z80_ops_selftest.c -o ot.exe */
#include "z80_ops.h"
#include <stdio.h>
/* satisfy sms_runtime.h externs (unused here) */
Z80State g_z80;
uint8_t sms_read8(uint16_t a){(void)a;return 0;}
void sms_write8(uint16_t a,uint8_t v){(void)a;(void)v;}
uint8_t sms_io_in(uint8_t p){(void)p;return 0;}
void sms_io_out(uint8_t p,uint8_t v){(void)p;(void)v;}
void call_by_address(uint16_t a){(void)a;}

static int fails=0;
static void chk(const char*n,unsigned got,unsigned want){
    if(got!=want){printf("FAIL %-16s got 0x%02X want 0x%02X\n",n,got,want);fails++;}
    else printf("ok   %-16s 0x%02X\n",n,got);
}
int main(void){
    Z80State s; s.f=0;
    /* 0x0F + 0x01 = 0x10, half-carry set, no carry */
    uint8_t r=z80_add8(&s,0x0F,0x01,0); chk("add.r",r,0x10); chk("add.H",z80f_get(&s,Z80_FLAG_H),1);
    chk("add.C",z80f_c(&s),0);
    /* 0xFF + 0x01 = 0x00, carry + zero + half */
    r=z80_add8(&s,0xFF,0x01,0); chk("add2.r",r,0x00); chk("add2.Z",z80f_get(&s,Z80_FLAG_Z),1);
    chk("add2.C",z80f_c(&s),1);
    /* 0x7F + 0x01 = 0x80, overflow (P) set, sign set */
    r=z80_add8(&s,0x7F,0x01,0); chk("add3.r",r,0x80); chk("add3.P",z80f_get(&s,Z80_FLAG_P),1);
    chk("add3.S",z80f_get(&s,Z80_FLAG_S),1);
    /* 0x00 - 0x01 = 0xFF, borrow: carry+half set, N set */
    r=z80_sub8(&s,0x00,0x01,0); chk("sub.r",r,0xFF); chk("sub.C",z80f_c(&s),1);
    chk("sub.N",z80f_get(&s,Z80_FLAG_N),1); chk("sub.H",z80f_get(&s,Z80_FLAG_H),1);
    /* AND 0x0F & 0xF0 = 0, H=1, Z=1, P(parity of 0)=1 */
    r=z80_and8(&s,0x0F,0xF0); chk("and.r",r,0x00); chk("and.H",z80f_get(&s,Z80_FLAG_H),1);
    chk("and.Z",z80f_get(&s,Z80_FLAG_Z),1);
    /* DAA: A=0x0A after add (N=0,H=0) -> 0x10 */
    s.a=0x0A; s.f=0; z80_daa(&s); chk("daa.a",s.a,0x10);
    /* RLC 0x80 -> 0x01, carry out */
    r=z80_rlc(&s,0x80); chk("rlc.r",r,0x01); chk("rlc.C",z80f_c(&s),1);
    /* SRL 0x01 -> 0x00, carry out, zero */
    r=z80_srl(&s,0x01); chk("srl.r",r,0x00); chk("srl.C",z80f_c(&s),1); chk("srl.Z",z80f_get(&s,Z80_FLAG_Z),1);
    /* 16-bit ADD HL: 0x0FFF + 0x0001 = 0x1000, H set, C clear */
    uint16_t w=z80_add16(&s,0x0FFF,0x0001); chk("add16.r",w,0x1000); chk("add16.H",z80f_get(&s,Z80_FLAG_H),1);
    printf("\n%s (%d failures)\n", fails?"FAILED":"ALL PASS", fails);
    return fails?1:0;
}
