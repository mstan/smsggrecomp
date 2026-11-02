/* Two-stage regression: compile with SMS_TEST_GENERATE and the recompiler
 * sources to emit a synthetic ROM; then compile this file without that define
 * alongside bankfixture_full.c and bankfixture_dispatch.c and run it.
 * Executing an address in an uncompiled bank must use the miss path, even
 * when that address has exactly one compiled variant in another bank. */
#ifdef SMS_TEST_GENERATE
#include "code_generator.h"
#include <string.h>
int main(int argc,char **argv){
    if(argc!=2)return 2;
    static uint8_t bytes[4*0x4000];memset(bytes,0xc9,sizeof bytes);
    bytes[3*0x4000]=0x3e;bytes[3*0x4000+1]=0x33; /* LD A,$33; RET */
    const uint8_t caller[]={0x3e,3,0x32,0xff,0xff,0xcd,0,0x80,0xc9};
    memcpy(bytes+0x100,caller,sizeof caller);
    /* Register spans cross neither EI nor a branch destination. Include
     * alternate registers, flags, 16-bit arithmetic and an R wrap. */
    const uint8_t registers[]={0x01,0x34,0x12,0x11,0x78,0x56,0x21,0xbc,0x9a,
        0x3e,0x27,0x09,0x03,0x0b,0x04,0x05,0x07,0x0f,0x17,0x1f,0x27,0x2f,
        0x37,0x3f,0x41,0x48,0x57,0x5f,0x67,0x6f,0x80,0x89,0x92,0x9b,
        0xa4,0xad,0xb0,0xb9,0xc6,0x18,0xce,0x01,0xd6,0x09,0xde,0x02,
        0xe6,0x7f,0xee,0xa5,0xf6,0x20,0xfe,0x42,0x08,0xd9,0xeb,0xf9,
        0xfb,0x00,0x3c,0x3d,0x20,0x02,0x04,0x0c,0x14,0x1c,0xc9};
    memcpy(bytes+0x300,registers,sizeof registers);
    SmsRom rom={0};rom.data=bytes;rom.size=sizeof bytes;rom.num_banks=4;rom.mapper=SMS_MAPPER_SEGA;
    FuncList fl;funclist_init(&fl);funclist_add(&fl,0x8000,3,NULL,FUNC_SRC_CONFIG,true);
    funclist_add(&fl,0x100,-1,NULL,FUNC_SRC_CONFIG,true);
    funclist_add(&fl,0x300,-1,NULL,FUNC_SRC_CONFIG,true);
    GameConfig cfg={0};strcpy(cfg.output_prefix,"bankfixture");
    cg_emit(&rom,&fl,&cfg,argv[1]);funclist_free(&fl);return 0;
}
#else
#include "sms_runtime.h"
#include <stdio.h>
#include <stdlib.h>
Z80State g_z80;
uint64_t g_sync_deadline=UINT64_MAX,g_frame_ic,g_diff_icount;
int g_diff_freeze,g_diff_active;
uint16_t g_enter_ring[SMS_ENTER_RING_SIZE],g_dbg_pc;
uint32_t g_enter_pos;
static int bank,misses,forced_bank=-1;
int sms_slot_bank(uint16_t address){(void)address;return bank;}
void sms_write8(uint16_t address,uint8_t value){if(address==0xffff)bank=forced_bank<0?value:forced_bank;}
void sms_dispatch_miss(uint16_t address){if(address!=0x8000)abort();misses++;g_z80.a=0x22;g_z80.sp+=2;}
void sms_diff_enter(uint16_t address){(void)address;abort();}
void sms_diff_abort(void){abort();}
void sms_sync(void){abort();}
int main(void){
    bank=2;call_by_address(0x8000);
    if(misses!=1||g_z80.a!=0x22){fputs("wrong-bank compiled code executed\n",stderr);return 1;}
    bank=3;g_z80.sp=0xfff0;call_by_address(0x8000);
    if(misses!=1||g_z80.a!=0x33||g_z80.sp!=0xfff2)return 1;
    /* The static caller selected bank3, but the live mapping differs. This
     * also checks the guarded native-call profile, not only the dispatcher. */
    forced_bank=2;g_z80.sp=0xfff0;call_by_address(0x100);
    if(misses!=2||g_z80.a!=0x22||g_z80.sp!=0xfff2)return 1;
    forced_bank=3;g_z80.sp=0xfff0;call_by_address(0x100);
    if(misses!=2||g_z80.a!=0x33||g_z80.sp!=0xfff2)return 1;
    puts("generated dispatch and direct calls respect the live physical bank");return 0;
}
#endif
