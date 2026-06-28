/*
 * sn76489.h — cycle-stamped SN76489 PSG wrapper API (SMS/GG).
 *
 * Named after the chip rather than "psg" to avoid a filename collision on the
 * include path. The PSG is clocked off the Z80 (advance takes Z80 T-states);
 * the renderer is stereo so the Game Gear's per-channel L/R routing register
 * ($06) is honoured. On the SMS the stereo mask stays all-on (0xFF), so the
 * two output channels are an identical mono signal.
 */
#ifndef AUDIO_SN76489_H
#define AUDIO_SN76489_H

#include <stdint.h>
#include <stddef.h>

void   psg_init(void);

/* Advance the PSG by `cycles_z80` Z80 T-states, synthesising whole stereo
 * frames into the internal scratch (one frame per 16 T-states). Sub-frame
 * remainder is accumulated, so calling this with small deltas is exact. */
void   psg_advance(uint32_t cycles_z80);

/* PSG data / latch byte (SMS port $7F, mirror $40-$7F). */
void   psg_write(uint8_t value);

/* Game Gear stereo register (port $06): high nibble = left enable for
 * tone0/tone1/tone2/noise, low nibble = right enable for the same. Default is
 * 0xFF (every channel on both sides). SMS never writes this. */
void   psg_write_stereo(uint8_t mask);

/* Drain up to `frame_count` STEREO frames into `out` (interleaved L,R — `out`
 * must hold 2*frame_count int16). Returns the number of frames written. */
size_t psg_render(int16_t *out, size_t frame_count);
size_t psg_frames_available(void);

uint32_t psg_sample_rate(void);

/* Reset the sub-frame Z80-cycle remainder (per-run sync). */
void   psg_reset_leftover(void);

/* Save-state hooks (own-backend snapshots). Returns 1 on success. */
#include <stdio.h>
void psg_dump_state_text(FILE *f);   /* measurement: labeled latched-register dump */
int psg_save_state(FILE *f);
int psg_load_state(FILE *f);

#endif
