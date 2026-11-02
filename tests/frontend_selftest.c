/* End-to-end front-end test: synthetic ROM -> decoder -> function_finder.
 * Build via PowerShell:
 *   gcc -I recompiler/src tests/frontend_selftest.c \
 *       recompiler/src/function_finder.c recompiler/src/z80_decoder.c \
 *       recompiler/src/rom_parser.c -o ft.exe
 */
#include "function_finder.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
static void want_entry(FuncList *fl, uint16_t a){
    int i=funclist_find(fl,a,-1);
    if (i<0 || !fl->items[i].is_entry){ printf("FAIL missing entry 0x%04X\n",a); fails++; }
    else printf("ok   entry 0x%04X (%s)\n",a,fl->items[i].name);
}

int main(void){
    static uint8_t rom[0x8000];
    memset(rom,0x00,sizeof(rom)); /* NOP fill */
    /* reset @0x0000: CALL 0x0100 ; CALL 0x0200 ; JR $ (loop) */
    uint8_t reset[] = {0xCD,0x00,0x01, 0xCD,0x00,0x02, 0x18,0xFE};
    memcpy(rom+0x0000, reset, sizeof(reset));
    /* func @0x0100: LD A,5 ; RET */
    uint8_t f1[] = {0x3E,0x05, 0xC9};            memcpy(rom+0x0100,f1,sizeof(f1));
    /* func @0x0200: CALL 0x0300 ; RET */
    uint8_t f2[] = {0xCD,0x00,0x03, 0xC9};       memcpy(rom+0x0200,f2,sizeof(f2));
    /* func @0x0300: XOR A ; RET */
    uint8_t f3[] = {0xAF, 0xC9};                 memcpy(rom+0x0300,f3,sizeof(f3));

    SmsRom r; memset(&r,0,sizeof(r));
    r.data=rom; r.size=sizeof(rom); r.num_banks=2; r.mapper=SMS_MAPPER_NONE;

    FuncList fl; funclist_init(&fl);
    ff_seed_vectors(&fl);
    ff_discover(&r,&fl,NULL,0);

    printf("discovered %d entries\n", fl.count);
    want_entry(&fl,0x0000);
    want_entry(&fl,0x0100);
    want_entry(&fl,0x0200);
    want_entry(&fl,0x0300);

    /* verify the internal JR-loop label inside reset is recognized */
    uint16_t entries[256]; int n=0;
    for (int i=0;i<fl.count;i++) entries[n++]=fl.items[i].addr;
    TraceResult tr; trace_function(&r,0x0000,-1,entries,n,&tr);
    if (trace_is_label(&tr,0x0006)) printf("ok   internal label 0x0006 in reset\n");
    else { printf("FAIL internal label 0x0006 missing\n"); fails++; }
    trace_free(&tr);

    /* ROM-proven dense object-state jump table, indexed through IX or IY. */
    for(int which=0;which<2;which++){
        const uint8_t dispatch[]={0xdd,0x7e,1,0x87,0x21,0x10,4,0x16,0,0x5f,0x19,0x7e,0x23,0x66,0x6f,0xe9,
                                  0x16,4,0x17,4,0x18,4,0xc9,0xc9,0xc9};
        memcpy(rom+0x400,dispatch,sizeof dispatch);rom[0x400]=which?0xfd:0xdd;
        FuncList tables;funclist_init(&tables);
        funclist_add(&tables,0x400,-1,NULL,FUNC_SRC_CONFIG,true);
        ff_discover(&r,&tables,NULL,0);
        for(int target=0x416;target<=0x418;target++){
            int found=0;
            for(int i=0;i<tables.count;i++)if(tables.items[i].addr==target)found=1;
            if(!found){printf("FAIL indexed table %d target %04x\n",which,target);fails++;}
        }
        funclist_free(&tables);
    }

    funclist_free(&fl);
    FuncList aliases;funclist_init(&aliases);
    funclist_add(&aliases,0x8000,4,NULL,FUNC_SRC_CONFIG,true);
    funclist_add(&aliases,0x8100,3,NULL,FUNC_SRC_CONFIG,true);
    uint16_t visible[2];
    int count=ff_visible_entries(&aliases,0x8000,4,visible,2);
    if(count!=1 || visible[0]!=0x8000){puts("FAIL other-bank entry truncated a trace");fails++;}
    funclist_free(&aliases);
    /* Other-bank code inside a dense table must not shorten the table. */
    static uint8_t bankrom[6*0x4000];memset(bankrom,0xc9,sizeof bankrom);
    const uint8_t indexed[]={0xdd,0x7e,1,0x87,0x21,0x10,0x80,0x16,0,0x5f,0x19,0x7e,0x23,0x66,0x6f,0xe9,
                             0x1a,0x80,0x1b,0x80,0x1c,0x80,0x1d,0x80,0x1e,0x80};
    memcpy(bankrom+4*0x4000,indexed,sizeof indexed);
    SmsRom br={0};br.data=bankrom;br.size=sizeof bankrom;br.num_banks=6;br.mapper=SMS_MAPPER_SEGA;
    funclist_init(&aliases);
    funclist_add(&aliases,0x8000,4,NULL,FUNC_SRC_CONFIG,true);
    funclist_add(&aliases,0x8016,3,NULL,FUNC_SRC_CONFIG,true);
    ff_discover(&br,&aliases,NULL,0);
    for(unsigned target=0x801a;target<=0x801e;target++)if(funclist_find(&aliases,(uint16_t)target,4)<0){
        printf("FAIL other-bank code shortened table at %04x\n",target);fails++;
    }
    funclist_free(&aliases);
    printf("\n%s (%d failures)\n", fails?"FAILED":"ALL PASS", fails);
    return fails?1:0;
}
