/*
 * glue.h - runner core entry points used by main.c.
 */
#ifndef GLUE_H
#define GLUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Load a .sms/.gg image into the runner's ROM space. Returns false on error. */
bool     glue_load_rom(const char *path);

/* Initialise bus/VDP/CPU for a fresh run. frame_limit==0 means run forever. */
void     glue_init(bool is_gg, uint64_t frame_limit);

/* Enter the game at the reset vector. Returns when the frame limit is hit
 * (the reset routine itself never returns; a longjmp unwinds out of it). */
void     glue_run(void);

/* Oracle reference run: execute the WHOLE game under the vendored superzazu
 * Z80 interpreter (instead of the recompiled code) over the SAME bus / VDP /
 * IO / timing. Used dev-only to diff against the recompiled run: because both
 * share the clean-room renderer, a renderer bug shows in both while a recomp
 * bug shows only in glue_run(). Frame dump / live callback / frame limit all
 * work identically. */
void     glue_run_interp(void);

/* Dump frame `frame` to `path` (PNG) when it completes. */
void     glue_set_dump(uint64_t frame, const char *path);

/* Enable a per-frame VDP-state trace (CSV: frame, FNV hashes of VRAM/CRAM/regs,
 * plus r8/r9/r0/r1). Always-on from frame 0; identical in glue_run() and
 * glue_run_interp(). Diffing a recomp trace against an --interp trace finds the
 * first frame whose VDP STATE diverges -> renderer-vs-recomp discriminator.
 * Pass NULL to disable. */
void     glue_set_vdp_trace(const char *path);
void     glue_set_psg_log(const char *path);   /* chip_ring: (cyc,val) PSG-write log */
void     glue_psg_log_close(void);

#ifdef SMS_CYC_WATCH
/* Cycle-watch oracle: record (hit_index, g_z80.cyc) each time `addr` is reached
 * (sampled in the SMS_PC hook). The offset-independent per-anchor cycle DELTA
 * is diffed against Mesen's exec-callback cycleCount (tools/oracle). */
void     glue_set_cyc_watch(uint16_t addr, const char *path);
void     glue_cyc_watch_close(void);
#endif

/* Register a live per-frame callback (e.g. the SDL host). When set, every
 * completed frame is rendered to the framebuffer and passed to `cb` as a
 * 256x192 ARGB8888 image. `cb` returns nonzero to request shutdown (the run
 * loop then unwinds cleanly, as on the frame limit). Keeps glue.c host-agnostic. */
void     glue_set_frame_callback(int (*cb)(const uint32_t *fb, int w, int h));

/* Controller buttons (1 = pressed). The D-pad + two face buttons map to the
 * SMS controller ports $DC/$DD; START is the Game Gear's port-$00 bit-7 button
 * (the SMS Pause button is a separate NMI, not a controller bit). */
enum {
    SMS_PAD_UP    = 0x01, SMS_PAD_DOWN  = 0x02,
    SMS_PAD_LEFT  = 0x04, SMS_PAD_RIGHT = 0x08,
    SMS_PAD_B1    = 0x10, SMS_PAD_B2    = 0x20,
    SMS_PAD_START = 0x40
};

/* Set the live controller state for player 1 / 2 (an SMS_PAD_* bitmask). The
 * SDL host refreshes these from the keyboard each frame; headless runs leave
 * them 0, so the ports read idle. */
void     glue_set_pad1(uint8_t pressed);
void     glue_set_pad2(uint8_t pressed);

/* Headless scripted input: when set, the runner calls this once per completed
 * frame to fetch player-1's mask for the upcoming frame — deterministic input
 * for automated tests, no window required. The SDL host uses glue_set_pad1()
 * instead. Pass NULL to disable. */
void     glue_set_input_cb(uint8_t (*cb)(uint64_t frame));

/* Register an audio sink. When set, the PSG is synthesised in lockstep with
 * the VDP (on every run path) and each completed video frame's output is
 * handed to `sink` as interleaved-stereo S16 frames (L,R per frame) at
 * glue_audio_sample_rate() Hz. Pass NULL to disable (the default: headless
 * oracle/diff runs synthesise nothing). */
void     glue_set_audio_sink(void (*sink)(const int16_t *stereo_frames, size_t frame_count));
uint32_t glue_audio_sample_rate(void);

/* The emulated frame rate in Hz (NTSC: Z80 clock / cycles-per-frame ≈ 59.92).
 * The SDL host uses it to pace the window to realtime independent of the
 * monitor's refresh. */
double   glue_frame_rate(void);

uint64_t glue_frame_count(void);
int      glue_dispatch_miss_count(void);
size_t   glue_rom_size(void);

#endif /* GLUE_H */
