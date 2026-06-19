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

    funclist_free(&fl);
    printf("\n%s (%d failures)\n", fails?"FAILED":"ALL PASS", fails);
    return fails?1:0;
}
