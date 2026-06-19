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

/* Open a window showing the `crop_w`x`crop_h` region at (`crop_x`,`crop_y`) of
 * the runner's 256x192 ARGB framebuffer, scaled by `scale`. Returns false on
 * failure. Presentation is vsync-paced. */
bool host_init(int fb_w, int fb_h,
               int crop_x, int crop_y, int crop_w, int crop_h,
               int scale, const char *title);

/* Upload `fb` (fb_w*fb_h ARGB8888), draw, present (vsync), and pump events.
 * Returns true to keep running, false if the user requested quit (window
 * close / Esc). */
bool host_present(const uint32_t *fb, int fb_w, int fb_h);

void host_shutdown(void);

#endif /* HOST_SDL_H */
