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

/* Register a live per-frame callback (e.g. the SDL host). When set, every
 * completed frame is rendered to the framebuffer and passed to `cb` as a
 * 256x192 ARGB8888 image. `cb` returns nonzero to request shutdown (the run
 * loop then unwinds cleanly, as on the frame limit). Keeps glue.c host-agnostic. */
void     glue_set_frame_callback(int (*cb)(const uint32_t *fb, int w, int h));

uint64_t glue_frame_count(void);
int      glue_dispatch_miss_count(void);
size_t   glue_rom_size(void);

#endif /* GLUE_H */
