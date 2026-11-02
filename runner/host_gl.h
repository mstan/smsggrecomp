#ifndef SMS_HOST_GL_H
#define SMS_HOST_GL_H
#include <stdbool.h>
#include <stdio.h>
#include <SDL.h>
#include "video/sms_vdp.h"
bool host_gl_init(SDL_Window *window, int x, int y, int w, int h, int scale);
void host_gl_present(const VdpFrameJob *frame);
void host_gl_shutdown(void);
void host_gl_report(FILE *out);
bool host_gl_dump(const char *path);
bool host_gl_readback(uint32_t *pixels,size_t capacity,int *width,int *height);
#endif
