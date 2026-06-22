/*
 * sn76489.c — cycle-stamped SN76489 PSG (Sega Master System / Game Gear).
 *
 * Clean-room SN76489 (SMS/GG/Mega Drive variant) implemented from public
 * hardware documentation — NO emulator-core code, so this carries no third-
 * party copyleft. See LICENSING.md.
 *
 * Models 3 square-tone channels + 1 LFSR noise channel, 4-bit logarithmic
 * volume (2 dB/step, 0xF = off), and the SMS/GG 16-bit noise LFSR (white-noise
 * taps at bits 0 and 3, feedback -> bit 15), with a one-pole low-pass to round
 * the step edges.
 *
 * Clocking: the PSG runs at the Z80 clock (3.579545 MHz NTSC) with an internal
 * /16 divider, so it emits one frame per 16 Z80 T-states (~223.7 kHz). The
 * runner accumulates Z80 T-states and calls psg_advance() with the delta, so
 * the PSG advances in lockstep with the VDP through the same timing path.
 *
 * Output is STEREO so the Game Gear stereo register ($06) can route each
 * channel to L and/or R independently. On the SMS the mask stays all-on, so
 * both channels carry an identical signal.
 */
#include "sn76489.h"        /* our wrapper API */
#include "sms_clocks.h"     /* SMS_Z80_HZ, SMS_PSG_DIVIDER */
#include <string.h>

#define PSG_FRAME_DIVISOR_Z80   ((uint32_t)SMS_PSG_DIVIDER)   /* 16 T-states / frame */

static int        s_inited = 0;
static uint32_t   s_leftover_z80_cycles = 0;     /* sub-frame Z80-cycle remainder */

#define PSG_SCRATCH_FRAMES  8192
static int16_t s_scratch[PSG_SCRATCH_FRAMES * 2];   /* interleaved L,R */
static size_t  s_scratch_write = 0;                 /* in frames */
static size_t  s_scratch_read  = 0;                 /* in frames */

/* ---- clean-room SN76489 ---- */
typedef struct {
    uint16_t tone_period[3];   /* 10-bit reload value */
    int32_t  tone_counter[3];
    uint8_t  tone_out[3];      /* square flip-flop (0/1) */
    uint8_t  vol[4];           /* 4-bit attenuation; index 3 = noise */
    uint8_t  noise_ctrl;       /* low 3 bits: fb-mode + rate */
    int32_t  noise_counter;
    uint16_t lfsr;
    uint8_t  noise_out;
    uint8_t  latch;            /* (chan<<1)|type of last LATCH byte */
    uint8_t  gg_stereo;        /* GG $06: hi nibble L-en, lo nibble R-en, per ch0..3 */
    int32_t  lpf_yL, lpf_prevL;   /* left low-pass state  */
    int32_t  lpf_yR, lpf_prevR;   /* right low-pass state */
} SN76489;

/* First-order low-pass matching the reference (clownmdemu low-pass-filter.c
 * with coefficients 26.044, 24.044): out = ((in + prevIn)*SAMPLE_MAGIC +
 * prevOut*OUTPUT_MAGIC) >> 16. */
#define SN_LPF_SAMPLE_MAGIC  2517   /* round(1.0    * 65536 / 26.044) */
#define SN_LPF_OUTPUT_MAGIC  60504  /* round(24.044 * 65536 / 26.044) */
static SN76489 s_sn;

/* 4-bit attenuation -> linear amplitude. 2 dB/step (×10^-0.1), 0xF = silence.
 * Peak 0x1FFF with per-channel /4 headroom so 4 summed channels (4*8191 =
 * 32764) just fit int16 on either stereo side. */
static const int16_t SN_VOL[16] = {
    0x1FFF, 0x196A, 0x1430, 0x1009, 0x0CBD, 0x0A1E, 0x0809, 0x0662,
    0x0512, 0x0407, 0x0333, 0x028A, 0x0204, 0x019A, 0x0146, 0x0000
};

static void sn_reset(SN76489 *p)
{
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) { p->tone_period[i] = 1; p->tone_counter[i] = 1; }
    p->vol[0] = p->vol[1] = p->vol[2] = p->vol[3] = 0xF;  /* all silent */
    p->noise_counter = 0x10;
    p->lfsr = 0x8000;
    p->gg_stereo = 0xFF;       /* every channel on both sides (SMS / GG default) */
}

static void sn_write(SN76489 *p, uint8_t b)
{
    if (b & 0x80) {                       /* LATCH/DATA byte: %1 cc t dddd */
        p->latch = (b >> 4) & 0x07;
        uint8_t chan = (b >> 5) & 0x03;
        uint8_t type = (b >> 4) & 0x01;   /* 1 = volume, 0 = tone/noise */
        if (type)            p->vol[chan] = b & 0x0F;
        else if (chan < 3)   p->tone_period[chan] = (uint16_t)((p->tone_period[chan] & 0x3F0) | (b & 0x0F));
        else               { p->noise_ctrl = b & 0x07; p->lfsr = 0x8000; }
    } else {                              /* DATA byte: %0 - dddddd */
        uint8_t chan = (p->latch >> 1) & 0x03;
        uint8_t type = p->latch & 0x01;
        if (type)            p->vol[chan] = b & 0x0F;
        else if (chan < 3)   p->tone_period[chan] = (uint16_t)((p->tone_period[chan] & 0x00F) | ((uint16_t)(b & 0x3F) << 4));
        else               { p->noise_ctrl = b & 0x07; p->lfsr = 0x8000; }
    }
}

static int sn_noise_reload(const SN76489 *p)
{
    switch (p->noise_ctrl & 0x03) {
        case 0:  return 0x10;
        case 1:  return 0x20;
        case 2:  return 0x40;
        default: return p->tone_period[2] ? p->tone_period[2] : 1;  /* track ch2 */
    }
}

/* Advance all four channels by one frame, returning each channel's signed
 * amplitude in amp[0..3] (tone0, tone1, tone2, noise). The per-frame state
 * mutation (counters, flip-flops, LFSR) happens here exactly once. */
static inline void sn_tick(SN76489 *p, int32_t amp[4])
{
    for (int i = 0; i < 3; i++) {
        if (--p->tone_counter[i] <= 0) {
            p->tone_counter[i] = p->tone_period[i] ? p->tone_period[i] : 1;
            p->tone_out[i] ^= 1;
        }
        amp[i] = p->tone_out[i] ? SN_VOL[p->vol[i]] : -SN_VOL[p->vol[i]];
    }

    if (--p->noise_counter <= 0) {
        p->noise_counter = sn_noise_reload(p);
        uint16_t fb = (p->noise_ctrl & 0x04)            /* 1 = white */
                    ? ((p->lfsr ^ (p->lfsr >> 3)) & 1)  /* taps bit0 ^ bit3 */
                    : (p->lfsr & 1);                    /* periodic */
        p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
        p->noise_out = (uint8_t)(p->lfsr & 1);
    }
    amp[3] = p->noise_out ? SN_VOL[p->vol[3]] : -SN_VOL[p->vol[3]];
}

static inline int16_t sn_lpf(int32_t mix, int32_t *y, int32_t *prev)
{
    int64_t lp = ((int64_t)(mix + *prev) * SN_LPF_SAMPLE_MAGIC
                + (int64_t)(*y) * SN_LPF_OUTPUT_MAGIC) / 65536;
    *prev = mix;
    int32_t out = (int32_t)lp;
    if (out >  32767) out =  32767;
    if (out < -32768) out = -32768;
    *y = out;
    return (int16_t)out;
}

/* Render one stereo frame, applying the GG stereo routing mask. */
static void sn_render_frame(SN76489 *p, int16_t *l, int16_t *r)
{
    int32_t amp[4];
    sn_tick(p, amp);

    int32_t mixL = 0, mixR = 0;
    uint8_t m = p->gg_stereo;
    for (int i = 0; i < 4; i++) {
        if (m & (uint8_t)(0x10 << i)) mixL += amp[i];   /* left  enables: bits 4-7 */
        if (m & (uint8_t)(0x01 << i)) mixR += amp[i];   /* right enables: bits 0-3 */
    }

    *l = sn_lpf(mixL, &p->lpf_yL, &p->lpf_prevL);
    *r = sn_lpf(mixR, &p->lpf_yR, &p->lpf_prevR);
}

void psg_init(void)
{
    sn_reset(&s_sn);
    s_scratch_write = s_scratch_read = 0;
    s_leftover_z80_cycles = 0;
    s_inited = 1;
}

void psg_advance(uint32_t cycles_z80)
{
    if (!s_inited) psg_init();
    if (cycles_z80 == 0) return;

    uint64_t total = (uint64_t)cycles_z80 + s_leftover_z80_cycles;
    size_t   frames_to_emit = (size_t)(total / PSG_FRAME_DIVISOR_Z80);
    s_leftover_z80_cycles = (uint32_t)(total % PSG_FRAME_DIVISOR_Z80);

    while (frames_to_emit > 0) {
        size_t avail = PSG_SCRATCH_FRAMES - s_scratch_write;
        if (avail == 0) return;  /* scratch full; drop (no sink draining) */
        size_t chunk = frames_to_emit < avail ? frames_to_emit : avail;
        for (size_t i = 0; i < chunk; i++)
            sn_render_frame(&s_sn,
                            &s_scratch[(s_scratch_write + i) * 2],
                            &s_scratch[(s_scratch_write + i) * 2 + 1]);
        s_scratch_write += chunk;
        frames_to_emit  -= chunk;
    }
}

void psg_write(uint8_t value)
{
    if (!s_inited) psg_init();
    sn_write(&s_sn, value);
}

void psg_write_stereo(uint8_t mask)
{
    if (!s_inited) psg_init();
    s_sn.gg_stereo = mask;
}

size_t psg_render(int16_t *out, size_t frame_count)
{
    if (!out || frame_count == 0) return 0;
    size_t available = s_scratch_write - s_scratch_read;
    size_t copy = frame_count < available ? frame_count : available;
    if (copy > 0) {
        memcpy(out, &s_scratch[s_scratch_read * 2], copy * 2 * sizeof(int16_t));
        s_scratch_read += copy;
    }
    if (s_scratch_read >= s_scratch_write) {
        s_scratch_read = s_scratch_write = 0;
    }
    return copy;
}

size_t psg_frames_available(void)
{
    return s_scratch_write - s_scratch_read;
}

uint32_t psg_sample_rate(void)
{
    return (uint32_t)(SMS_Z80_HZ / PSG_FRAME_DIVISOR_Z80);
}

void psg_reset_leftover(void)
{
    s_leftover_z80_cycles = 0;
}

/* Save-state hooks (own-backend snapshots): the whole SN76489 register/counter
 * struct + sub-frame cycle remainder. The output scratch is transient (drained
 * every frame) and is reset on load. */
#include <stdio.h>
int psg_save_state(FILE *f)
{
    if (!s_inited) psg_init();
    if (fwrite(&s_sn, sizeof s_sn, 1, f) != 1) return 0;
    if (fwrite(&s_leftover_z80_cycles, sizeof s_leftover_z80_cycles, 1, f) != 1) return 0;
    return 1;
}

int psg_load_state(FILE *f)
{
    if (!s_inited) psg_init();
    if (fread(&s_sn, sizeof s_sn, 1, f) != 1) return 0;
    if (fread(&s_leftover_z80_cycles, sizeof s_leftover_z80_cycles, 1, f) != 1) return 0;
    s_scratch_read = s_scratch_write = 0;
    return 1;
}
