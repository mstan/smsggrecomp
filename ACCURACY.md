# SMS / Game Gear Recompiler — Accuracy Record

The canonical, revisitable record of **what accuracy we claim, how we measured
it, and what's outstanding.** Read this first; `SMS_GG_ACCURACY_BURNDOWN.md` is
the detailed 7-axis scorecard with dated findings, and `accuracy/*.md` hold the
deep-dives. Everything here was produced 2026-06-28 and merged to `main`.

---

## 1. The claim (and its precise scope)

> Over the **exercised path** — power-on + the self-running attract/demo, ~40 s —
> the recompiled output of **Sonic the Hedgehog (SMS)** and **Sonic Blast (GG)**
> matches accurate reference emulators across all 7 accuracy axes, validated
> against **two independent oracles** (Mesen 2 and Genesis Plus GX).

**What "exercised path" means, exactly:** boot through the attract/demo loop with
no input (plus a hand-played confirmation by the maintainer). It is **NOT**
whole-game, played-to-completion validation. Code paths only reached deeper in
real gameplay are covered by *static recompilation completeness* (0 dispatch
misses) and the *interpreter twin*, but were not differentially measured against
an external oracle frame-by-frame. Claims should always be quoted with this
scope.

**The GREEN bar we held ourselves to:** an axis is only "validated" when it is
both (a) cross-referenced against a reference (hardware docs / emulator source)
**and** (b) runtime-diffed against an accurate oracle on the relevant state
surface. Self-agreement (recomp == our own interpreter) is necessary but **never
sufficient** — both can be identically wrong.

---

## 2. Results — 7 axes

| Axis | Verdict | Oracle | Key evidence |
|---|---|---|---|
| 1 Instruction semantics | ✅ validated | Mesen + GPGX | VRAM/CRAM byte-identical (below) ⇒ ALU/flags/addressing correct; `R` modeled; `WZ` accepted-benign |
| 2 Cycle / timing | ✅ validated | Mesen | per-anchor Δcycle = **jitter-only, no net drift** (−247 cyc / 1797 frames); steady 59,736 cyc/frame matches to the cycle |
| 3 Interrupt timing | ✅ validated | Mesen | (with cycle) IRQ-accept jitter **±8.6 cyc/frame, self-cancelling**; HW-correct accept contract |
| 4 Memory / MMIO | ✅ validated | Mesen + GPGX | VRAM (16 KB) + CRAM byte-identical at static/fade/**active-scroll** frames; `$3E/$3F` never written; no DMA (correct) |
| 5 Peripherals / audio | ✅ validated | GPGX | system audio ALIGNED (env 0.976 SMS / 0.90 GG); isolated chip-math ALIGNED (env 0.977, 4.8¢) |
| 6 Static-vs-dynamic | ✅ validated | (intrinsic + GPGX) | 100% static / 0 dispatch misses; byte-identical VDP ⇒ recomp == hardware-correct CPU |
| 7 Determinism | ✅ validated | (intrinsic) | 3 headless runs **byte-identical** (SHA256) |

**The load-bearing measurement** is VRAM/CRAM byte-identicality: the VDP memory
is produced entirely by the CPU executing the game. If any instruction, flag,
cycle-driven upload, or MMIO write were wrong, the game's logic would diverge and
VRAM would differ. It does not — byte-for-byte, vs **two independent emulators**,
at static, transition, and actively-scrolling frames, on both titles. That single
fact underwrites Axes 1, 4, and 6.

---

## 3. The oracles

- **Mesen 2** (2.1.1, GPLv3) — cycle-accurate SMS/GG. Used for **cycle + state**
  via headless `--testRunner` Lua (`emu.getState().cpu.cycleCount`, VRAM/CRAM).
  Audio is GUI-only (no Lua sample API / no `--recordAudio`), so Mesen is the
  *cycle/state* oracle, not the audio oracle. Staged at `_diag/emu/Mesen/`
  (not committed).
- **smsref / Genesis Plus GX** — a headless GPGX build we wrote
  (`tools/smsref/`) that loads a ROM and exposes VRAM/CRAM/Z80/PSG via a dump
  CLI, a TCP/JSON server, and a PSG-replay synth. Used for **state + audio**
  (the headless audio Mesen couldn't give). GPGX source lives in `smsref-ext/`
  (external clone, **never linked into the shipped runner** — license + size).

Two independent emulators (different codebases) agreeing byte-for-byte is the
strongest form of the GREEN gate; a single-oracle agreement could be a shared
bug.

---

## 4. How to reproduce (re-verify the claims)

All builds use mingw-w64 gcc (the recompiler builds with gcc too — no MSVC
needed). `R` is the workspace root `F:\Projects\smsggrecomp`.

**Build the recompiler + regenerate a game:**
```
gcc -O1 -w -I smsggrecomp/recompiler/src smsggrecomp/recompiler/src/*.c -o SmsRecomp.exe
cd <Game>; SmsRecomp.exe <rom> --game game.toml      # writes generated/
```

**Build the headless runner (per game):** see `runner-build-run` recipe —
`gcc -DSMSGG_HAVE_GAME_LAYOUT -I runner -I runner/include -I runner/video -I <gen>
  runner/{main,glue,video/sms_vdp,audio/sn76489,external/superzazu/z80}.c
  <gen>/<prefix>_{full,dispatch,layout}.c -o <Game>.exe`.

**Build smsref (GPGX oracle):**
```
git clone --depth 1 https://github.com/ekeeke/genesis-plus-gx smsref-ext/genesis-plus-gx
smsggrecomp/tools/smsref/build.ps1      # -> tools/smsref/smsref.exe
```

**Reproduce each axis:**
- **VDP (Axes 1/4/6):** `<Game>.exe <rom> --frames N --dump-frame F --dump-out x.png`
  (emits `x.png.vram/.cram/.cpu`); `smsref.exe <rom> --frames N+10 --dump "F" --out g`;
  `python tools/oracle/vdp_diff.py x.png.vram x.png.cram g_F.vram g_F.cram` → MATCH.
- **Cycle (Axes 2/3):** build with `-DSMS_CYC_WATCH`, `--cyc-watch 0x0038 r.csv`;
  `Mesen.exe --testRunner tools/oracle/mesen_cyc_watch.lua <rom>` → m.csv;
  `python tools/oracle/cyc_compare.py r.csv m.csv` → JITTER-ONLY.
- **Audio (Axis 5):** `<Game>.exe <rom> --frames 2400 --audio-wav r.wav`;
  `smsref.exe <rom> --frames 2400 --wav g.wav`;
  `python tools/audio/audio_diff.py g.wav r.wav` → ALIGNED. Isolated chip-math:
  `--psg-log ring.csv` then `smsref --replay-psg ring.csv --wav` vs `synth_ours`.
- **Determinism (Axis 7):** run `--vdp-trace d{1,2,3}.csv` 3× → identical SHA256.

The drift-tolerant audio metric is self-tested: `python tools/audio/selftest.py`
(recovers known offset/pitch, sees through the 223721↔48000 Hz resample, rejects
unrelated audio).

---

## 5. Outstanding / known gaps / caveats

- **Whole-game validation** — NOT done. Only the exercised path is
  oracle-diffed. The biggest honest gap.
- **±8.6 cyc IRQ jitter vs Mesen** — deliberately NOT chased. The recomp already
  accepts the IRQ at the hardware-correct instruction boundary; the residual is
  frame-boundary placement *between two emulators*, not a recomp error. Reducing
  it would be emulator-matching (rejected by PRINCIPLES). It has zero net drift.
- **Per-scanline raster** — the renderer is a full-frame snapshot. Fine for the
  tested titles (write-ring proved 0 active-display VDP writes), but a game with
  mid-frame raster effects would need a per-scanline renderer.
- **H-counter** (`$7F`) — implemented as a clean-room sub-line approximation, not
  the exact per-cycle table (GPGX's is license-encumbered; neither tested title
  reads `$7F`). V-counter (`$7E`) jump-back IS exact (both titles read it).
- **`$3E`/`$3F`** ports unmodeled — confirmed never written by the tested titles;
  a real gap only for BIOS-dependent / `$3E`-gating ROMs.
- **`WZ`/MEMPTR** undoc X/Y flags on `BIT n,(HL)` — accepted-benign (masked;
  byte-identical VDP proves unobservable). Drive it only if a title reads it.
- **zexall/zexdoc** not run — would need a CP/M BDOS harness + recompiling a
  non-SMS ROM. The byte-identical-VDP-vs-two-oracles evidence covers the
  exercised opcodes instead.
- **GG CRAM dump tooling** — smsref de-strides GG's 64-byte 12-bit CRAM as if SMS
  (32 B); the GG correctness signal is therefore VRAM (which IS byte-identical).
- **Audio residual** — env 0.90–0.98 / ~5¢, attributable to clean-room-vs-GPGX
  synthesis micro-differences (volume curve, LFSR, output filter), NOT logic.

### Rejected (kept for the record)
- **period-0 → 0x400 "fix"** — REJECTED as a would-be regression. GPGX uses the
  discrete-chip 0x400 only for SG-1000; SMS/SMS2/GG use the integrated 0x1 ==
  our clamp-to-1. Sonic Blast GG writes period-0 at audible volume yet stays
  ALIGNED with GPGX, empirically confirming clamp-to-1 is correct. Lesson:
  validate *which oracle branch applies to our platform* before changing code.

---

## 6. Provenance (key commits on `main`)

```
769c18f vdp: real V-counter jump-back + sub-line H-counter
ad30885 recompiler: model Z80 R refresh register (closes Axis 1 R gap)
508758d smsref(Phase C): synth_gpgx isolated chip-math diff = ALIGNED
03f622e audio: period-0 fix REJECTED (would regress); Sonic Blast GG ALIGNED
eb8df7f audio: system audio recomp-vs-GPGX = ALIGNED; period-0 inaudible
9763a1a accuracy(mmio): Axis 4 GREEN - VDP byte-identical vs Mesen
8674f68 accuracy(cycle): per-anchor recomp-vs-Mesen diff -> jitter-only
e882c59 accuracy: 7-axis burndown + audio diff tooling + Mesen cycle oracle
        (+ smsref Phase A/B, measurement sweep, GG cross-title — see git log)
```

Real bugs found + fixed this effort: the Z80 `R` register (unmodeled → modeled),
a sample-drop bug in `synth_ours`. One would-be regression caught and rejected
(period-0). The methodology is modeled on the psxrecomp tomba2 7-axis template.
</content>
