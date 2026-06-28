# Axis 5 (Audio) — deep-dive + oracle slice

Companion to `SMS_GG_ACCURACY_BURNDOWN.md`. The first active axis. Stomping
ground: Sonic the Hedgehog (SMS), SN76489 PSG, mono (L==R on SMS).

## The chip + the recomp tap

- **SN76489** (`runner/audio/sn76489.c`): 3 square + 1 LFSR noise, 4-bit log
  volume, 16-bit LFSR taps {0,3}→bit15. Clocked Z80/16 → **223,721 Hz native**.
  Advanced inside `advance_vdp()` (`glue.c:615-618`) — the single VDP-timing
  choke shared by recomp/hybrid/interp — with the sub-frame cycle remainder
  carried exactly (`sn76489.c:171-173`). No FM (YM2413) — out of scope.
- **Deterministic tap:** `--audio-wav out.wav` → S16/stereo @ native 223,721 Hz,
  no resample (`runner/main.c`). Headless, reproducible, diffable. This is the
  recomp side of every audio diff. Capture:
  ```
  Sonic1SmsAcc.exe sonicthehedgehog.sms --frames 1800 --audio-wav recomp_static.wav
  Sonic1SmsAcc.exe sonicthehedgehog.sms --frames 1800 --interp --audio-wav recomp_interp.wav
  ```

## The metric — drift-tolerant (validated)

`tools/audio/audio_diff.py`: resample both clips to a common rate (default
48 kHz), envelope-cross-correlate to align, then report — all AFTER alignment:
- **lag** (samples/ms) + envelope-peak NCC,
- **NCC** of the waveform (per channel + mono) — expected LOW across independent
  PSG implementations (see ceiling below); informational, not the verdict key,
- **env_corr** — Pearson of the short-window RMS envelope (the robust verdict
  key: dynamics / note timing, phase-independent),
- **onset_dt** — per-onset timing histogram (median/IQR/matched%),
- **pitch_cents** — median |pitch error| over voiced frames.

Verdict (keyed on env_corr; onsets only tighten ALIGNED, never rescue a low
env_corr — dense onsets false-match): `ALIGNED` (env≥0.90 & onset≥80%) /
`DRIFT` (env≥0.60) / `DIVERGENT` (else).

**Self-test (`tools/audio/selftest.py`) — ALL PASS (2026-06-28):**
| case | expected | got |
|---|---|---|
| identical | ALIGNED, lag~0, pitch 0 | ALIGNED, 0 ms, 0 cents ✓ |
| +250 ms offset | recover ~250 ms | −250.0 ms ✓ |
| **223721 Hz vs 48000 Hz** (the real rate gap) | ALIGNED | ALIGNED, env 1.0 ✓ |
| +1 semitone | ~100 cents | 100.0 cents ✓ |
| unrelated noise | DIVERGENT | DIVERGENT, env 0.31 ✓ |

The metric provably recovers known offsets/pitch, sees through the 223721↔48000
resample (critical, since that's exactly the recomp↔Mesen gap), and rejects
unrelated audio.

## First real diff (2026-06-28): static recomp vs interp twin

`recomp_static.wav` vs `recomp_interp.wav`, 1800 frames (30 s), both 223,721 Hz.
Both runs: 0 dispatch misses, 100% static, irq/frame 1.00.
SHA256 **differ** (not byte-identical).

```
align lag = 0 ms,  env peak NCC = 0.9964
NCC   mono = 0.4631  (L=R=0.4631)
env_corr = 0.9964
onsets ref=136 test=115 matched=115 (84.6%) median_dt=0.0ms iqr=5.0ms
pitch median|err| = 1.0 cents (860 voiced frames)
VERDICT: ALIGNED
```
Native-rate probe (`tools/audio/_probe_static_interp.py`):
```
raw native NCC = 0.4631;  differing stereo frames = 57.2%
RMS(diff)/RMS(ref) = 1.0358;  best ±50-sample lag = -9 -> NCC 0.4819
first differing frame = 108599  (t = 0.485 s)
```

**Interpretation.** For two square trains of the *same* frequency and energy,
`RMS(diff)/RMS = √(2(1−NCC))`; 0.46 NCC ⇒ 1.04, matching the measured 1.036.
So the two paths emit the **same notes at the same pitch (≤1 cent) and same
envelope (0.996)** but with **decorrelated square-wave phase** — accumulating
jitter (a global lag only lifts NCC 0.46→0.48), onset at t≈0.49 s. This is the
audible-null signature of the documented **sub-instruction PSG-write timing
jitter** (Burndown Axis 3, ±1-insn IRQ sampling: a PSG `OUT` lands a few
T-states apart between the pre-pay recomp tick and superzazu's post-pay tick,
drifting square phase while preserving register values).

**Consequences:**
1. **Bit-exact is off the table even internally** — our own two paths reach
   only 0.46 waveform NCC. The drift-tolerant metric is mandatory, not a
   convenience. A bit-exact gate would have false-flagged identical music.
2. This is a **self-agreement** result → does NOT close the GREEN gate. It
   validates the tool on real game audio and sets the realistic ceiling for the
   Mesen diff (judge on env/pitch/onset; expect waveform NCC ≤ ~0.5).
3. The t≈0.49 s phase-divergence onset is a reusable hook: if the Axis-3 jitter
   is ever tightened (sub-line VDP stepping), this NCC should rise — a cheap
   regression signal.

## GREEN-gate slice (open) — Mesen 2 reference

Mesen 2 is GPLv3, SMS/GG cycle-accurate, but has **no Lua audio API and no
`--recordAudio` CLI** — audio is GUI-only. So the system-level audio reference
needs ONE human GUI capture (everything else is automated). Staged at
`_diag/emu/Mesen/`.

**Deterministic capture recipe (Sonic 1 SMS, boot+attract, no input):**
1. Launch `Mesen.exe`. Settings → Audio: **Sample Rate = 48000**, disable
   equalizer/effects (raw mix). Settings → Emulation: region = NTSC.
2. Load `sonicthehedgehog.sms`. Tools → Movies → **Record** → `s1_attract.mmo`.
   Sit with **no input** for 30 s (1800 frames @ 60 Hz). Stop. (The `.mmo` pins
   startup + input + timing → byte-stable replays.)
3. Tools → Sound Recorder → **Record**; Tools → Movies → **Play** `s1_attract.mmo`;
   on movie end, Sound Recorder → **Stop** → `s1_mesen.wav` (48 kHz stereo PCM).
4. Diff:
   ```
   python tools/audio/audio_diff.py s1_mesen.wav recomp_static.wav \
     --label-ref mesen --label-test recomp --json mesen_vs_recomp.json
   ```
   SMS = PSG-only, so Mesen's full mix ≈ pure SN76489 → a fair reference.

**Cycle leg (headless — VALIDATED 2026-06-28):**
`Mesen.exe --testRunner tools/oracle/mesen_trace.lua sonicthehedgehog.sms --noaudio --doNotSaveSettings`
→ per-frame `cycleCount` CSV (`%USERPROFILE%\Documents\Mesen2\LuaScriptData\mesen_trace\mesen_cyc.csv`).
First external cycle checkpoint, 1800 frames: recomp 107,524,801 vs Mesen
107,478,745 (+0.043%); Mesen steady Δ=59,736/frame = recomp's 228×262 exactly →
steady rate matches to the cycle, the gap is boot-offset only (Mesen frame-1
varies run-to-run; use a `.mmo` for boot determinism). Next: `cyc_compare.py`
per-anchor Δ. This is the GREEN (external) leg for the Axis-2 gross rate.

## Risk-driven next steps (post-GREEN)
1. **Period-0 (Axis 5 Risk #1):** `sn76489.c:112,90` clamps tone period 0→1;
   HW = 0x400. Build/borrow a clip that writes period 0; if Mesen's WAV shows a
   low/near-silent tone where ours shows max-frequency, that's a real bug — fix
   in the recompiler synthesis, not the output. Validate OUTPUT-first.
2. Volume curve (Risk #2), output LPF (Risk #3), LFSR reseed policy (Risk #4) —
   each only matters if the Mesen diff exposes it; do not pre-emptively rewrite.
3. GG (Sonic Blast): stereo `$06`, expect L≠R; re-run the slice.
</content>
