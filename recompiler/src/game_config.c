/*
 * game_config.c — parse game.toml. See game_config.h.
 */
#include "game_config.h"
#include "toml.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src){
    snprintf(dst, cap, "%s", src ? src : "");
}

static void load_int_array(toml_table_t *tab, const char *key,
                           uint16_t *out, int *count, int cap){
    *count = 0;
    if (!tab) return;
    toml_array_t *arr = toml_array_in(tab, key);
    if (!arr) return;
    int n = toml_array_nelem(arr);
    for (int i=0;i<n && *count<cap;i++){
        toml_datum_t d = toml_int_at(arr, i);
        if (d.ok) out[(*count)++] = (uint16_t)d.u.i;
    }
}

bool game_config_load(const char *path, GameConfig *out){
    memset(out, 0, sizeof(*out));
    out->game_mode_addr = out->vblank_count_addr = out->player_object_addr = GC_ADDR_UNSET;
    copy_str(out->mapper_kind, sizeof(out->mapper_kind), "sega");
    out->platform = SMS_PLATFORM_SMS;

    FILE *f = fopen(path, "r");
    if (!f){ fprintf(stderr,"[cfg] cannot open %s\n", path); return false; }
    char err[200];
    toml_table_t *root = toml_parse_file(f, err, sizeof(err));
    fclose(f);
    if (!root){ fprintf(stderr,"[cfg] parse error in %s: %s\n", path, err); return false; }

    toml_table_t *game = toml_table_in(root, "game");
    if (game){
        toml_datum_t d;
        d = toml_string_in(game,"output_prefix"); if (d.ok){ copy_str(out->output_prefix,sizeof(out->output_prefix),d.u.s); free(d.u.s); }
        d = toml_string_in(game,"rom");           if (d.ok){ copy_str(out->rom_path,sizeof(out->rom_path),d.u.s); free(d.u.s); }
        d = toml_string_in(game,"platform");      if (d.ok){ out->platform = (strcmp(d.u.s,"gg")==0)?SMS_PLATFORM_GG:SMS_PLATFORM_SMS; free(d.u.s); }
        d = toml_int_in(game,"crc32");            if (d.ok){ out->have_crc=true; out->crc32=(uint32_t)d.u.i; }
    }

    toml_table_t *rl = toml_table_in(root, "ram_layout");
    if (rl){
        toml_datum_t d;
        d = toml_int_in(rl,"game_mode");     if (d.ok) out->game_mode_addr=(uint32_t)d.u.i;
        d = toml_int_in(rl,"vblank_count");  if (d.ok) out->vblank_count_addr=(uint32_t)d.u.i;
        d = toml_int_in(rl,"player_object"); if (d.ok) out->player_object_addr=(uint32_t)d.u.i;
    }

    toml_table_t *mp = toml_table_in(root, "mapper");
    if (mp){ toml_datum_t d = toml_string_in(mp,"kind"); if (d.ok){ copy_str(out->mapper_kind,sizeof(out->mapper_kind),d.u.s); free(d.u.s); } }

    toml_table_t *fn = toml_table_in(root, "functions");
    load_int_array(fn, "extra",     out->extra,     &out->extra_count,     GC_MAX_FUNCS);
    load_int_array(fn, "blacklist", out->blacklist, &out->blacklist_count, GC_MAX_FUNCS);

    toml_table_t *jt = toml_table_in(root, "jump_tables");
    if (jt){
        toml_array_t *aa = toml_array_in(jt, "addr");
        toml_array_t *ba = toml_array_in(jt, "bank");
        toml_array_t *ca = toml_array_in(jt, "count");
        int n = aa ? toml_array_nelem(aa) : 0;
        for (int i=0;i<n && out->jt_n<64;i++){
            toml_datum_t da = toml_int_at(aa, i);
            if (!da.ok) continue;
            int k = out->jt_n++;
            out->jt_addr[k]  = (uint16_t)da.u.i;
            out->jt_bank[k]  = -1;
            out->jt_count[k] = 0;
            if (ba){ toml_datum_t db = toml_int_at(ba, i); if (db.ok) out->jt_bank[k]  = (int)db.u.i; }
            if (ca){ toml_datum_t dc = toml_int_at(ca, i); if (dc.ok) out->jt_count[k] = (int)dc.u.i; }
        }
    }

    toml_free(root);

    if (out->output_prefix[0]=='\0'){ fprintf(stderr,"[cfg] missing [game].output_prefix\n"); return false; }
    if (out->rom_path[0]=='\0'){ fprintf(stderr,"[cfg] missing [game].rom\n"); return false; }

    printf("[cfg] prefix=%s platform=%s rom=%s extras=%d blacklist=%d\n",
           out->output_prefix, out->platform==SMS_PLATFORM_GG?"gg":"sms",
           out->rom_path, out->extra_count, out->blacklist_count);
    return true;
}
