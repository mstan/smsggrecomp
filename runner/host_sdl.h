/*
 * host_sdl.h - optional SDL2 live display host for the SMS/GG runner.
 *
 * Compiled only when the runner is built with -DSMS_HAVE_SDL (link -lSDL2).
 * The headless build omits this TU entirely. The runner core (glue.c) stays
 * SDL-agnostic: main.c registers host_present() as the per-frame callback via
 * glue_set_frame_callback(), so the game loop drives the window without glue.c
 * ever knowing about SDL.
 */
#ifndef HOST_SDL_H
#define HOST_SDL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
/* 0 keeps ordinary native SDL; 1..16 explicitly request the GPU job path. */
void host_set_internal_scale(int scale);
void host_set_hidden(bool hidden);
void host_set_fullscreen(int fullscreen);
void host_report(FILE *out);
/* Headless presented-frame capture: write frame_%06u.png of what the window shows
 * (GPU target or cropped native fb, aspect-fitted onto a w x h canvas when
 * w,h > 0) for 0-based frames from..to every `every`. */
void host_set_capture(const char *dir, int w, int h, uint64_t from, uint64_t to, uint64_t every);
uint32_t host_capture_written(void);
bool host_dump_gpu(const char *path);
bool host_uses_gpu(void);

/* Open a window showing the `crop_w`x`crop_h` region at (`crop_x`,`crop_y`) of
 * the runner's 256x192 ARGB framebuffer, scaled by `scale`. Returns false on
 * failure. Presentation is vsync-paced. */
bool host_init(int fb_w, int fb_h,
               int crop_x, int crop_y, int crop_w, int crop_h,
               int scale, const char *title);

/* Upload `fb` (fb_w*fb_h ARGB8888), draw, present, pump events, and pace the
 * loop to the frame cap (see host_set_frame_cap). Returns true to keep running,
 * false if the user requested quit (window close / Esc). */
bool host_present(const uint32_t *fb, int fb_w, int fb_h);

/* Cap the present loop to `fps` frames/sec using a precise wall-clock limiter,
 * so the game runs at realtime regardless of the monitor's refresh rate. Pass 0
 * to disable (uncapped). Without this the loop would run as fast as the display
 * refreshes (e.g. 2x on a 120 Hz monitor). */
void host_set_frame_cap(double fps);
void host_set_prepare_pacing(bool fast);

void host_shutdown(void);

/* ---- audio ----
 * Open an SDL audio device and an SDL_AudioStream that resamples the PSG's
 * native `src_rate` (interleaved-stereo S16) to the device rate. Returns false
 * on failure (the caller then runs without sound). */
bool host_audio_init(uint32_t src_rate);

/* Submit `frame_count` interleaved-stereo S16 frames; they are resampled and
 * queued to the device. Safe to call with the device unopened (no-op). */
void host_audio_submit(const int16_t *stereo_frames, size_t frame_count);

void host_audio_shutdown(void);

/* Current player-1 controller state as an SMS_PAD_* bitmask (1 = pressed),
 * built from the keyboard in host_present(). main.c bridges this into glue via
 * glue_set_pad1() each frame, keeping the host free of glue calls. */
uint8_t host_get_pad1(void);

bool host_poll(void);
void host_show(void);
void host_audio_pause(bool);
const char *host_controller_label(void);
#endif /* HOST_SDL_H */
