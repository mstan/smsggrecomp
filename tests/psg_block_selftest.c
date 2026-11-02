/* Build this file once with -DPSG_BUILD_REFERENCE -c, then link that object
 * with this file normally and runner/audio/sn76489.c. Only the selftest links
 * both implementations; production chooses SMS_PSG_REFERENCE at build time. */
#ifdef PSG_BUILD_REFERENCE
#define SMS_PSG_REFERENCE
#define psg_init reference_psg_init
#define psg_advance reference_psg_advance
#define psg_write reference_psg_write
#define psg_write_stereo reference_psg_write_stereo
#define psg_render reference_psg_render
#define psg_frames_available reference_psg_frames_available
#define psg_drain reference_psg_drain
#define psg_sample_rate reference_psg_sample_rate
#define psg_reset_leftover reference_psg_reset_leftover
#define psg_dump_state_text reference_psg_dump_state_text
#define psg_save_state reference_psg_save_state
#define psg_load_state reference_psg_load_state
#include "../runner/audio/sn76489.c"
#else
#include "sn76489.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
void reference_psg_init(void);
void reference_psg_advance(uint32_t);
void reference_psg_write(uint8_t);
void reference_psg_write_stereo(uint8_t);
size_t reference_psg_render(int16_t *,size_t);
size_t reference_psg_frames_available(void);
void reference_psg_reset_leftover(void);
int reference_psg_save_state(FILE *);
int reference_psg_load_state(FILE *);
static uint32_t random_state=0x73217649;
static unsigned random_word(void){random_state=random_state*1664525u+1013904223u;return random_state;}
static FILE *state_a,*state_b;
static size_t state_size;
static unsigned comparisons;
static int compare_state(void)
{
    unsigned char a[256],b[256];
    rewind(state_a);rewind(state_b);
    if(!psg_save_state(state_a)||!reference_psg_save_state(state_b))return 0;
    long n=ftell(state_a),m=ftell(state_b);
    if(n<0 || n>(long)sizeof a || n!=m)return 0;
    rewind(state_a);rewind(state_b);
    if(fread(a,1,(size_t)n,state_a)!=(size_t)n || fread(b,1,(size_t)n,state_b)!=(size_t)n)return 0;
    state_size=(size_t)n;comparisons++;
    if(memcmp(a,b,state_size)){fprintf(stderr,"state mismatch at comparison %u\n",comparisons);return 0;}
    return psg_frames_available()==reference_psg_frames_available();
}
static int compare_drain(size_t requested)
{
    int16_t a[8192*2],b[8192*2];
    size_t n=psg_render(a,requested),m=reference_psg_render(b,requested);
    if(n!=m || memcmp(a,b,n*2*sizeof *a)){fprintf(stderr,"PCM mismatch at comparison %u\n",comparisons);return 0;}
    return compare_state();
}
static void both_write(uint8_t v){psg_write(v);reference_psg_write(v);}
static void both_stereo(uint8_t v){psg_write_stereo(v);reference_psg_write_stereo(v);}
static int advance(uint32_t cycles){psg_advance(cycles);reference_psg_advance(cycles);return compare_state();}
static int load_crossed(void)
{
    /* Cross-load the other build's snapshot; host mix caches are not stored. */
    rewind(state_a);rewind(state_b);
    if(!psg_load_state(state_b)||!reference_psg_load_state(state_a))return 0;
    return compare_state();
}
static int directed_cases(void)
{
    const unsigned periods[]={0,1,2,3,15,16,17,1023};
    const unsigned pans[]={0,0xff,0xaa,0x55,0x0f,0xf0};
    for(unsigned period=0;period<8;period++)for(unsigned noise=0;noise<8;noise++)for(unsigned pan=0;pan<6;pan++){
        psg_init();reference_psg_init();
        for(unsigned channel=0;channel<3;channel++){
            unsigned n=periods[(period+channel)%8];
            both_write((uint8_t)(0x80|(channel<<5)|(n&15)));both_write((uint8_t)(n>>4));
            both_write((uint8_t)(0x90|(channel<<5)|(channel*3)));
        }
        both_write((uint8_t)(0xe0|noise));both_write(0xf4);both_stereo((uint8_t)pans[pan]);
        if(!advance(1)||!advance(14)||!advance(16*8192+1)||!compare_drain(37))return 0;
        /* Scratch remains full after a partial drain: overflow drops sample
         * production rather than silently recycling the freed prefix. */
        if(!advance(16*8192+11)||!compare_drain(8192)||!load_crossed())return 0;
        both_write(0xff);both_stereo(0);
        if(!advance(16*4096+7)||!compare_drain(4096))return 0;
        both_stereo(0xff);both_write(0xf0);
        if(!advance(16*4096+15)||!compare_drain(8192))return 0;
    }
    return 1;
}
static int random_cases(void)
{
    for(unsigned seed=0;seed<8;seed++){
        psg_init();reference_psg_init();
        for(unsigned step=0;step<4000;step++){
            unsigned value=random_word(),op=(value>>27)&7;
            switch(op){
            case 0:case 1:both_write((uint8_t)(value>>13));break;
            case 2:both_stereo((uint8_t)(value>>17));break;
            case 3:if(!advance(value&4095))return 0;break;
            case 4:if(!compare_drain((value>>8)&511))return 0;break;
            case 5:if(!compare_state()||!load_crossed())return 0;break;
            case 6:if(!advance(131072+(value&65535)))return 0;break;
            default:psg_reset_leftover();reference_psg_reset_leftover();break;
            }
            if(!compare_state())return 0;
        }
        if(!compare_drain(8192))return 0;
    }
    return 1;
}
static int invalid_load_cases(void)
{
    unsigned char saved[256],invalid[256],after[256];
    if(!compare_state())return 0;
    rewind(state_a);
    if(fread(saved,1,state_size,state_a)!=state_size)return 0;
    FILE *bad=tmpfile();if(!bad)return 0;
    fwrite(saved,1,state_size-1,bad);rewind(bad);
    if(psg_load_state(bad)){fclose(bad);return 0;}
    rewind(state_a);if(!psg_save_state(state_a)){fclose(bad);return 0;}
    rewind(state_a);if(fread(after,1,state_size,state_a)!=state_size){fclose(bad);return 0;}
    if(memcmp(saved,after,state_size)){fclose(bad);return 0;}
    memset(invalid,0xff,state_size);rewind(bad);fwrite(invalid,1,state_size,bad);rewind(bad);
    if(psg_load_state(bad)){fclose(bad);return 0;}
    rewind(state_a);if(!psg_save_state(state_a)){fclose(bad);return 0;}
    rewind(state_a);if(fread(after,1,state_size,state_a)!=state_size){fclose(bad);return 0;}
    fclose(bad);
    return !memcmp(saved,after,state_size);
}
typedef struct {
    void (*init)(void);void (*write)(uint8_t);void (*stereo)(uint8_t);
    void (*advance)(uint32_t);size_t (*render)(int16_t *,size_t);
} BenchApi;
static const BenchApi native_api={psg_init,psg_write,psg_write_stereo,psg_advance,psg_render};
static const BenchApi reference_api={reference_psg_init,reference_psg_write,reference_psg_write_stereo,reference_psg_advance,reference_psg_render};
static double benchmark(const BenchApi *api,unsigned scenario,unsigned frames)
{
    static int16_t out[4096*2];api->init();
    if(scenario){
        for(unsigned channel=0;channel<3;channel++){
            unsigned period=scenario==2?1:113+channel*137;
            api->write((uint8_t)(0x80|(channel<<5)|(period&15)));api->write((uint8_t)(period>>4));
            api->write((uint8_t)(0x90|(channel<<5)|channel));
        }
        api->write(0xe4);api->write(0xf3);
        api->stereo(scenario==3?0x96:0xff);
    }
    clock_t start=clock();
    for(unsigned done=0;done<frames;){
        unsigned chunk=frames-done;if(chunk>4096)chunk=4096;
        if(scenario==4){api->write((uint8_t)(0x90|((done>>12)&15)));api->stereo((uint8_t)(done>>10));}
        api->advance(chunk*16);api->render(out,chunk);done+=chunk;
    }
    return (double)(clock()-start)*1000/CLOCKS_PER_SEC;
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"--bench")){
        for(unsigned scenario=0;scenario<5;scenario++){
            benchmark(&reference_api,scenario,65536);benchmark(&native_api,scenario,65536);
            for(unsigned pair=0;pair<3;pair++){
                double a=benchmark(&reference_api,scenario,4000000),b=benchmark(&native_api,scenario,4000000);
                printf("scenario=%u pair=%u reference_ms=%.3f native_ms=%.3f\n",scenario,pair,a,b);
            }
        }
        return 0;
    }
    state_a=tmpfile();state_b=tmpfile();if(!state_a||!state_b)return 2;
    if(!directed_cases()||!random_cases()||!invalid_load_cases())return 1;
    fclose(state_a);fclose(state_b);
    printf("PSG native/reference exact: 384 directed cases, 32000 random operations, %u full-state/PCM comparisons; overflow, partial drain, stereo, reset, crossed load and invalid load pass\n",comparisons);
    return 0;
}
#endif
