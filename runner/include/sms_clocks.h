/*
 * sms_clocks.h - SMS/GG timing constants.
 *
 * The SMS/GG VDP master clock is ~10.738635 MHz (3x NTSC colorburst); the Z80
 * runs at master/3 ~= 3.579545 MHz. The PSG is clocked at the same Z80 rate
 * with an internal /16 divider. Generated code accumulates Z80 T-states into
 * g_z80.cyc; the runner converts T-states <-> scanlines with these constants.
 *
 * NTSC: 262 lines/frame, ~228 Z80 T-states/line, active display lines 0..191,
 * the frame (VBlank) interrupt latches at the start of line 192.
 *
 * PAL would be 313 lines/frame; SMS PAL detection is a later concern - NTSC is
 * the bring-up target (the Sonic SMS/GG titles are NTSC-timed in practice).
 */
#ifndef SMS_CLOCKS_H
#define SMS_CLOCKS_H

#define SMS_Z80_HZ            3579545u   /* Z80 clock (NTSC), T-states/sec */
#define SMS_CYC_PER_LINE      228        /* Z80 T-states per scanline      */
#define SMS_LINES_PER_FRAME   262        /* NTSC                            */
#define SMS_ACTIVE_LINES      192        /* display lines 0..191           */
#define SMS_VBLANK_LINE       192        /* frame interrupt latches here   */
#define SMS_CYC_PER_FRAME     (SMS_CYC_PER_LINE * SMS_LINES_PER_FRAME)  /* 59736 */

/* PSG: Z80 clock / 16. (sn76489.c consumes "master cycles"; for the SMS the
 * master we feed it is the Z80 clock, matching its internal divider.) */
#define SMS_PSG_DIVIDER       16

#endif /* SMS_CLOCKS_H */
