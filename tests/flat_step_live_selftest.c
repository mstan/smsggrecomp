/* Native flat dispatch must retain mutable operands for each uploaded opcode
 * shape even when a bootstrap image has a different instruction at that PC. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef SMS_TEST_GENERATE
#include "code_generator.h"
int main(int argc, char **argv) {
    assert(argc == 2);
    uint8_t boot[16] = {0};
    uint8_t driver[16] = {0x3e,0,0xc6,0,0xc3,8,0};
    SmsRom original = {0}, uploaded = {0};
    original.data=boot;original.size=sizeof boot;
    uploaded.data=driver;uploaded.size=sizeof driver;
    GameConfig config={0};strcpy(config.output_prefix,"flatfixture");
    cg_emit_flat_step(&original,&config,argv[1],&uploaded,1);
    return 0;
}
#else
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "flatfixture_step.h"
Z80State g_z80;
static uint8_t memory[65536];
static unsigned misses;
uint8_t sms_read8(uint16_t address) {return memory[address];}
void sms_write8(uint16_t address,uint8_t value) {memory[address]=value;}
void sms_dispatch_miss(uint16_t address) {(void)address;++misses;}
int main(void) {
    g_z80=(Z80State){0};flatfixture_step();
    assert(g_z80.pc==1 && g_z80.cyc==4 && g_z80.r==1 && !misses);
    const uint8_t driver[]={0x3e,0x80,0xc6,0x40,0xc3,9,0};
    memcpy(memory,driver,sizeof driver);g_z80=(Z80State){0};
    flatfixture_step();assert(g_z80.a==0x80 && g_z80.pc==2 && g_z80.cyc==7 && g_z80.r==1);
    flatfixture_step();assert(g_z80.a==0xc0 && g_z80.pc==4 && g_z80.cyc==14 && g_z80.r==2 && g_z80.f==0x80);
    flatfixture_step();assert(g_z80.pc==9 && g_z80.cyc==24 && g_z80.r==3 && !misses);
    memory[0]=0xff;g_z80=(Z80State){0};flatfixture_step();
    assert(misses==1 && !g_z80.pc && !g_z80.cyc && !g_z80.r);
    puts("Mixed bootstrap/uploaded opcode shapes preserve live operands and guarded native timing");
    return 0;
}
#endif
