/*
 * main_sms.c — SmsRecomp entry point.
 *   SmsRecomp <rom>            --game <game.toml>
 *   SmsRecomp --game <game.toml>     (ROM taken from [game].rom, relative to it)
 *
 * Pipeline: load game.toml -> parse ROM -> seed vectors + config extras ->
 * static discovery -> coverage probe (emission lands next).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rom_parser.h"
#include "game_config.h"
#include "function_finder.h"
#include "code_generator.h"

/* Directory portion of a path (everything up to the last / or \). Writes into
 * dir[] with the trailing separator; empty string if none. */
static void dirname_of(const char *path, char *dir, size_t cap){
    size_t len = strlen(path);
    size_t cut = 0;
    for (size_t i=0;i<len;i++) if (path[i]=='/'||path[i]=='\\') cut=i+1;
    if (cut >= cap) cut = cap-1;
    memcpy(dir, path, cut); dir[cut]='\0';
}

/* Re-derive the runtime's 256-byte FNV-1a32 code-CRC at a z80 address, reading
 * through the manifest's per-slot banks (matching glue.c mb_code_crc on the live
 * bus). Returns 0 in *ok if the window spills into RAM (>=0xC000) - those bytes
 * weren't in ROM so the entry can't be CRC-verified statically. */
static uint32_t manifest_rom_crc(const SmsRom *rom, uint16_t addr,
                                 int b0, int b1, int b2, int *ok){
    const int banks[3] = { b0, b1, b2 };
    uint32_t h = 2166136261u;
    *ok = 1;
    for (int i=0;i<256;i++){
        uint32_t ai = (uint32_t)addr + (uint32_t)i;     /* no 16-bit wrap: addr<0xC000 */
        if (ai >= 0xC000){ *ok = 0; return 0; }          /* RAM byte: unverifiable */
        size_t off = rom_z80_to_offset(rom, (uint16_t)ai, banks[ai >> 14]);
        uint8_t v = (off==SIZE_MAX) ? 0xFF : rom_read_offset(rom, off);
        h ^= v; h *= 16777619u;
    }
    return h;
}

/* Ingest the runtime dispatch manifest (PRINCIPLES #16: runtime says an
 * (addr,bank) IS an executed entry; the ROM cross-check confirms WHICH bytes).
 * Each verified line seeds a banked entry so ff_discover traces + cascades it. */
static int manifest_seed(const SmsRom *rom, FuncList *fl, const char *path){
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int seeded=0, rejected=0, total=0;
    unsigned a,b0,b1,b2,crc;
    while (fscanf(f, "%x %x %x %x %x", &a,&b0,&b1,&b2,&crc) == 5){
        total++;
        if (a >= 0xC000) continue;                       /* RAM target: not ROM code */
        int slot = a >> 14;
        int bank = (slot==0) ? (int)b0 : (slot==1) ? (int)b1 : (int)b2;
        int ok;
        uint32_t rc = manifest_rom_crc(rom, (uint16_t)a, (int)b0,(int)b1,(int)b2, &ok);
        if (ok && rc != crc){ rejected++; continue; }    /* stale/garbage vs current ROM */
        if (funclist_find(fl,(uint16_t)a,bank) < 0){
            funclist_add(fl,(uint16_t)a,bank,NULL,FUNC_SRC_MANIFEST,true);
            seeded++;
        }
    }
    fclose(f);
    printf("[SmsRecomp] dispatch manifest: %d entries, %d seeded, %d rejected (ROM-CRC)\n",
           total, seeded, rejected);
    return seeded;
}

int main(int argc, char **argv){
    const char *rom_arg = NULL;
    const char *game_toml = NULL;

    for (int i=1;i<argc;i++){
        if (strcmp(argv[i],"--game")==0 && i+1<argc) game_toml = argv[++i];
        else if (argv[i][0] != '-') rom_arg = argv[i];
        else { fprintf(stderr,"[SmsRecomp] unknown arg: %s\n", argv[i]); }
    }
    if (!game_toml){
        fprintf(stderr,"usage: SmsRecomp [<rom>] --game <game.toml>\n");
        return 2;
    }

    printf("[SmsRecomp] SMS/GG Z80 static recompiler\n");

    GameConfig cfg;
    if (!game_config_load(game_toml, &cfg)) return 1;

    /* Resolve ROM path: explicit arg wins; else [game].rom relative to toml. */
    char rompath[520];
    if (rom_arg){
        snprintf(rompath, sizeof(rompath), "%s", rom_arg);
    } else {
        char dir[260]; dirname_of(game_toml, dir, sizeof(dir));
        snprintf(rompath, sizeof(rompath), "%s%s", dir, cfg.rom_path);
    }

    SmsRom rom;
    if (!rom_parse(rompath, &rom)) return 1;

    if (cfg.have_crc && cfg.crc32 != rom.crc32){
        fprintf(stderr,"[SmsRecomp] WARNING: ROM crc32 0x%08X != game.toml 0x%08X\n",
                rom.crc32, cfg.crc32);
    }
    if (cfg.platform != rom.platform){
        fprintf(stderr,"[SmsRecomp] NOTE: game.toml platform=%s but ROM region implies %s\n",
                cfg.platform==SMS_PLATFORM_GG?"gg":"sms",
                rom.platform==SMS_PLATFORM_GG?"gg":"sms");
    }

    FuncList fl; funclist_init(&fl);
    ff_seed_vectors(&fl);
    for (int i=0;i<cfg.extra_count;i++)
        funclist_add(&fl, cfg.extra[i], -1, NULL, FUNC_SRC_CONFIG, true);

    /* Resolve [jump_tables]: read each 16-bit LE pointer table from ROM and
     * seed its targets as banked entries (ground truth; PRINCIPLES #16). */
    int jt_added = 0;
    for (int t=0;t<cfg.jt_n;t++){
        uint16_t taddr = cfg.jt_addr[t];
        int bank = cfg.jt_bank[t];
        int slot_bank = (bank >= 0) ? bank : (taddr >> 14);
        for (int e=0;e<cfg.jt_count[t];e++){
            size_t off = rom_z80_to_offset(&rom, (uint16_t)(taddr + e*2), slot_bank);
            if (off==SIZE_MAX || off+1 >= rom.size) continue;
            uint16_t target = (uint16_t)(rom_read_offset(&rom, off) |
                                         (rom_read_offset(&rom, off+1) << 8));
            funclist_add(&fl, target, bank, NULL, FUNC_SRC_TABLE, true);
            jt_added++;
        }
    }
    if (jt_added) printf("[SmsRecomp] jump tables seeded %d entries\n", jt_added);

    /* Profile-guided seeds from the runtime dispatch manifest (next to game.toml). */
    {
        char dir[260]; dirname_of(game_toml, dir, sizeof(dir));
        char mpath[320]; snprintf(mpath,sizeof mpath,"%sdispatch_manifest.txt", dir);
        manifest_seed(&rom, &fl, mpath);
    }

    ff_discover(&rom, &fl, cfg.blacklist, cfg.blacklist_count);

    printf("[SmsRecomp] discovered %d function entries\n", fl.count);

    cg_probe(&rom, &fl);

    /* Emit generated C into <game.toml dir>/generated/. */
    {
        char dir[260]; dirname_of(game_toml, dir, sizeof(dir));
        char gendir[300]; snprintf(gendir, sizeof(gendir), "%sgenerated", dir);
        cg_emit(&rom, &fl, &cfg, gendir);
    }

    funclist_free(&fl);
    rom_free(&rom);
    return 0;
}
