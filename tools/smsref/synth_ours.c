/* synth_ours — replay a recomp PSG chip_ring (cyc,val) through OUR clean-room
 * SN76489 and write a WAV. Isolates synthesis from CPU timing (both this and a
 * future GPGX-psg.c reference get the IDENTICAL register stream), so the diff
 * is drift-tolerant via tools/oracle/audio_diff.py. The "ours" half of
 * synth_replay; pair with synth_gpgx (GPGX psg.c) for the independent ref.
 *
 *   synth_ours psg_ring.csv out.wav
 *
 * Links runner/audio/sn76489.c. Native rate = 223721 Hz S16 stereo.
 */
#include "sn76489.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void wav_hdr(FILE *f, uint32_t rate, uint32_t nframes)
{
    uint32_t bytes = nframes * 4; uint32_t ch = 2, bits = 16;
    uint32_t byterate = rate * ch * bits / 8; uint16_t blockalign = ch * bits / 8;
    fwrite("RIFF", 1, 4, f); uint32_t r = 36 + bytes; fwrite(&r, 4, 1, f);
    fwrite("WAVE", 1, 4, f); fwrite("fmt ", 1, 4, f);
    uint32_t fmtlen = 16; fwrite(&fmtlen, 4, 1, f); uint16_t pcm = 1; fwrite(&pcm, 2, 1, f);
    uint16_t c16 = (uint16_t)ch; fwrite(&c16, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&byterate, 4, 1, f); fwrite(&blockalign, 2, 1, f);
    uint16_t b16 = (uint16_t)bits; fwrite(&b16, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: synth_ours psg_ring.csv out.wav\n"); return 2; }
    FILE *in = fopen(argv[1], "r"); if (!in) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    FILE *wav = fopen(argv[2], "wb"); if (!wav) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }

    psg_init();
    psg_reset_leftover();
    uint32_t rate = psg_sample_rate();
    wav_hdr(wav, rate, 0);            /* patch size at end */

    static int16_t buf[4096 * 2];
    uint64_t total = 0, prev_cyc = 0;
    char line[128]; int first = 1;
    while (fgets(line, sizeof line, in)) {
        if (line[0] == 'c') continue;            /* header */
        char *comma = strchr(line, ','); if (!comma) continue;
        uint64_t cyc = strtoull(line, NULL, 10);
        int val = atoi(comma + 1);
        if (first) { prev_cyc = cyc; first = 0; }
        /* Advance in bounded chunks and drain after each — a single psg_advance
         * over a large inter-write gap would render far more than the internal
         * scratch (8192 frames) holds and drop samples. */
        uint64_t delta = cyc - prev_cyc; prev_cyc = cyc;
        while (delta > 0) {
            uint32_t chunk = delta > 32768 ? 32768 : (uint32_t)delta;   /* <=2048 frames */
            psg_advance(chunk); delta -= chunk;
            size_t n;
            while ((n = psg_render(buf, 4096)) > 0) { fwrite(buf, 4, n, wav); total += n; }
        }
        psg_write((uint8_t)val);
    }
    /* tail: render ~0.3s so trailing tones decay (chunked, same scratch limit) */
    for (uint32_t left = rate / 3 * 16; left > 0; ) {
        uint32_t chunk = left > 32768 ? 32768 : left; psg_advance(chunk); left -= chunk;
        size_t n; while ((n = psg_render(buf, 4096)) > 0) { fwrite(buf, 4, n, wav); total += n; }
    }

    fseek(wav, 0, SEEK_SET); wav_hdr(wav, rate, (uint32_t)total); fclose(wav); fclose(in);
    fprintf(stderr, "synth_ours: %llu frames @ %u Hz -> %s\n",
            (unsigned long long)total, rate, argv[2]);
    return 0;
}
