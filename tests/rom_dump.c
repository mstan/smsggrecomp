/* rom_dump <rom> — parse a ROM and print its identity. Verifies rom_parser
 * against real images. Build via PowerShell:
 *   gcc -I recompiler/src tests/rom_dump.c recompiler/src/rom_parser.c -o rd.exe
 */
#include "rom_parser.h"
#include <stdio.h>
int main(int argc, char **argv){
    if (argc < 2){ fprintf(stderr,"usage: rom_dump <rom>\n"); return 2; }
    SmsRom r;
    if (!rom_parse(argv[1], &r)) return 1;
    rom_free(&r);
    return 0;
}
