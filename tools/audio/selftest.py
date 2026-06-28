#!/usr/bin/env python3
"""Self-test for audio_diff.py: prove the drift-tolerant metric does what it
claims on signals with KNOWN ground truth, before trusting it on recomp output.

Cases:
  1. identical                  -> ALIGNED, lag~0, env_corr~1, pitch~0
  2. known time offset (+250ms) -> alignment recovers the offset
  3. native-rate resample       -> 223721 Hz copy vs 48000 Hz -> still ALIGNED
  4. pitch-shifted (+1 semitone) -> pitch_cents ~ 100, env still correlates
  5. unrelated signal           -> DIVERGENT
"""
import os, sys, tempfile
import numpy as np
from scipy.io import wavfile
import audio_diff as ad

SR = 48000


def make_song(sr=SR, seed=0, pitch_factor=1.0):
    """A little SN76489-flavored square-wave melody with clear note onsets.
    pitch_factor scales every note frequency while keeping durations identical
    (a true pitch shift, no time/tempo change)."""
    notes = [262, 330, 392, 523, 392, 330, 294, 330]  # C E G C5 G E D E
    notes = [f * pitch_factor for f in notes]
    out = []
    for f in notes:
        n = int(sr * 0.25)
        t = np.arange(n) / sr
        sq = np.sign(np.sin(2 * np.pi * f * t)) * 0.3
        env = np.minimum(1.0, np.minimum(np.arange(n), n - np.arange(n)) / (0.02 * sr))
        out.append(sq * env)
    sig = np.concatenate(out)
    return np.stack([sig, sig], axis=1)  # stereo, L==R (SMS)


def write(path, sr, x):
    wavfile.write(path, sr, (np.clip(x, -1, 1) * 32767).astype(np.int16))


def pitch_shift_semitones(x, n):
    # crude resample-based shift then crop/pad to original length
    from scipy.signal import resample
    factor = 2 ** (n / 12.0)
    y = resample(x, int(len(x) / factor), axis=0)
    if len(y) < len(x):
        y = np.pad(y, ((0, len(x) - len(y)), (0, 0)))
    return y[:len(x)]


def run():
    d = tempfile.mkdtemp(prefix="audiodiff_selftest_")
    song = make_song()
    ref = os.path.join(d, "ref.wav"); write(ref, SR, song)

    ok = True

    # 1 identical
    p = os.path.join(d, "same.wav"); write(p, SR, song)
    r = ad.analyze(ref, p, 48000, 2000, "ref", "same")
    c1 = r["verdict"] == "ALIGNED" and abs(r["alignment"]["lag_ms"]) < 6 and r["env_corr"] > 0.95
    print(f"[1] identical            verdict={r['verdict']:9} lag={r['alignment']['lag_ms']}ms env={r['env_corr']}  {'OK' if c1 else 'FAIL'}")
    ok &= c1

    # 2 known +250 ms offset (silence prepended to test)
    off = int(0.250 * SR)
    shifted = np.concatenate([np.zeros((off, 2)), song], axis=0)
    p = os.path.join(d, "off.wav"); write(p, SR, shifted)
    r = ad.analyze(ref, p, 48000, 2000, "ref", "off250")
    # test lags ref by 250ms -> recovered lag_ms ~ -250 (ref must shift right)
    c2 = abs(abs(r["alignment"]["lag_ms"]) - 250) < 15
    print(f"[2] +250ms offset        recovered lag={r['alignment']['lag_ms']}ms (expect ~|250|)  {'OK' if c2 else 'FAIL'}")
    ok &= c2

    # 3 native-rate (223721 Hz) resample of the SAME song vs 48k ref
    from scipy.signal import resample_poly
    from math import gcd
    g = gcd(223721, SR)
    nat = resample_poly(song, 223721 // g, SR // g, axis=0)
    p = os.path.join(d, "native.wav"); write(p, 223721, nat)
    r = ad.analyze(ref, p, 48000, 2000, "ref48k", "native223k")
    c3 = r["verdict"] == "ALIGNED" and r["env_corr"] > 0.95
    print(f"[3] 223721Hz vs 48000Hz  verdict={r['verdict']:9} env={r['env_corr']}  {'OK' if c3 else 'FAIL'}")
    ok &= c3

    # 4 pitch +1 semitone (frequencies scaled, durations identical)
    p = os.path.join(d, "pitch.wav")
    write(p, SR, make_song(pitch_factor=2 ** (1 / 12.0)))
    r = ad.analyze(ref, p, 48000, 2000, "ref", "pitch+1")
    pc = r["pitch_cents_median_abs"]
    c4 = pc is not None and 70 < pc < 140
    print(f"[4] +1 semitone          pitch_cents={pc} (expect ~100)  {'OK' if c4 else 'FAIL'}")
    ok &= c4

    # 5 unrelated noise
    p = os.path.join(d, "noise.wav")
    write(p, SR, np.random.default_rng(7).standard_normal(song.shape) * 0.3)
    r = ad.analyze(ref, p, 48000, 2000, "ref", "noise")
    c5 = r["verdict"] == "DIVERGENT"
    print(f"[5] unrelated noise      verdict={r['verdict']:9} env={r['env_corr']}  {'OK' if c5 else 'FAIL'}")
    ok &= c5

    print(f"\nSELFTEST: {'ALL PASS' if ok else 'FAILURES PRESENT'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(run())
