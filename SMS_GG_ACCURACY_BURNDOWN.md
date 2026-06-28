# SMS / Game Gear Recompiler — Accuracy Burndown

The master 7-axis accuracy scorecard for `smsggrecomp` (SMS + Game Gear, one
engine + a platform flag). Modeled on the psxrecomp tomba2 worktree's
`ACCURACY_BURNDOWN.md` methodology. Companion deep-dive docs land in
`accuracy/<axis>.md` as each axis goes active. Audio (Axis 5) is the first
active axis — see `accuracy/audio.md` + `tools/audio/`.

Stomping-ground title: **Sonic the Hedgehog (SMS)** — the byte-exact,
0-dispatch-miss baseline. Game Gear (Sonic Blast) is the second pass.

---

## ⚠️ LESSONS (append-only, dated)

- **2026-06-28 — Self-agreement is not GREEN.** `sonic_audio.wav` (static
  recomp) matching `sonic_interp.wav` (our superzazu interp path) proves
  *backend equivalence*, NOT correctness — both run the same clean-room
  SN76489 over the same bus, so both can be identically wrong (e.g. the
  period-0 clamp, §Axis 5 risk #1). A GREEN audio item requires an
  **external accurate oracle** (Mesen 2), not our own interp.
- **2026-06-28 — The accurate oracles have no headless audio.** Mesen 2 has
  **no Lua audio-sample API and no `--recordAudio` CLI** (verified against
  `LuaDocumentation.json` + `CommandLineHelper.cs`); audio capture is GUI
  Sound Recorder + deterministic movie (`.mmo`) playback only. Emulicious is
  likewise GUI-only for WAV. So the *system-level* audio oracle leg requires
  one human GUI capture per reference clip; everything downstream (diff,
  metric, recomp tap) is automated. This is an environment constraint, not a
  methodology shortcut.
- **2026-06-28 — Output validation before code change.** A claimed
  source-level discrepancy (e.g. "our LFSR reseeds on data writes, MAME
  doesn't") is a HYPOTHESIS. Diff the OUTPUT against the oracle on the same
  input before touching `sn76489.c`. "Looked right before, wrong after" ⇒
  revert.

---

## Method (non-negotiable)

Every item gets a **status**, the **external comparative(s)** to cross-ref it
against, and a **validation method**. "Looks good" is not a status. An item is
only **GREEN** once it is BOTH:

1. **Cross-referenced against a reference** — SMS dev wiki / VDP & Z80 HW docs
   (smspower.org), an accurate emulator's *source* (Mesen 2 GPLv3, Genesis
   Plus GX), or a hardware test ROM; cite the section/file. AND
2. **Runtime-validated against an accurate oracle** — diff the running native
   build against **Mesen 2** (state/cycle via `--testRunner` Lua
   `emu.getState().cpu.cycleCount`; audio via GUI/movie WAV) on the relevant
   **state surface** (VRAM/CRAM hash, audio samples, guest cycles, IRQ
   timeline) via first-divergence.

`compiled == our-interp` is **necessary, not sufficient**. The static recomp
and the superzazu interp are a deterministic twin pair — invaluable for
first-divergence narrowing (Axis 6), but their agreement proves codegen
equivalence, never hardware correctness. Both legs of the gate must be cited
on the item line.

**Output-validation corollary:** validate OUTPUT vs oracle on the *same input*
BEFORE changing recompiler/runtime code. A research claim is a hypothesis until
the output diff confirms it.

**Ring-buffer-first (PRINCIPLES #17, #22):** all observation queries always-on
rings (`g_enter_ring`, the VDP-write ring `vdp_write_obs`, `--vdp-trace` FNV
hash, the `[exec]`/`[timing]` counters). Never arm-then-capture; never
pause/step to synchronize two observers. Seed any frame-gated capture
(`SMS_*_FRAME`, `--press`) at boot, not at probe time.

---

## Comparative sources (the reference shelf)

| Source | Role | How to invoke |
|---|---|---|
| **Mesen 2** (GPLv3, `SourMesen/Mesen2`, fork `nesdev-org/MesenCE`) | PRIMARY oracle: state + cycle (Lua `--testRunner`), audio (GUI/movie WAV @ 48 kHz) | `Mesen.exe --testRunner script.lua rom.sms`; audio via Tools→Sound Recorder + `.mmo` |
| **Mesen 2 source** | cross-ref for Z80 timing, VDP, SN76489 synthesis | read `Core/SMS/*` |
| **Genesis Plus GX** (`ekeeke/genesis-plus-gx`) | 2nd-opinion source; has SMS FM (YM2413) reference | read `core/sound/sn76489.c` etc. |
| **smspower.org dev wiki** | canonical SMS/GG HW reference (VDP, ports, mapper, PSG) | cite page/section per item |
| **Emulicious** (closed, Java; installed `_diag/emu/`) | SECONDARY visual cross-check; GUI WAV/video record | `_diag/emu/Emulicious/Emulicious.exe` |
| **superzazu z80** (in-tree, MIT) | Z80 semantic reference + our interp twin | `runner/external/superzazu/z80.c` |
| **Z80 HW test ROMs** (zexall/zexdoc) | instruction-semantics ground truth | run through runner + oracle |

---

## Validation infrastructure to BUILD

- [x] Recomp deterministic audio tap — `--audio-wav` S16/stereo @ 223,721 Hz
  (`runner/main.c`). Native PSG rate, no resample. **Exists.**
- [x] Drift-tolerant audio diff — `tools/audio/audio_diff.py`
  (cross-correlation alignment + onset histogram + per-channel pitch error +
  envelope correlation). Resamples both to a common rate. **Built 2026-06-28.**
- [x] Diff self-test — metric recovers a known injected offset and reports ~0
  drift on identical input. **Built 2026-06-28** (`tools/audio/selftest.py`).
- [x] Mesen state/cycle oracle — `tools/oracle/mesen_trace.lua` per-frame
  `cycleCount` CSV via headless `--testRunner`. **Validated 2026-06-28**
  (Mesen 2.1.1 staged at `_diag/emu/Mesen/`; .NET 8/9 present). VRAM/CRAM hash
  still TODO. First external cycle checkpoint recorded (Axis 2).
- [ ] Mesen audio capture recipe — deterministic `.mmo` + GUI Sound Recorder;
  documented in `accuracy/audio.md`. (Needs the 1 human GUI step — Mesen has no
  Lua audio API / no `--recordAudio`.)
- [x] `cyc_compare.py` + recomp `--cyc-watch ADDR file` (general arbitrary-PC
  cycle-watch via the `SMS_PC` hook, `-DSMS_CYC_WATCH`, zero-cost when off) +
  `mesen_cyc_watch.lua` (Mesen `cpuExec` callback). Diffs the offset-independent
  per-anchor Δcycle. **Built 2026-06-28.**
- [ ] `mmio_tally.py` — histogram I/O-port writes per frame (Axis 4 surface).
- [ ] `build_instruction_coverage.py` — every opcode in the ROM × translator
  emit-coverage, with `code_generator.c` line cites (Axis 1 machine-checkable).
- [ ] VRAM/CRAM first-divergence harness recomp↔Mesen (extend `--vdp-trace`).

---

## Axis 1 — Instruction semantics (Z80 decoder + ALU/flags)

**Status: STRONG.** Instruction-accurate, HW-correct including undocumented
behavior. No stubs (an untranslatable opcode is a hard `cg_fail`,
`code_generator.c`; STATUS.md:30-44).

- [x] Full ISA decode (base/CB/ED/DD/FD/DDCB/FDCB) — `z80_decoder.c`;
  self-test `tests/z80_decoder_selftest.c`. Cross-ref: superzazu `z80.c`. ED
  4-byte length bug fixed (`decoder-ed-4byte-length`).
- [x] Flag semantics S/Z/Y/H/X/P/N/C packed F (`sms_runtime.h:48-57`); ALU
  ported from superzazu (`z80_ops.h`); self-test
  `tests/z80_ops_selftest.c`. Cross-ref: superzazu.
- [x] Undocumented DD/FD half-registers (IXH/IXL/IYH/IYL), DDCB/FDCB
  (`code_generator.c:595-606`).
- [ ] **GREEN gate not met:** validated only vs our superzazu twin
  (self-agreement). Need external oracle. Lever/validation:
  - Cross-ref: run **zexall/zexdoc** through the runner; cross-ref against a
    documented pass list (smspower / Mesen).
  - Oracle: compare zex CRC results vs Mesen 2 running the same test ROM.
- [ ] WZ/MEMPTR-driven X/Y flags for `BIT n,(HL)`/`(IX+d)` — currently masked
  in the differential harness (`glue.c:898-899`, mask `0x28`); `g_z80.wz`
  exists (`sms_runtime.h:35`) but is not driven by codegen.
  **Lever:** drive WZ in `code_generator.c` for the BIT-on-memory forms.
  Cross-ref: smspower undoc-flags note; oracle: zexall MEMPTR sub-tests.

---

## Axis 2 — Cycle / timing

**Status: SCANLINE-ACCURATE (per-instruction T-states; VDP advanced per
228-T line).** Not cycle-accurate at sub-instruction resolution. ← the active
tightening target.

Model: per-insn T-states from NMOS `base_cyc[256]` (`code_generator.c:246-264`)
+ prefix tables (`:266-298`) + conditional taken-extras (DJNZ+5 `:678`, JR/JP
cc, CALL cc+7 `:690`, RET cc+6 `:661`), accumulated via `sms_tick(N)` emitted
before each body (`:749`). VDP advanced line-by-line in **`advance_vdp()`
(`glue.c:609-624`)**, the single choke point; deadline normally the next line
boundary (`:633-635`).

- [x] Per-instruction T-state table present + several HW-correct fixes:
  `LD (IX/IY+d),n` = 19 not 22 (`code_generator.c:289-294`); block ops tick
  per-iteration 21/16 (`:533`); IM1 accept = 13 T (`glue.c:572`). Cross-ref:
  Z80 timing tables (smspower / Sean Young Z80 undoc).
- [ ] **VDP sub-line state not modeled** — `vdp_hcounter()` returns constant 0
  (`sms_vdp.c:98`); V-counter is a monotonic line approximation with no
  mid-frame jump-back (`:91-96`). GAP vs cycle-accurate.
  **Lever (cross-cutting #1):** step the VDP to the exact `s->cyc` in
  `advance_vdp` with a real H-counter + V-counter jback. Tightens Axes 2/3/4
  at once.
  - Cross-ref: smspower VDP timing (H/V counter tables, 228 T/line, 256/342
    pixel-clocks).
  - Oracle: `cyc_compare.py` Δcycle-per-anchor recomp↔Mesen
    (`emu.getState().cpu.cycleCount`).
- [~] **External gross-rate checkpoint vs Mesen (2026-06-28).** Recomp
  (`tools/oracle/mesen_trace.lua` headless) vs Mesen 2.1.1, Sonic 1 SMS, 1800
  frames: recomp **107,524,801** Z80 cyc vs Mesen **107,478,745** (+0.043%).
  Mesen steady per-frame Δ = **59,736** (min 59,730 / max 59,742); recomp =
  228×262 = **59,736** exactly. So the steady cycle RATE matches to the cycle;
  the 0.043% total is purely a boot/frame-1 offset (Mesen frame-1 varies
  13,681↔58,825 run-to-run — its testRunner boot isn't cycle-deterministic
  without a `.mmo`). This is an EXTERNAL-oracle agreement, not self-agreement.
- [x] **Per-anchor Δcycle vs Mesen — DONE 2026-06-28.** Anchor = IM1/VBlank
  handler `0x0038`, 1798/1800 hits. Median Δ = **59,736 on both**; max Δ =
  **79,206 identical**; **cumulative diff = −247 cyc over 1797 frames ≈ 0 net
  drift** (−0.14 cyc/frame). The +0.043% gross total is thus CONFIRMED pure
  boot-offset, not rate drift. Verdict: **JITTER-ONLY** (oscillates, no drift).
  `_diag/accuracy/cyc0038_compare.json`.

---

## Axis 3 — Interrupt / event timing

**Status: SCANLINE-LATCHED, instruction-boundary accepted; HW-correct
acceptance contract.** Known ±1-instruction jitter is documented + architectural.

Model: frame IRQ latches at line 192 (`sms_vdp.c:114-117`); line IRQ via reg10
down-counter underflow (`:100-121`); `vdp_irq_asserted()` gates on enable bits
(`:123-126`); status read clears (`:82-89`). Accept: `sms_sync()` takes IRQ when
asserted && `iff1` && `!ei_block` (`glue.c:640`); `take_irq()` charges 13 T,
pushes real interrupted PC `g_dbg_pc` (`:585`), C-calls 0x0038 (`:602`).
EI-delay via `ei_block` (`sms_runtime.h:121-127`). Poll-deadline `cyc+1` while
`/INT` asserted (`glue.c:633-635`). Sync-first tick ordering
(`sms_runtime.h:111-120`).

- [x] Acceptance contract (EI-delay, 13-T cost, poll-deadline, sync-first)
  cross-ref'd to superzazu `z80_gen_int` + real-HW "/INT sampled on final
  T-state."
- [ ] **±1-instruction per-frame jitter vs interp** from frame 45
  (`STATUS.md:435-445`) — IRQ sampled one insn off in edge cases because the
  tick CALL must precede control-flow bodies. Mostly self-cancelling.
  **Lever:** sub-line IRQ latch (the Axis-2 cross-cutting fix) — accept at the
  exact T-state the flag latches.
  - Cross-ref: smspower line-IRQ timing.
  - Oracle: Mesen IRQ event timeline (Lua `addEventCallback(... irq)`) vs our
    enter-ring at the IRQ.
- [x] **External-oracle jitter bound (2026-06-28).** The recomp's IRQ-accept
  timing vs Mesen at anchor `0x0038`: **mean |Δ-diff| = 8.6 cyc/frame** (0.014%
  of a 59,736-cyc frame), diff histogram centered+symmetric on 0, **no net
  drift** (cumulative −247 over 1797). So the ±1-insn jitter (architectural,
  STATUS.md:435-445) is now QUANTIFIED against an accurate oracle, not just the
  interp twin — it is small and self-cancelling, not a divergence.
  **Lever to drive it to 0:** sub-line IRQ latch (accept at the exact T-state
  the line-192 flag latches, the Axis-2 cross-cutting fix), not instruction
  alignment. Tool: `cyc_compare.py` + `mesen_cyc_watch.lua`.

---

## Axis 4 — Memory / MMIO

**Status: INSTRUCTION-ACCURATE functional model; Sega + Codemasters mappers;
no DMA (correct — SMS/GG have none).**

Model (`glue.c`): map `$0000-$03FF` fixed bank 0, slots at `$0400/$4000/$8000`,
`$C000+` 8 KB RAM mirrored (`sms_read8:313-319`); Sega frame regs `$FFFC-$FFFF`
(`sms_write8:343-349`), Codemasters `$0000/$4000/$8000` (`:353-357`). Ports
(`sms_io_*:365-405`): VDP `$BE/$BF`, `$7E` V-ctr / `$7F` H-ctr + PSG, pads
`$DC/$DD`, GG `$00` START + `$06` stereo. VRAM/CRAM auto-increment +
control-port latch modeled (`sms_vdp.c:29-72`).

- [x] Sega + Codemasters mappers; RAM mirror; bank-aware dispatch
  (`sms_slot_bank:361`). Cross-ref: smspower mapper docs.
- [x] No DMA — correct by omission (CPU-OUT-driven VRAM fill). Cross-ref:
  smspower (SMS has no DMA engine).
- [x] **`$3E`/`$3F` gap CONFIRMED moot for Sonic 1 SMS (2026-06-28).** The
  always-on `[mmio]` port tally (`glue.c`, `sms_io_out/in`) over 1800 frames:
  OUT ports used = `7F`(PSG)/`BE`(VDP data)/`BF`(VDP ctrl); IN = `7E`/`BF`/`DC`/
  `DD`. **`$3E.out=0 $3F.out=0`** — never written. So the unmodelled ports are
  not exercised by this title (a real gap only for BIOS-dependent / `$3E`-gating
  titles; note for cross-title). All ports the game DOES use are modeled.
- [x] **GREEN runtime leg — VDP state-surface byte diff vs Mesen (2026-06-28).**
  Recomp raw `.vram`/`.cram` (`--dump-frame`) vs Mesen `mesen_vdp_dump.lua`.
  **3 frames byte-identical (VRAM 16384/16384 + CRAM 32/32):** static title
  (450), fade transition (1200, disp=0), and **active-scrolling gameplay (1600,
  r8/r9 hscroll/vscroll live)** — all MATCH at the aligned frame. The metric is
  sharp: recomp-1600 vs Mesen-**1599** shows exactly 14 differing bytes in the
  sprite attribute table (`0x3F24+`) = the 1-frame-old sprite positions, while
  vs Mesen-**1600** it is byte-identical. So the recompiled CPU's `$BE/$BF` port
  writes land the exact bytes the cycle-accurate oracle does, even mid-scroll —
  *tighter* than the recomp-vs-interp drift. Tools:
  `tools/oracle/{mesen_vdp_dump.lua,vdp_diff.py}`.

---

## Axis 5 — Peripherals incl. AUDIO  ← FIRST ACTIVE AXIS

**Status: AUDIO = T-state-accurate clean-room SN76489 (deterministic
divider/LFSR); input correct; no other peripherals (none on platform).**
See `accuracy/audio.md` for the deep-dive + the live oracle slice.

Model: SN76489 (`runner/audio/sn76489.c`) — 3 square + 1 LFSR noise, 4-bit log
volume, 16-bit LFSR taps {0,3}→bit15, GG `$06` stereo mask; clocked Z80/16 →
**223,721 Hz native**; advanced inside `advance_vdp()` (`glue.c:615-618`) so
audio is identical across recomp/hybrid/interp; sub-frame remainder carried
exactly (`sn76489.c:171-173`). Output: `--audio-wav` S16/stereo @ native rate
(`main.c`), deterministic; live SDL path resamples 223.7k→48k.

- [x] Recomp deterministic audio tap (`--audio-wav`).
- [x] Drift-tolerant diff tool + self-test (`tools/audio/`, 2026-06-28).
- [x] Recomp↔interp twin audio diff (real game audio; backend-equiv check —
  NOT GREEN, self-agreement). First slice, 2026-06-28.
- [DEFERRED — user decision 2026-06-28] External **audio** GREEN slice (recomp
  `--audio-wav` vs Mesen WAV) is **not pursued**: even our two internal paths
  reach only 0.46 waveform NCC (sub-insn PSG jitter), and an independent
  emulator (different volume curve / LPF / period-0 convention) is only
  drift-comparable — not precise enough to be worth the GUI-only capture. The
  drift-tolerant tool is retained as a gross-regression check; the active axis
  pivots to **Cycle (Axis 2)**, where the Mesen oracle IS accurately diffable.
  (Re-openable via the headless GPGX synthesis-isolation oracle if a precise
  chip-math diff is ever wanted.)
- [ ] **Risk #1 — period-0 handling.** `sn76489.c:112,90` clamps tone period 0
  → 1; real SN76489 treats written 0 as **0x400**. Audible if any driver writes
  period 0. **Highest-risk synthesis defect.** Cross-ref: smspower PSG /
  Maxim's SN76489 notes; oracle: Mesen WAV on a period-0 test.
- [ ] Risk #2 — volume curve is an ideal 2 dB/step table (`:62-65`); real chip
  + other emus deviate per-step → absolute amplitude won't be sample-exact.
- [ ] Risk #3 — output LPF is a clownmdemu Mega-Drive IIR (`:129-139`), not an
  SMS/GG RC measurement; stateful across the run.
- [ ] Risk #4 — LFSR reseeds to 0x8000 on every noise-control AND latch write
  (`:85,91`); noise phase may decorrelate vs an oracle with a different reseed
  policy. Treat noise as a separate, lower-confidence track.
- [ ] No YM2413 FM (SMS-JP add-on) — out of scope for these export/GG titles;
  note only. Genesis Plus GX has a reference if ever needed.

GG note: default `gg_stereo=0xFF` ⇒ L==R on SMS (`sn76489.c:74`); on GG, games
write `$06` and L≠R is expected — do not assert L==R for GG diffs.

---

## Axis 6 — Static-vs-dynamic recompiler fidelity

**Status: STRONG.** Near-100% static on exercised paths; behavior-identical
interp twin; invariant-based differential harness.

Triad: static recompiled C (bulk); `hybrid_interpret` (`glue.c:746-812`) runs
unresolved computed targets under superzazu over the live bus until RET past
entry frame (`sp>entry_sp` + CALL/RET depth, the line-IRQ-freeze fix
`:735-744`); `glue_run_interp` (`:1091-1112`) pure-superzazu oracle. `[exec]`
split + dispatch manifest → Sonic1 SMS & SonicBlast GG both 100% static / 0
misses over the demo (`dispatch-manifest-profile-guided-discovery`).

- [x] Differential function harness (regs+RAM+**cyc** from a frozen snapshot,
  VDP/IRQ frozen) — `sms_diff_enter` (`glue.c:925-991`); found+fixed 4
  control-flow mistranslations (`STATUS.md:409-433`).
- [x] Backend equivalence (compiled == interp) — necessary, NOT sufficient.
- [ ] **Hybrid path cycle-fidelity soft spot:** a routine running entirely
  inside one non-returning hybrid call doesn't sample IRQs at native
  instruction boundaries identically. **Lever:** shrink hybrid hot set to 0 on
  every reached path (GOAL = 100% static). Oracle: Mesen guest-cycle diff over
  a hybrid-covered routine.
- [ ] GREEN gate: the diff harness is self-referential (recomp vs our interp).
  Anchor at least one function's exit cyc vs Mesen.

---

## Axis 7 — Determinism

**Status: STRONG.** Byte-deterministic headless runs; no wall-clock/rand in the
sim loop.

- [x] Full state init in `glue_init` (`glue.c:1065-1089`); LFSR seeded
  deterministically (`sn76489.c:73`). Wall-clock only in the SDL host pacer
  (presentation-only, absent headless).
- [x] `--vdp-trace` per-frame FNV-1a64 hash of VRAM/CRAM/regs/RAM+SP
  (`glue.c:99-104,420-431`), identical in `glue_run` and `glue_run_interp` —
  the always-on determinism oracle. Cross-ref: re-run reproducibility.
- [ ] GREEN gate: determinism is intrinsic; the external leg is "Mesen movie
  replay is also deterministic" — confirm the recomp `--vdp-trace` is stable
  across 3 runs (trivially true; record the evidence line).

---

## Research findings (dated)

### 2026-06-28 — Initial 7-axis recon (this session)
- Cross-cutting **single biggest lever = sub-line VDP stepping** in
  `advance_vdp`: advance to exact `s->cyc` with real H/V counters. Tightens
  Axis 2 (scanline→cycle), Axis 3 (erases ±1 jitter), Axis 4 (H-counter), and
  unblocks any future per-scanline raster effect (renderer is currently a
  single full-frame snapshot, `sms_vdp.c:157-238`, acceptable only because the
  inspected GHZ frames had zero active-display reg/CRAM writes — proven via the
  VDP-write ring, `STATUS.md:233-237`).
- **Oracle posture:** Mesen 2 = GREEN-grade for state+cycle (headless Lua),
  GUI-only for audio. Emulicious = secondary visual. Genesis Plus GX =
  2nd-opinion source + YM2413 reference. Mesen NOT installed (corrupt zip);
  re-download `2.1.1`.
- **Audio P0:** period-0 clamp (Risk #1) is the most likely real synthesis bug;
  prioritize a Mesen WAV diff on a clip that exercises it.
- **Mesen headless cycle oracle CONFIRMED working** (`--testRunner` Lua →
  per-frame `cycleCount` CSV); first external cycle checkpoint = +0.043% gross,
  exact steady-rate match (Axis 2). Audio remains GUI-gated (no Lua audio API).
- **First audio diff (recomp static vs interp twin):** pitch ≤1 cent, env_corr
  0.996, but waveform NCC 0.46 (phase-decorrelated by the Axis-3 ±1-insn PSG
  jitter; RMS(diff)/RMS = 1.036 = √(2·(1−0.46))). Self-agreement → not GREEN,
  but it sets the bit-exact ceiling: drift-tolerant is mandatory. See
  `accuracy/audio.md`.

### 2026-06-28 — MMIO axis (Axis 4): VDP state-surface GREEN, $3E/$3F gap moot
- Port tally over 1800 frames: Sonic 1 SMS uses only `7F/BE/BF` (out) +
  `7E/BF/DC/DD` (in); **`$3E`/`$3F` never written** → the unmodelled-port gap is
  not exercised by this title (note only for cross-title / BIOS-dependent ROMs).
- VDP state-surface byte diff vs Mesen at 3 frames (title 450, fade 1200,
  scroll 1600): **VRAM 16 KB + CRAM 32 B byte-identical at every aligned frame.**
  The recompiled CPU's MMIO writes reproduce the cycle-accurate oracle's VDP
  memory exactly, even mid-scroll. **Axis 4 GREEN for the exercised path.**
  (Caveat #23: representative frames, not every frame; the tool is reusable for
  any frame / any title.)

### 2026-06-28 — Cycle axis: first per-anchor recomp-vs-Mesen diff (GREEN-leg)
- Anchor `0x0038` (IM1/VBlank), 1800 frames: median Δ **59,736 both**, max Δ
  **79,206 both**, **net drift ≈ 0** (−247 cyc / 1797 frames), residual = ±8.6
  cyc/frame oscillating IRQ-accept jitter. The recomp's interrupt timing tracks
  the accurate oracle with no systematic divergence. **Axis 3 strengthened to an
  external-validated bound.**
- **Mesen Lua API gotcha (this build, MesenCE 2.1.1):** the exec callback enum
  is **`emu.callbackType.exec` (=2)**, NOT the docs' `emu.memCallbackType`
  (which is `nil` here). Cycle field = `emu.getState().cpu.cycleCount`
  (= `masterClock`, Z80 cycles). `emu.cpuType.sms=10`, `emu.memType.smsMemory=11`.
  Mesen also exposes `cpu.wz` (MEMPTR) — useful for the future Axis-1 WZ item.
  Probe recipe in `tools/oracle/_mesen_probe.lua`.

---

## Cross-cutting closer

Every validation is a ring-buffer diff of the running native build against an
accurate oracle on the relevant state surface — VRAM/CRAM FNV hash (video),
sample stream (audio), guest cycles per anchor (timing), IRQ timeline
(interrupts), port-write histogram (MMIO). The recomp↔interp twin narrows
*first divergence*; **Mesen closes the GREEN gate.** Self-agreement is never
sufficient.

---

## Phasing

(Re-ordered 2026-06-28: external audio oracle deferred — pivot to the
accurately-diffable cycle axis. Audio keeps its drift-tolerant gross-regression
tool only.)

1. ~~**Cycle (Axis 2/3)**~~ — DONE 2026-06-28. Per-anchor Δ vs Mesen =
   JITTER-ONLY, no net drift. Sub-line VDP stepping remains the optional lever
   to drive the ±8.6-cyc residual to 0 (also makes Axis 2 cycle-accurate).
2. ~~**MMIO (Axis 4)**~~ — DONE 2026-06-28. VDP VRAM+CRAM byte-identical vs
   Mesen across 3 frames incl. active scroll; `$3E/$3F` confirmed unused. GREEN
   for the exercised path.
3. **Instruction (Axis 1) — NEXT.** zexall/zexdoc through runner + Mesen;
   `build_instruction_coverage.py` (every ROM opcode × translator emit);
   drive WZ for the masked BIT n,(HL) X/Y flags (Mesen exposes `cpu.wz`).
4. **Audio (Axis 5)** — keep the drift-tolerant diff as a gross-regression
   gate; external oracle DEFERRED (see Axis 5). Revisit only via the headless
   GPGX synthesis-isolation oracle if precise chip-math diffing is wanted.
5. **Cross-title** — repeat the green slices on Sonic Blast (GG).
6. **Determinism (Axis 7)** — record the 3-run stability evidence; keep
   `--vdp-trace` always-on.
</content>
</invoke>
