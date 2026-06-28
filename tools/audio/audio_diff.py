#!/usr/bin/env python3
"""Drift-tolerant audio diff for the SMS/GG recomp accuracy oracle.

Compares two WAVs (e.g. recomp `--audio-wav` @ 223721 Hz vs a Mesen reference
@ 48000 Hz) with metrics that tolerate sample-rate, resample and start-offset
differences -- because sample-exact is not realistic across independent
SN76489 implementations (different volume curve / output filter / period-0
convention; see SMS_GG_ACCURACY_BURNDOWN.md Axis 5 risks).

Metrics (all computed AFTER cross-correlation alignment):
  - lag            : integer-sample (and ms) offset that best aligns the clips
  - ncc            : normalized waveform cross-correlation at the aligned lag
                     (per channel + mono mix)
  - env_corr       : Pearson correlation of the short-window RMS *envelopes*
                     (captures dynamics / note timing, ignores phase)
  - onset_dt       : per-onset timing error histogram (median / IQR / matched%)
  - pitch_cents    : median |pitch error| in cents over voiced frames
A verdict (ALIGNED / DRIFT / DIVERGENT) is derived from env_corr + onset match.

Usage:
  python audio_diff.py REF.wav TEST.wav [--rate 48000] [--json out.json]
                       [--label-ref NAME] [--label-test NAME] [--max-lag-ms N]
"""
import argparse, json, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly
from math import gcd


def load_wav(path):
    sr, data = wavfile.read(path)
    if data.dtype == np.int16:
        x = data.astype(np.float64) / 32768.0
    elif data.dtype == np.int32:
        x = data.astype(np.float64) / 2147483648.0
    elif data.dtype == np.uint8:
        x = (data.astype(np.float64) - 128.0) / 128.0
    else:
        x = data.astype(np.float64)
    if x.ndim == 1:
        x = x[:, None]
    return sr, x  # (N, ch)


def to_rate(x, sr, target):
    if sr == target:
        return x
    g = gcd(int(sr), int(target))
    up, down = int(target) // g, int(sr) // g
    return resample_poly(x, up, down, axis=0)


def rms_envelope(mono, sr, hop_ms=5.0, win_ms=20.0):
    hop = max(1, int(sr * hop_ms / 1000))
    win = max(hop, int(sr * win_ms / 1000))
    n = 1 + max(0, (len(mono) - win) // hop)
    env = np.empty(n)
    for i in range(n):
        seg = mono[i * hop: i * hop + win]
        env[i] = np.sqrt(np.mean(seg * seg) + 1e-20)
    return env, hop


def best_lag(ref_env, test_env):
    """Integer-sample lag (in envelope frames) maximizing normalized xcorr."""
    a = ref_env - ref_env.mean()
    b = test_env - test_env.mean()
    if np.allclose(a, 0) or np.allclose(b, 0):
        return 0, 0.0
    corr = np.correlate(a, b, mode="full")
    norm = np.sqrt(np.sum(a * a) * np.sum(b * b)) + 1e-20
    corr = corr / norm
    k = int(np.argmax(corr))
    lag = k - (len(b) - 1)   # >0: test lags ref (shift test left / ref right)
    return lag, float(corr[k])


def ncc(a, b):
    a = a - a.mean(); b = b - b.mean()
    d = np.sqrt(np.sum(a * a) * np.sum(b * b)) + 1e-20
    return float(np.sum(a * b) / d)


def pearson(a, b):
    if len(a) < 2:
        return 0.0
    a = a - a.mean(); b = b - b.mean()
    d = np.sqrt(np.sum(a * a) * np.sum(b * b)) + 1e-20
    return float(np.sum(a * b) / d)


def detect_onsets(env, hop, sr, thresh_rel=0.15):
    """Onset frame indices: positive flux peaks above a relative threshold."""
    flux = np.diff(env, prepend=env[:1])
    flux[flux < 0] = 0
    if flux.max() <= 0:
        return np.array([], dtype=int)
    thr = thresh_rel * flux.max()
    cand = np.where((flux[1:-1] >= thr) &
                    (flux[1:-1] >= flux[:-2]) &
                    (flux[1:-1] > flux[2:]))[0] + 1
    times = cand * hop / sr
    return times


def onset_metrics(ref_t, test_t, tol_ms=30.0):
    if len(ref_t) == 0 or len(test_t) == 0:
        return dict(ref_onsets=int(len(ref_t)), test_onsets=int(len(test_t)),
                    matched=0, matched_pct=0.0, median_dt_ms=None, iqr_dt_ms=None)
    tol = tol_ms / 1000.0
    dts = []
    for t in ref_t:
        j = np.argmin(np.abs(test_t - t))
        dt = test_t[j] - t
        if abs(dt) <= tol:
            dts.append(dt)
    dts = np.array(dts)
    matched = len(dts)
    return dict(
        ref_onsets=int(len(ref_t)), test_onsets=int(len(test_t)),
        matched=int(matched),
        matched_pct=round(100.0 * matched / len(ref_t), 1),
        median_dt_ms=(round(float(np.median(dts)) * 1000, 2) if matched else None),
        iqr_dt_ms=(round(float(np.subtract(*np.percentile(dts, [75, 25]))) * 1000, 2)
                   if matched else None),
    )


def f0_track(mono, sr, frame=2048, hop=1024, fmin=55.0, fmax=2000.0):
    lo, hi = int(sr / fmax), int(sr / fmin)
    win = np.hanning(frame)
    out = []
    for s in range(0, len(mono) - frame, hop):
        seg = mono[s:s + frame] * win
        if np.sqrt(np.mean(seg * seg)) < 1e-3:
            out.append(0.0); continue
        ac = np.correlate(seg, seg, mode="full")[frame - 1:]
        ac0 = ac[0] + 1e-20
        region = ac[lo:hi]
        if len(region) == 0:
            out.append(0.0); continue
        k = int(np.argmax(region)) + lo
        if ac[k] / ac0 < 0.3:        # unvoiced
            out.append(0.0); continue
        # parabolic interpolation
        if 0 < k < len(ac) - 1:
            a, b, c = ac[k - 1], ac[k], ac[k + 1]
            denom = (a - 2 * b + c)
            k = k + 0.5 * (a - c) / denom if denom != 0 else k
        out.append(sr / k)
    return np.array(out)


def pitch_cents(ref_mono, test_mono, sr):
    rf = f0_track(ref_mono, sr); tf = f0_track(test_mono, sr)
    n = min(len(rf), len(tf))
    rf, tf = rf[:n], tf[:n]
    voiced = (rf > 0) & (tf > 0)
    if voiced.sum() < 4:
        return None, int(voiced.sum())
    cents = 1200.0 * np.log2(tf[voiced] / rf[voiced])
    return round(float(np.median(np.abs(cents))), 1), int(voiced.sum())


def analyze(ref_path, test_path, target_rate, max_lag_ms, lr, lt):
    sr_r, xr = load_wav(ref_path)
    sr_t, xt = load_wav(test_path)
    rate = target_rate or min(sr_r, sr_t)
    xr = to_rate(xr, sr_r, rate)
    xt = to_rate(xt, sr_t, rate)

    mono_r = xr.mean(axis=1)
    mono_t = xt.mean(axis=1)

    # coarse alignment on envelopes
    env_r, hop = rms_envelope(mono_r, rate)
    env_t, _ = rms_envelope(mono_t, rate)
    lag_frames, env_peak = best_lag(env_r, env_t)
    lag_samples = lag_frames * hop
    lag_ms = 1000.0 * lag_samples / rate
    if max_lag_ms and abs(lag_ms) > max_lag_ms:
        lag_samples = int(np.sign(lag_samples) * max_lag_ms * rate / 1000)
        lag_ms = 1000.0 * lag_samples / rate

    # apply lag: positive lag => test lags ref => drop lag from ref head
    if lag_samples >= 0:
        a = xr[lag_samples:]; b = xt
    else:
        a = xr; b = xt[-lag_samples:]
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    am, bm = a.mean(axis=1), b.mean(axis=1)

    # per-channel + mono NCC
    nch = min(a.shape[1], b.shape[1])
    chan_ncc = {("L", "R")[c] if nch == 2 else f"ch{c}":
                round(ncc(a[:, c], b[:, c]), 4) for c in range(nch)}
    mono_ncc = round(ncc(am, bm), 4)

    # envelope correlation on aligned overlap
    ea, hop2 = rms_envelope(am, rate)
    eb, _ = rms_envelope(bm, rate)
    m = min(len(ea), len(eb))
    env_corr = round(pearson(ea[:m], eb[:m]), 4)

    # onsets
    ons = onset_metrics(detect_onsets(ea[:m], hop2, rate),
                        detect_onsets(eb[:m], hop2, rate))

    # pitch
    pc, voiced = pitch_cents(am, bm, rate)

    # verdict -- keyed on envelope correlation (the robust drift-tolerant
    # discriminator). Onset match only TIGHTENS the ALIGNED gate; it can never
    # promote a low-env-corr pair out of DIVERGENT (dense onsets false-match).
    if env_corr >= 0.90 and ons["matched_pct"] >= 80:
        verdict = "ALIGNED"
    elif env_corr >= 0.60:
        verdict = "DRIFT"
    else:
        verdict = "DIVERGENT"

    return {
        "ref": {"path": ref_path, "label": lr, "rate": int(sr_r),
                "samples": int(len(xr)), "dur_s": round(len(xr) / rate, 3)},
        "test": {"path": test_path, "label": lt, "rate": int(sr_t),
                 "samples": int(len(xt)), "dur_s": round(len(xt) / rate, 3)},
        "compare_rate": int(rate),
        "alignment": {"lag_samples": int(lag_samples),
                      "lag_ms": round(lag_ms, 2),
                      "env_peak_ncc": round(env_peak, 4)},
        "ncc": {"mono": mono_ncc, **chan_ncc},
        "env_corr": env_corr,
        "onset": ons,
        "pitch_cents_median_abs": pc,
        "pitch_voiced_frames": voiced,
        "verdict": verdict,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref"); ap.add_argument("test")
    ap.add_argument("--rate", type=int, default=48000,
                    help="comparison sample rate (default 48000)")
    ap.add_argument("--max-lag-ms", type=float, default=2000.0)
    ap.add_argument("--label-ref", default="ref")
    ap.add_argument("--label-test", default="test")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    r = analyze(a.ref, a.test, a.rate, a.max_lag_ms, a.label_ref, a.label_test)

    print(f"\n  {r['ref']['label']:>10} : {a.ref}")
    print(f"  {r['test']['label']:>10} : {a.test}")
    print(f"  rates {r['ref']['rate']} vs {r['test']['rate']} Hz -> compared @ {r['compare_rate']} Hz")
    al = r["alignment"]
    print(f"  align  lag = {al['lag_samples']} samp ({al['lag_ms']} ms), env peak NCC = {al['env_peak_ncc']}")
    print(f"  NCC    mono={r['ncc']['mono']}  " +
          "  ".join(f"{k}={v}" for k, v in r['ncc'].items() if k != 'mono'))
    print(f"  env_corr = {r['env_corr']}")
    o = r["onset"]
    print(f"  onsets ref={o['ref_onsets']} test={o['test_onsets']} "
          f"matched={o['matched']} ({o['matched_pct']}%) "
          f"median_dt={o['median_dt_ms']}ms iqr={o['iqr_dt_ms']}ms")
    print(f"  pitch  median|err| = {r['pitch_cents_median_abs']} cents "
          f"({r['pitch_voiced_frames']} voiced frames)")
    print(f"  VERDICT: {r['verdict']}\n")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(r, f, indent=2)
        print(f"  wrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
