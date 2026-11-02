#include "sms_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef SMS_NATIVE_HOOKS
#error Compile this comparison with SMS_NATIVE_HOOKS
#endif
Z80State g_z80;
uint64_t g_sync_deadline,g_frame_ic,g_diff_icount;
int g_diff_freeze,g_diff_active;
uint16_t g_dbg_pc,g_enter_ring[SMS_ENTER_RING_SIZE];
uint32_t g_enter_pos;
typedef struct { uint64_t cyc;uint16_t pc;uint8_t ei,r; } Event;
static Event events[2][20000];static unsigned counts[2],arm;
void sms_sync(void){
    unsigned n=counts[arm]++;if(n>=20000)abort();
    events[arm][n]=(Event){g_z80.cyc,g_dbg_pc,g_z80.ei_block,g_z80.r};
    g_sync_deadline=g_z80.cyc+((n%7)==0?1:228);
    if(n%5==0){g_z80.cyc+=13;g_dbg_pc=0x38;}
}
void sms_diff_enter(uint16_t a){(void)a;abort();}
void sms_diff_abort(void){abort();}
static void run(unsigned which){
    arm=which;memset(&g_z80,0,sizeof g_z80);g_sync_deadline=0;g_frame_ic=0;
    g_enter_pos=0;memset(g_enter_ring,0,sizeof g_enter_ring);
    uint32_t random=0x517317;unsigned observed=0;
    for(unsigned i=0;i<100000;i++){
        random=random*1664525+1013904223;
        uint16_t pc=(uint16_t)(0x100+(i%30000));uint8_t cost=(uint8_t)(4+(random%20));
        bool post=i%11==0,control=post||i%13==0;
        if(which==0)g_dbg_pc=pc;
        else if(control)SMS_PC_CONTROL(pc);else SMS_PC(pc);
        g_z80.r=(uint8_t)((g_z80.r&128)|((g_z80.r+1)&127));
        g_z80.ei_block=(i%17)==0;
        if(which==0){if(post)sms_tick_post_reference(cost);else sms_tick_reference(cost);}
        else {if(post)sms_tick_post(cost);else sms_tick(cost);}
        if(control && g_dbg_pc!=pc && !(g_z80.cyc>=g_sync_deadline)){
            /* IRQ-like mock may overwrite the PC during the sync itself. */
            observed++;
        }
        if(i%19==0){if(which==0)sms_enter_reference(pc);else sms_enter(pc);}
    }
    (void)observed;
}
int main(void){
#ifndef SMS_GENERATED_NATIVE_PC
    /* A legacy generated block instruction increments time before syncing. */
    g_z80.cyc=100;g_sync_deadline=105;g_dbg_pc=0;SMS_PC(0x1234);sms_tick_post(16);
    if(counts[0]!=1 || events[0][0].pc!=0x1234)return 1;
    memset(counts,0,sizeof counts);memset(events,0,sizeof events);
#endif
    run(0);Z80State expected=g_z80;uint16_t ring[SMS_ENTER_RING_SIZE];memcpy(ring,g_enter_ring,sizeof ring);uint32_t pos=g_enter_pos;
    run(1);
    if(memcmp(&expected,&g_z80,sizeof expected)||counts[0]!=counts[1]||
       memcmp(events[0],events[1],counts[0]*sizeof(Event))||pos!=g_enter_pos||memcmp(ring,g_enter_ring,sizeof ring)){
        fputs("native boundary/PC/EI/event contract differs\n",stderr);return 1;
    }
    puts("100000 native/reference boundaries: exact guest state, sync PCs, EI and entry ring");return 0;
}
