/* Minimal headless osd.h for smsref — replaces the libretro/SDL osd layer.
 * Genesis Plus GX's core includes "osd.h" via shared.h; this one is put first
 * on the -I path. No libretro-common streams, no CD stream macros. SMS/GG only.
 * t_config mirrors libretro/osd.h WITHOUT the HAVE_YM3438_CORE / HAVE_OPLL_CORE
 * / USE_PER_SOUND_CHANNELS_CONFIG fields (those macros are left undefined). */
#ifndef _OSD_H
#define _OSD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <zlib.h>   /* crc32() — GPGX loadrom uses it (normally via scrc32.h) */

#define MAX_INPUTS 8
#define MAX_KEYS 8
#define MAXPATHLEN 1024
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef M_PI
#define M_PI 3.1415926535897932385
#endif

#define CHEATS_UPDATE() ROMCheatUpdate()

#define HAVE_NO_SPRITE_LIMIT
#define MAX_SPRITES_PER_LINE 80
#define TMS_MAX_SPRITES_PER_LINE (config.no_sprite_limit ? MAX_SPRITES_PER_LINE : 4)
#define MODE4_MAX_SPRITES_PER_LINE (config.no_sprite_limit ? MAX_SPRITES_PER_LINE : 8)
#define MODE5_MAX_SPRITES_PER_LINE (config.no_sprite_limit ? MAX_SPRITES_PER_LINE : (bitmap.viewport.w >> 4))
#define MODE5_MAX_SPRITE_PIXELS (config.no_sprite_limit ? MAX_SPRITES_PER_LINE * 32 : max_sprite_pixels)

typedef struct { int8_t device; uint8_t port; uint8_t padtype; } t_input_config;

typedef struct
{
  char version[16];
  uint8_t hq_fm;
  uint8_t filter;
  uint8_t hq_psg;
  uint8_t ym2612;
  uint8_t ym2413;
  uint8_t mono;
  int16_t psg_preamp;
  int16_t fm_preamp;
  int16_t cdda_volume;
  int16_t pcm_volume;
  uint16_t lp_range;
  int16_t low_freq;
  int16_t high_freq;
  int16_t lg;
  int16_t mg;
  int16_t hg;
  uint8_t system;
  uint8_t region_detect;
  uint8_t master_clock;
  uint8_t vdp_mode;
  uint8_t force_dtack;
  uint8_t addr_error;
  uint8_t bios;
  uint8_t lock_on;
  uint8_t add_on;
  uint8_t overscan;
  uint8_t aspect_ratio;
  uint8_t ntsc;
  uint8_t lcd;
  uint8_t gg_extra;
  uint8_t left_border;
  uint8_t render;
  t_input_config input[MAX_INPUTS];
  uint8_t invert_mouse;
  uint8_t gun_cursor;
  uint32_t overclock;
  uint8_t no_sprite_limit;
  uint8_t enhanced_vscroll;
  uint8_t enhanced_vscroll_limit;
  uint8_t cd_latency;
} t_config;

extern t_config config;

extern char GG_ROM[256];
extern char AR_ROM[256];
extern char SK_ROM[256];
extern char SK_UPMEM[256];
extern char GG_BIOS[256];
extern char MD_BIOS[256];
extern char CD_BIOS_EU[256];
extern char CD_BIOS_US[256];
extern char CD_BIOS_JP[256];
extern char MS_BIOS_US[256];
extern char MS_BIOS_EU[256];
extern char MS_BIOS_JP[256];

extern void osd_input_update(void);
extern int load_archive(char *filename, unsigned char *buffer, int maxsize, char *extension);
extern void ROMCheatUpdate(void);

#endif /* _OSD_H */
