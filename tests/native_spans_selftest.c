/* Generate bankfixture using generator_bank_selftest.c, then compile this
 * with SMS_NATIVE_HOOKS and the generated include directory. Both versions
 * use identical instruction hooks; only register-span admission differs. */
#define SMS_GENERATED_NATIVE_PC 1
#include "sms_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define func_8000_b3 reference_8000
#define func_0100 reference_0100
#define func_0300 reference_0300
#undef SMS_NATIVE_SPANS
#include "bankfixture_full.c"
#undef func_8000_b3
#undef func_0100
#undef func_0300
#define SMS_NATIVE_SPANS 1
#include "bankfixture_full.c"
#include "bankfixture_dispatch.c"

Z80State g_z80;
uint64_t g_sync_deadline,g_frame_ic,g_diff_icount;
int g_diff_freeze,g_diff_active;
uint16_t g_dbg_pc,g_enter_ring[SMS_ENTER_RING_SIZE];
uint32_t g_enter_pos;
typedef struct { Z80State state;uint16_t pc; } Event;
static Event events[2][256];static unsigned count[2],arm;
int sms_slot_bank(uint16_t a){(void)a;return 3;}
void sms_write8(uint16_t a,uint8_t v){(void)a;(void)v;}
void sms_dispatch_miss(uint16_t a){(void)a;abort();}
void sms_diff_enter(uint16_t a){(void)a;abort();}
void sms_diff_abort(void){abort();}
void sms_sync(void){
    unsigned n=count[arm]++;if(n>=256)abort();
    events[arm][n].state=g_z80;events[arm][n].pc=g_dbg_pc;
    /* Include instruction-sized pending IRQ deadlines and interrupt work. */
    g_sync_deadline=g_z80.cyc+(n%3?1:31);
    if(n%5==0)g_z80.cyc+=13;
}
int main(void){
    uint32_t rng=0x341ab59;
    for(unsigned trial=0;trial<4096;trial++){
        Z80State initial;
        for(size_t i=0;i<sizeof initial;i++){
            rng=rng*1664525+1013904223;((uint8_t *)&initial)[i]=(uint8_t)(rng>>24);
        }
        initial.cyc=100;initial.ei_block=trial&1;
        initial.iff1=(trial>>1)&1;initial.iff2=(trial>>2)&1;initial.halted=0;
        initial.r=(trial&2)?0xff:0x7d;
        uint64_t deadline=trial==4095?UINT64_MAX:initial.cyc+(trial%512);
        memset(events,0,sizeof events);memset(count,0,sizeof count);
        Z80State expected={0};uint64_t expected_deadline=0;
        for(arm=0;arm<2;arm++){
            g_z80=initial;g_sync_deadline=deadline;g_dbg_pc=0x300;
            if(arm==0){reference_0300();expected=g_z80;expected_deadline=g_sync_deadline;}
            else func_0300();
        }
        if(memcmp(&expected,&g_z80,sizeof expected)||expected_deadline!=g_sync_deadline||
           count[0]!=count[1]||memcmp(events[0],events[1],sizeof events[0])){
            fprintf(stderr,"register-span state/event difference at trial %u\n",trial);return 1;
        }
    }
    puts("4096 register-span comparisons: exact state, refresh, EI and event boundaries");return 0;
}
