/*
 * game_config.h — parsed game.toml for the SMS/GG recompiler.
 * Schema: [game] output_prefix/platform/rom/crc32, [ram_layout] (optional
 * addresses), [mapper] kind, [functions] extra[]/blacklist[].
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "rom_parser.h"

#define GC_MAX_FUNCS 8192
#define GC_ADDR_UNSET 0xFFFFFFFFu

typedef struct {
    char        output_prefix[64];
    char        rom_path[260];      /* as written in game.toml (relative) */
    SmsPlatform platform;
    bool        have_crc;
    uint32_t    crc32;
    char        mapper_kind[16];

    /* [ram_layout] — GC_ADDR_UNSET if absent */
    uint32_t    game_mode_addr;
    uint32_t    vblank_count_addr;
    uint32_t    player_object_addr;

    uint16_t    extra[GC_MAX_FUNCS];     int extra_count;
    uint16_t    blacklist[GC_MAX_FUNCS]; int blacklist_count;

    /* [jump_tables] - computed-jump targets the static finder can't resolve:
     * an array of 16-bit LE pointer tables read straight from ROM. Parallel
     * arrays addr/bank/count (bank = the bank mapped into the table's slot, and
     * the bank its targets run under; -1 = fixed). */
    uint16_t    jt_addr[64];
    int         jt_bank[64];
    int         jt_count[64];
    int         jt_n;
} GameConfig;

/* Load + validate. Returns false (with a message on stderr) on hard errors
 * (missing file, missing output_prefix/rom). */
bool game_config_load(const char *path, GameConfig *out);
