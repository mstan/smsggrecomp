#include "sms_vdp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static uint32_t rng=0x13791379;
static unsigned next(void){ rng=rng*1664525u+1013904223u; return rng; }
int main(void){
    static uint32_t a[256*192],b[256*192];
    static VdpFrameJob f;
    for (int test=0;test<512;test++){
        vdp_reset(test&1);
        for (int i=0;i<0x4000;i++) g_vdp.vram[i]=(uint8_t)(next()>>24);
        for (int i=0;i<64;i++) g_vdp.cram[i]=(uint8_t)(next()>>24);
        for (int i=0;i<16;i++) g_vdp.reg[i]=(uint8_t)(next()>>24);
        SmsVdp before=g_vdp;
        vdp_render_reference(a);
        vdp_prepare_frame(&f);
        memset(&g_vdp,0,sizeof g_vdp); /* jobs own their source data */
        vdp_render_jobs(&f,b);
        g_vdp=before;
        if (memcmp(a,b,sizeof a)){
            for(int i=0;i<256*192;i++) if(a[i]!=b[i]){
                fprintf(stderr,"case %d pixel %d,%d reference=%08x job=%08x\n",test,i%256,i/256,a[i],b[i]);break;
            }
            return 1;
        }
    }
    /* Each line owns the state that existed then, even if every source changes
     * before publication. The reference is independently sampled per line. */
    for(int test=0;test<16;test++){
        vdp_reset(test&1);vdp_enable_raster(true);
        for(int y=0;y<192;y++){
            for(int i=0;i<0x4000;i++)g_vdp.vram[i]=(uint8_t)(next()>>24);
            for(int i=0;i<64;i++)g_vdp.cram[i]=(uint8_t)(next()>>24);
            for(int i=0;i<16;i++)g_vdp.reg[i]=(uint8_t)(next()>>24);
            vdp_render_reference(b);memcpy(a+y*256,b+y*256,256*4);
            vdp_step_line();
        }
        memset(g_vdp.vram,0,sizeof g_vdp.vram);
        vdp_render_frame(b);
        if(memcmp(a,b,sizeof a)){fprintf(stderr,"raster capture case %d failed\n",test);return 1;}
    }
    puts("512 immutable frames and 16 changing raster frames match (SMS and GG)");
    return 0;
}
