/* Throwaway self-test for z80_decode. Compile:
 *   gcc -Wall _decoder_selftest.c z80_decoder.c -o dt && ./dt
 */
#include "z80_decoder.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void chk(const char *name, int got, int want) {
    if (got != want) { printf("FAIL %-22s got %d want %d\n", name, got, want); fails++; }
    else             { printf("ok   %-22s %d\n", name, got); }
}

#define DEC(pc, ...) do { uint8_t b[]={__VA_ARGS__}; z80_decode(b,sizeof(b),pc,&in); } while(0)

int main(void) {
    Z80Insn in;

    DEC(0,0x00);              chk("NOP.len",in.length,1);     chk("NOP.cf",in.cf,Z80_CF_NONE);
    DEC(0,0xC3,0x34,0x12);    chk("JPnn.len",in.length,3);    chk("JPnn.cf",in.cf,Z80_CF_JUMP);
                              chk("JPnn.tgt",in.target,0x1234);
    DEC(0,0xCD,0x00,0x80);    chk("CALL.len",in.length,3);    chk("CALL.cf",in.cf,Z80_CF_CALL);
                              chk("CALL.tgt",in.target,0x8000);
    DEC(0,0xC9);              chk("RET.len",in.length,1);     chk("RET.cf",in.cf,Z80_CF_RET);
    DEC(0x100,0x18,0xFE);     chk("JR.len",in.length,2);      chk("JR.cf",in.cf,Z80_CF_JUMP);
                              chk("JR.tgt",in.target,0x100);
    DEC(0x200,0x10,0x05);     chk("DJNZ.len",in.length,2);    chk("DJNZ.cf",in.cf,Z80_CF_JUMP_COND);
                              chk("DJNZ.tgt",in.target,0x207);
    DEC(0,0x21,0x00,0xC0);    chk("LDHLnn.len",in.length,3);  chk("LDHLnn.imm",in.imm,0xC000);
    DEC(0,0x06,0x42);         chk("LDBn.len",in.length,2);    chk("LDBn.imm",in.imm,0x42);
    DEC(0,0xCB,0x40);         chk("BIT.len",in.length,2);     chk("BIT.pfx",in.prefix,Z80_PFX_CB);
    DEC(0,0xED,0x4D);         chk("RETI.len",in.length,2);    chk("RETI.cf",in.cf,Z80_CF_RET);
    DEC(0,0xED,0xB0);         chk("LDIR.len",in.length,2);    chk("LDIR.cf",in.cf,Z80_CF_NONE);
    DEC(0,0xDD,0x21,0x00,0x40);chk("LDIXnn.len",in.length,4); chk("LDIXnn.imm",in.imm,0x4000);
    DEC(0,0xDD,0x7E,0x05);    chk("LDA(IX).len",in.length,3); chk("LDA(IX).disp",in.disp,5);
                              chk("LDA(IX).usesd",in.uses_disp,1);
    DEC(0,0xDD,0x36,0x05,0x99);chk("LD(IX)n.len",in.length,4);chk("LD(IX)n.disp",in.disp,5);
                              chk("LD(IX)n.imm",in.imm,0x99);
    DEC(0,0xDD,0xCB,0x05,0x46);chk("DDCB.len",in.length,4);   chk("DDCB.pfx",in.prefix,Z80_PFX_DDCB);
                              chk("DDCB.disp",in.disp,5);      chk("DDCB.op",in.opcode,0x46);
    DEC(0,0xE9);              chk("JPHL.len",in.length,1);    chk("JPHL.cf",in.cf,Z80_CF_JUMP);
                              chk("JPHL.notgt",in.has_target,0);
    DEC(0,0xC7);              chk("RST0.len",in.length,1);    chk("RST0.tgt",in.target,0x00);
    DEC(0,0xFF);              chk("RST38.len",in.length,1);   chk("RST38.tgt",in.target,0x38);
    DEC(0,0xC2,0x00,0x10);    chk("JPNZ.len",in.length,3);    chk("JPNZ.cf",in.cf,Z80_CF_JUMP_COND);
                              chk("JPNZ.tgt",in.target,0x1000);
    DEC(0,0xFD,0xE9);         chk("JPIY.len",in.length,2);    chk("JPIY.cf",in.cf,Z80_CF_JUMP);
    DEC(0,0xDB,0xBE);         chk("INAn.len",in.length,2);    chk("INAn.imm",in.imm,0xBE);
    DEC(0,0x76);              chk("HALT.len",in.length,1);    chk("HALT.halt",in.is_halt,1);
    DEC(0,0xDD,0x09);         chk("ADDIXBC.len",in.length,2);
    DEC(0,0xDD,0x26,0x10);    chk("LDIXHn.len",in.length,3);  chk("LDIXHn.disp?",in.uses_disp,0);
    DEC(0,0x32,0x00,0xC0);    chk("LD(nn)A.len",in.length,3); chk("LD(nn)A.imm",in.imm,0xC000);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
