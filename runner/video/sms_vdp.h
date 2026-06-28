/*
 * sms_vdp.h - SMS/GG VDP (Texas 9918-derived mode-4 video).
 *
 * Register-level model shared by SMS and GG (PRINCIPLES #25): identical VRAM /
 * register interface; GG differs only in CRAM depth (12-bit, 2 bytes/entry vs
 * SMS 6-bit, 1 byte) and the viewport crop - neither of which affects the
 * register/IRQ surface this header exposes.
 *
 * This is the timing + memory core needed to run the CPU and deliver
 * line/frame interrupts. Pixel rendering (mode-4 tilemap + sprites) layers on
 * top later; the dispatch-miss loop needs accurate interrupts, not pixels.
 */
#ifndef SMS_VDP_H
#define SMS_VDP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  vram[0x4000];     /* 16 KB video RAM */
    uint8_t  cram[0x40];       /* 32 (SMS) / 64 (GG) bytes colour RAM */
    uint8_t  reg[16];          /* VDP registers 0..15 */

    uint16_t addr;             /* current VRAM/CRAM address (14-bit) */
    uint8_t  code;             /* address-control code (0..3) */
    uint8_t  latch;            /* first control byte */
    bool     ctrl_second;      /* awaiting the second control byte */
    uint8_t  readbuf;          /* read prefetch buffer */

    uint8_t  status;           /* status flags (bit7 frame INT, 6 ovr, 5 col) */
    int      line;             /* current scanline 0..261 */
    int      line_counter;     /* line-interrupt down-counter */
    bool     frame_irq;        /* frame (VBlank) interrupt pending */
    bool     line_irq;         /* line interrupt pending */

    bool     is_gg;            /* Game Gear (12-bit CRAM, latched writes) */
    uint8_t  cram_latch;       /* GG: low byte held until odd-address write */
} SmsVdp;

extern SmsVdp g_vdp;

void    vdp_reset(bool is_gg);
void    vdp_control_write(uint8_t v);   /* port $BF write */
void    vdp_data_write(uint8_t v);      /* port $BE write */
uint8_t vdp_data_read(void);            /* port $BE read  */
uint8_t vdp_status_read(void);          /* port $BF read (clears INT flags) */
uint8_t vdp_vcounter(void);             /* port $7E read (mode-4 NTSC jback) */
uint8_t vdp_hcounter(int sub_line_cyc); /* port $7F read; sub_line_cyc = Z80 T-states into the current line */

/* Step the VDP one scanline (called by the runner's sms_sync). Updates the
 * line counter and latches line/frame interrupts. */
void    vdp_step_line(void);

/* Is the VDP currently asserting the Z80 IRQ line? (frame or line int,
 * gated by the matching enable bits in registers 1 and 0). */
bool    vdp_irq_asserted(void);

/* SMS display dimensions (mode 4). GG shows a 160x144 centred crop. */
#define SMS_SCREEN_W 256
#define SMS_SCREEN_H 192
#define GG_SCREEN_W  160
#define GG_SCREEN_H  144

/* Render the current frame (whole VRAM/CRAM state) into a 256x192 ARGB8888
 * framebuffer (fb must hold SMS_SCREEN_W*SMS_SCREEN_H pixels). Full-frame
 * render from end-of-frame state - no per-scanline raster effects yet. */
void    vdp_render_frame(uint32_t *fb);

/* Optional VDP-write observer (dev instrumentation, decoupled from the runner).
 * Called with the RESOLVED write each time the CPU writes a VDP register or
 * CRAM (VRAM writes are NOT reported - too high-volume, and not needed for the
 * raster-effect question). kind is VDPW_REG (addr = register index 0..15) or
 * VDPW_CRAM (addr = resolved CRAM index). Lets the runner keep an always-on
 * ring of VDP writes tagged with the current scanline, to detect mid-frame
 * raster effects (per-scanline hscroll, palette cycling) without the VDP
 * knowing anything about frames/scanlines/rings. NULL disables. */
enum { VDPW_REG = 0, VDPW_CRAM = 1, VDPW_VRAM = 2 };
typedef void (*VdpWriteObserver)(int kind, uint16_t addr, uint8_t value);
void    vdp_set_write_observer(VdpWriteObserver f);

#endif /* SMS_VDP_H */
