#define SDL_MAIN_HANDLED
#include "host_gl.h"
#include <stdlib.h>
#include <string.h>
static uint32_t random_state=7919;
static uint32_t random_word(void){random_state=random_state*1664525+1013904223;return random_state;}
int main(void){
    SDL_SetMainReady();if(SDL_Init(SDL_INIT_VIDEO))return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window *w=SDL_CreateWindow("GPU job test",0,0,320,240,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
    if(!w){fprintf(stderr,"%s\n",SDL_GetError());return 2;}
    static VdpFrameJob job;static uint32_t native[256*192];
    int scales[]={1,4,16};
    for(int gg=0;gg<2;gg++)for(int scale_id=0;scale_id<3;scale_id++){
        int s=scales[scale_id],cw=gg?160:256,ch=gg?144:192,cx=gg?48:0,cy=gg?24:0;
        if(!host_gl_init(w,cx,cy,cw,ch,s))return 2;
        size_t count=(size_t)cw*ch*s*s;uint32_t *gpu=malloc(count*4);if(!gpu)return 2;
        for(int test=0;test<8;test++){
            vdp_reset(gg);
            for(int i=0;i<16384;i++)g_vdp.vram[i]=(uint8_t)(random_word()>>24);
            for(int i=0;i<64;i++)g_vdp.cram[i]=(uint8_t)(random_word()>>24);
            for(int i=0;i<16;i++)g_vdp.reg[i]=(uint8_t)(random_word()>>24);
            if(test)g_vdp.reg[1]|=0x40;
            vdp_prepare_frame(&job);vdp_render_reference(native);
            /* Mutating sources after preparation cannot affect the GPU. */
            memset(g_vdp.vram,0,sizeof g_vdp.vram);
            if(test==3)SDL_SetWindowSize(w,711,377);
            host_gl_present(&job);int width,height;
            if(!host_gl_readback(gpu,count,&width,&height)||width!=cw*s||height!=ch*s)return 1;
            for(int y=0;y<height;y++)for(int x=0;x<width;x++){
                uint32_t expected=native[(cy+y/s)*256+cx+x/s];
                if(gpu[(size_t)y*width+x]!=expected){fprintf(stderr,"GG%d %dx case%d pixel%d,%d expected%08x actual%08x\n",gg,s,test,x,y,expected,gpu[(size_t)y*width+x]);return 1;}
            }
            host_gl_present(&job); /* identical resident frame uploads nothing */
        }
        host_gl_report(stdout);host_gl_report(stdout); /* reporting preserves raw pass pairings */
        free(gpu);host_gl_shutdown();
    }
    SDL_DestroyWindow(w);SDL_Quit();puts("48 GPU cases: exact pixels, frozen ownership, resize, reset and recreation pass");return 0;
}
