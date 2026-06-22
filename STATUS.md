# STATUS — smsggrecomp bring-up

Snapshot of what exists, what's verified, and what comes next. This is a
brand-new project scaffolded to mirror `segagenesisrecomp`'s structure.

## Decision: one repo, both platforms

SMS and Game Gear share the Z80 CPU, the SN76489 PSG, and a register-identical
VDP. GG differs only in viewport crop, palette depth, PSG stereo, and a few
system ports (PRINCIPLES #25). So this is **one engine + a `platform` flag**,
not two repos. No `ggrecomp`.

## Built and VERIFIED (compiles + self-tests pass)

| Component | File(s) | Verification |
|-----------|---------|--------------|
| Project structure + discipline docs | `PRINCIPLES.md`, `CLAUDE.md`, `README.md`, `DEBUG.md` | — |
| Vendored TOML parser | `recompiler/src/toml.{c,h}` | from genesis, unchanged |
| Vendored Z80 interpreter (MIT) | `runner/external/superzazu/` | reference + hybrid path |
| Clean-room SN76489 PSG | `runner/audio/sn76489.{c,h}` | wired + verified — Z80-clocked, stereo, plays Sonic 1 SMS music (see PSG audio §) |
| **SMS/GG ROM parser** | `recompiler/src/rom_parser.{c,h}` | TMR SEGA footer, region→platform, size, CRC32, Sega/Codemasters mapper |
| **Z80 instruction decoder** | `recompiler/src/z80_decoder.{c,h}` | `tests/z80_decoder_selftest.c` — ALL PASS (length/prefix/CF/target across base, CB, ED, DD/FD, DDCB) |
| **Z80 semantic core** (flags/ALU/rotates/DAA) | `runner/include/z80_ops.h` + `sms_runtime.h` | `tests/z80_ops_selftest.c` — ALL PASS; logic ported from superzazu |
| **Function finder + tracer** | `recompiler/src/function_finder.{c,h}` | `tests/frontend_selftest.c` — ALL PASS (synthetic ROM → decode → discover entries transitively + internal labels) |

The genuinely hard, correctness-critical CPU front-end (decode + flag
semantics + reachability) is done and tested.

## Code generator — DONE (cg_emit), compiles clean

`code_generator.c::cg_emit` translates every discovered function to C over the
full Z80 ISA (base/CB/ED/DD/FD/DDCB/FDCB) — NO stubs (an untranslatable opcode
is a hard `cg_fail` abort, PRINCIPLES #12). Built on the verified `z80_ops.h`
semantic core. Per `SmsRecomp.exe --game …` it emits into the game's
`generated/`:

- `<prefix>_full.c` — one C function per (bank,addr) entry; intra-function
  jumps → `goto L_<addr>`, tail jumps → `callee(); return;`, CALL pushes the
  Z80 return address + C-calls (RET pops + `return;`, so `s->sp` stays the
  authentic Z80 SP), computed/unknown transfers → `call_by_address()`, HALT →
  `sms_halt()`, per-insn T-states into `s->cyc`.
- `<prefix>_dispatch.c` — `<prefix>_lookup(addr)` address→function table.
- `<prefix>_layout.c` — `g_game_layout` (guarded; awaits runner's struct).
- `<prefix>_funcs.h` — prototypes + lookup decl.

Verified for **SonicTheHedgehogSMS**: 168 functions, ~5.9 MB, no `cg_fail`;
`tests/gen_compile_harness.c` (stub bus/IO) compiles + links `_full.c` +
`_dispatch.c` warning-clean (`-Wall -Wextra`). A function whose trace contains
code below its entry now emits `goto L_<entry>` so execution starts correctly.
NOT yet run (no runner) — correctness is end-to-end-unverified per #23.

## Runner core — DONE (headless), RUNS Sonic 1 SMS

A headless runner links the generated C and executes the game:

- `runner/include/sms_clocks.h` — NTSC timing (228 T-states/line, 262 lines,
  VBlank latch at line 192).
- `runner/video/sms_vdp.{c,h}` — mode-4 VDP register/VRAM/CRAM + line/frame
  interrupt timing (no pixel rendering yet — the loop needs interrupts, not
  pixels). SMS + GG share it (GG only differs in 12-bit CRAM).
- `runner/glue.{c,h}` — `g_z80`, Sega/Codemasters ROM mapper + 8 KB RAM, bus
  read/write, I/O ($BE/$BF/$7E/$7F PSG/controllers; PSG now wired, GG $06 stereo plumbed),
  `sms_sync` (advance VDP + take IM1 IRQ when IFF1), `sms_halt`,
  `sms_dispatch_miss` → `dispatch_misses.log`, frame-limit longjmp shutdown.
- `runner/game_spec.h` + `runner/game_layout.h` — per-game contract structs
  (`_layout.c` now compiles under `-DSMSGG_HAVE_GAME_LAYOUT`).
- `runner/main.c` — load ROM → `glue_run()` (enters reset via
  `call_by_address(0x0000)`; reset never returns; interrupts injected inline by
  the per-instruction `sms_tick` poll).

**Interrupt model (mirrors genesis/NES):** generated code calls `sms_tick(N)`
after each instruction; on reaching the next VDP event it runs `sms_sync`,
which advances scanlines, latches frame/line interrupts, and calls the IM1
handler (0x0038) when IFF1 is set. Verified: links warning-clean, **runs Sonic 1
SMS for 1200 frames in <1 s**, frames advance correctly.

### Banking — DONE; dispatch-miss loop EMPTY for Sonic 1 SMS

An always-on **function-entry ring** (`g_enter_ring`) pinpointed the 4 misses as
**slot-0/1 bank switching the finder didn't track** (e.g. `func_02ED`:
`ld a,$03; ld ($FFFE),a; call $4006` pages bank 3 into slot 1 → real code at ROM
0xC006; the finder mis-read 0x4006 as bank-1 data and the garbage produced the
RAM computed jumps). Fix shipped across finder + codegen + runtime:

- **Finder** (`BankState` in `function_finder.{c,h}`): tracks all 3 slots via the
  `ld a,#n; ld ($FFFD/$FFFE/$FFFF),a` idiom; `trace_function` reads each address
  under its slot's bank; `ff_discover` assigns each target the correct
  `(slot,bank)`.
- **Codegen**: `emit_function` tracks the same `BankState`; `name_for_target`
  binds direct CALL/JP to the right banked function (e.g. `func_4006_b3`).
- **Dispatch**: `call_by_address` is bank-aware — addresses with multiple bank
  variants `switch (sms_slot_bank(addr))` (e.g. `0x77BE` → bank 1/3/5).

Result: discovery **168 → 198 entries** (`b3=20`, `b5=44`, `b2=3`); garbage
`func_400x` gone; **0 dispatch misses over 6000 frames** (~100 s game time),
runs in <1 s. Frontend self-test still passes; all builds warning-clean.

Caveat (#23): zero misses means every reached code target resolves to a
generated function — a strong correctness signal, but NOT visual/behavioural
verification (no video yet). The game executes deep into banked logic (bank-5
subsystem) but is not confirmed to render correctly.

## Video + the decoder bug - Sonic 1 SMS RENDERS

Mode-4 renderer (`runner/video/sms_vdp.c::vdp_render_frame`: background tilemap
+ scroll, sprites, SMS/GG CRAM palette) + a vendored `png_write.h` + a
`--dump-frame`/`--dump-out` PNG path. First render was black; a per-instruction
PC breadcrumb (`-DSMS_TRACE_PC`) + the IRQ-interrupted-PC capture + watching SP
drift down each frame localised a stall to a corrupted bank-3 VInt routine.

**Root cause = a decoder bug.** `z80_decode` gave EVERY `ED` instruction
length 2, but `ED 43/4B/53/5B/63/6B/73/7B` (`LD (nn),rp` / `LD rp,(nn)`,
`(op&0xC7)==0x43`) are 4 bytes - mis-length cascaded into garbage that spun and
leaked the Z80 stack. Fixed (capture imm, length `i+4`); the decoder self-test
had a GAP here. Discovery 198->117 entries (the bogus `b5=44` set was pure
misalignment artifact). See [[decoder-ed-4byte-length]] memory.

**Result:** the recompiled game now uploads graphics (VRAM ~8.6 KB, CRAM 30
colours) and renders the recognisable **"GREEN HILL" map screen**
(`runner/forced_300.png`, captured with `--force-display`). This is the first
real end-to-end visual confirmation.

### Boots to the TITLE SCREEN on its own

The art decompressor's computed jump (`jp (hl)` at bank-3 `$450A`) goes through a
16-bit pointer table at bank-3 `$4529` (14 entries `$45AE..$46D1`) the static
finder can't resolve. Added a ROM-truth `[jump_tables]` directive to game.toml
(`addr/bank/count` parallel arrays); the recompiler reads the actual pointers
and seeds them as banked entries (`FUNC_SRC_TABLE`), parsed in `main_sms.c`.

Result: discovery 117->131 entries, **0 dispatch misses**, the game
**self-enables display** (`disp=1`) and renders the **Sonic 1 title screen**
pixel-correctly (`runner/self_300.png`). First fully self-driven visible output.

### Self-drives boot -> title -> map -> in-level (GHZ)

Two more recompiler advances got it through the intro:
- **`[jump_tables]` directive** (game.toml `addr/bank/count`, read from ROM in
  `main_sms.c`): resolves computed-jump TARGET tables the static finder can't,
  e.g. the title art decompressor table bank-3 `$4529` (14 ptrs).
- **Computed-call idiom** (`trace_computed_call` in `function_finder.c`): the
  Z80 fakes `call (hl)` as `ld rr,ret; push rr; jp (hl)`. The recompiler now
  follows/continues at the pushed return instead of `return;`-ing, fixing the
  stack-leak stall in the script/level loader. See [[computed-call-via-jp-hl]].

Visual milestones (self-driven, no input): **title screen** (`runner/title.png`,
pixel-correct), **GREEN HILL map** (`runner/map.png`, correct colours), and
**in-level Green Hill Zone** geometry (`runner/level.png`, force-displayed
mid-fade). The recompiled game runs its real intro + gameplay code.

Caveat (#23): NOT verified correct in motion. ~54 dispatch misses remain
(gameplay/script handlers + multi-bank tables like `$2AF6` no-op), level colours
look fade/palette-off, display flickers across transitions, and a long run
(frame ~6000) crashed. So it *reaches* gameplay graphics; it does not yet *play*
a clean attract demo.

### superzazu hybrid fallback — DONE

`sms_dispatch_miss()` (runner `glue.c`) no longer no-ops an unresolved computed
target — it interprets the routine with the vendored superzazu Z80 over the
SAME bus/IO hooks (`sms_read8/write8/io_in/io_out`), so it sees the **live
`g_bank` mapping**, until the routine RETs past its entry frame
(`sp > entry_sp`, the call-vs-tail-robust stop condition), then syncs register
state back and lets the recompiled caller continue. Timing is driven from the
interpreter's cycle count via a shared `advance_vdp()` helper (factored out of
`sms_sync`); IM1 interrupts are delivered inside the interpreter via
`z80_gen_int` (so HALT-wait-for-VBlank works). State is synced both ways
(`state_to_hz`/`state_from_hz`, packed-F ↔ superzazu bool flags); superzazu's
32-bit `cyc` is rebased per call to avoid overflow. The dispatch-miss log is
kept as the static-analysis worklist (PRINCIPLES #16). No recompiler change /
no regen — runner rebuild only (adds `external/superzazu/z80.c` to the link).

Result (SonicTheHedgehogSMS, self-driven attract, no input): the **~54 misses
collapse to 8** distinct script-engine targets (all from `func_32C8`, the
multi-bank script/level loader: `$48C8 $7AED $6E0C $693F $5B09 $65EE $7B95
$6AC1`), and every one is handled by the hybrid. **No crash through 7000
frames** (the prior ~frame-6000 crash was the stack leak from no-op'd misses —
gone). Visual: pixel-correct **title screen** (`runner/hybrid_3000.png`) and
**Green Hill Zone gameplay** with correct palette + HUD (`runner/hybrid_6000.png`).
The blue/teal "fade" colours reported before were a mid-fade capture; the
palette renders correctly now.

Caveat (#23): NOT user-verified in motion. A colourful band in the upper GHZ
background remains — most likely a renderer limitation (full-frame snapshot, no
per-scanline raster split for the water/mountain parallax), not a CPU issue
(sprites + palette are correct); verify after the SDL window / raster work.

### SDL host window — DONE (build verified; live watch awaits user, #23)

Optional SDL2 live display, fully decoupled from the runner core:
`runner/host_sdl.{c,h}` (streaming ARGB texture, vsync-paced, Esc/close to
quit, integer-scaled with letterbox, GG 160x144 crop). glue.c stays
SDL-agnostic via `glue_set_frame_callback()`: `frame_completed()` renders +
presents every frame through the registered callback; the window-close return
unwinds the run loop like the frame limit. `main.c` gains `--window [scale]`
(runs until the window closes unless `--frames` is also given). Built with
`-DSMS_HAVE_SDL` + `-lSDL2` (SDL_MAIN_HANDLED, so our `main()` stays); headless
build is unchanged and omits the TU. Both configs build warning-clean
(`-Wall -Wextra`). Smoke-tested 200 windowed frames: SDL init OK, window opens,
clean exit, no crash. `SDL2.dll` is copied next to `Sonic1SmsWindow.exe`.

### Frame timing — MEASURED CORRECT (the ~3x over-count is gone)

Added always-on timing counters (glue.c): `g_frame` (VDP frame boundaries ==
real VBlanks), `g_irq_taken` (IM1 handler runs), `g_irq_reentrant`,
`g_sync_maxdepth`; a one-line `[timing]` report prints at shutdown. Measured at
600 / 3000 / 6000 frames: **`irq/frame = 1.00`, reentrant = 0, sync_maxdepth =
2** (the expected main→ISR nesting) at every point — title AND gameplay. So
`g_frame` tracks real VBlanks 1:1; the previously-reported ~3x over-count was a
symptom of the pre-hybrid stack corruption (no-op'd misses leaking SP) and the
hybrid fix resolved it. The SDL host presents once per `g_frame` under vsync —
which only equals realtime on a **60 Hz** display; on a higher-refresh monitor
the loop runs at `refresh/60 ×` speed (see Known issues #1).

### GHZ background band — recomp/palette/VRAM all PROVEN CORRECT; no demonstrable bug from internal evidence

Closed the oracle diff (2026-06-21). NOTE: an intermediate conclusion of "shared
palette bug" was WRONG and was corrected by a ROM check (below) - the palette is
loaded verbatim from ROM. Built three reusable instruments and ran them:
- **`--vdp-trace <csv>`** (glue.c) — per-frame FNV hash of VRAM/CRAM/regs, always-on
  from frame 0, identical in `glue_run` and `glue_run_interp`.
- **VDP-write ring** (glue.c `vdp_write_obs` + `vdp_set_write_observer` in sms_vdp.c) —
  always-on ring of every register/CRAM write tagged with the scanline; replayed for
  the just-finished frame at the dump frame.
- One-shot nametable / CRAM / tile-pattern dump at the dump frame (frame_completed).

Findings (SonicTheHedgehogSMS, frame 6000, GHZ gameplay):
1. **NOT a recomp/CPU bug.** Per-frame state diff recomp vs `--interp`: VRAM matches
   96.8 % of 6200 frames, CRAM 99.5 %; mismatches are transient upload-timing drift
   that re-converges. At the band frames (6000/6090/6200) VRAM **and** CRAM **and** all
   VDP regs are **byte-identical** and the dumped PNGs share a SHA256. The recompiled
   CPU (incl. its hybrid fallback) produces the same VRAM/CRAM as a clean MIT Z80.
2. **NOT a raster effect.** The write-ring shows only ~4 reg/CRAM writes in the whole
   frame, **all during vblank, ZERO during active display** — no mid-frame hscroll or
   palette cycling. The single-snapshot renderer is faithful here. (The earlier
   "line-ints off" claim was sampled at end-of-frame = unsound; the ring is the sound
   probe and confirms no raster anyway.)
3. **Palette is CORRECT (verbatim from ROM).** The band is a background scenery element
   (tiles `0FC..0FF`, nametable palette bit = 0 → palette 0) whose pixels use colour
   indices 13/14/15 = yellow/black/white. Searching the ROM, our **entire live 32-byte
   CRAM is byte-identical to the ROM palette table at 0x629E** (palette-0 + the sprite
   palette at 0x62AE both match exactly), so idx13=yellow is what the game intends (a
   real GHZ palette colour, e.g. for flowers). The recompiled + interp runs both load
   this table faithfully.

**Net:** the band renders from a CORRECT palette (verbatim ROM), CORRECT VRAM (matches
the reference interpreter) with no raster and a standard mode-4 decoder — so the output
is faithful to what the ROM data specifies. There is **no demonstrable bug from internal
evidence**.

**Most likely it is CORRECT ART, not a bug.** ChatGPT (consulted via the recomp thread,
having read SMS Power's stitched Act 1 map) reports the 8-bit GHZ background genuinely
has "repeated jagged skyline/mountain elements ... pink/purple/blue with bright yellow
highlights" — i.e. the SMS GHZ distant scenery is jagged and colourful (unlike the
plain-sky Genesis GHZ one mentally compares to). So our yellow/white/black jagged block
is plausibly the intended distant-mountain highlight set. Verdict: "the runner is
faithfully drawing what the ROM loaded; the only remaining uncertainty is whether your
eyes expect Genesis-style GHZ."

DECISIVE confirmation — **DONE (2026-06-21): CONFIRMED CORRECT ROM ART.** Installed
Emulicious (accurate SMS emulator, `_diag/emu/`), ran `sonicthehedgehog.sms` to GHZ Act 1
attract-demo, captured + cropped the horizon band, and compared side-by-side against the
recomp (`_diag/emu_vs_recomp_band.png`, `_diag/emu_ghz_full.png`, `_diag/emu_ghz_horizon.png`).
The emulator renders the **identical** jagged pink/yellow/white/blue dithered skyline — same
palette, same dithering, same silhouette (incl. the distinctive tall central peak with a
pink/white tip flanked by blue spikes). The "garbage band" is intended Green Hill Zone
background art; the recompiler reproduces it faithfully. **Not a bug — closed.**

### PSG audio — DONE (wired + verified; live watch awaits user, #23)

The clean-room SN76489 is now wired end-to-end and producing recognisable music.

- **`sn76489.{c,h}` re-based on the SMS Z80 clock + made stereo.** It previously
  `#include`d a non-existent `genesis_clocks.h` (vendored from clownmdemu, never
  built here). Now clocked off Z80 T-states (`/16` → ~223.7 kHz via `sms_clocks.h`),
  and renders **stereo** so the GG stereo register ($06) routes each channel L/R
  (SMS keeps the default 0xFF mask → both sides identical). Clean-room synthesis
  math unchanged; `psg_advance(z80_cycles)` / `psg_write` / `psg_write_stereo(mask)`
  / `psg_render(stereo frames)`.
- **Wired into `glue.c`.** `sms_io_out`: $40–$7F → `psg_write`, GG $06 → `psg_write_stereo`.
  The PSG is advanced inside `advance_vdp()` — the single VDP-timing choke point shared
  by the recomp, hybrid, and interp paths — so audio is identical on every path and stays
  in lockstep with video. Synthesis runs only when an audio sink is attached
  (`glue_set_audio_sink`), so headless oracle/diff runs pay nothing (6200 frames still
  207 ms; 8 misses unchanged). Each completed frame's output is drained to the sink.
- **Outputs (main.c, fan-out sink).** `--audio-wav out.wav` writes an observable
  S16-stereo WAV at the native rate (headless, deterministic, diffable); the SDL window
  build (`-DSMS_HAVE_SDL`) opens a live audio device via `host_audio_{init,submit,shutdown}`
  (SDL_AudioStream resamples 223.7 kHz → 48 kHz), `--mute` to silence. Both may be active.
- **Verified:** Sonic 1 SMS, 2400-frame run → `_diag/sonic_audio.wav` (40 s): 71.9 %
  non-silent, peak 21565 (no clipping), `L==R` (correct SMS mono), and a clear musical
  RMS envelope — intro jingle (0–8 s) with dynamics, rests, then sustained GHZ attract
  music (20–38 s) with accents/phrasing (recognisable note durations ⇒ pitch + tempo
  correct). The `--interp` oracle path produces the same. Both builds warning-clean.
  **Awaits user listen-test (#23) for the by-ear confirmation.**

### Controller input + frame pacing — DONE (2026-06-22; `dev/input` → main)

Live-window I/O bring-up (both live in the SDL host loop, so kept together):

- **Controller input.** `glue.c` serves real state on `$DC`/`$DD` (P1 D-pad +
  buttons + P2 up/down, active-low) and GG **START** on port `$00` bit 7, all read
  through the single `sms_io_in` (so input reaches the recomp AND hybrid/interp
  paths). The SDL host maps keyboard → P1 mask (arrows / Z=B1 / X=B2 / Enter=Start
  / Esc=quit) and `main.c` bridges it to `glue_set_pad1()` each frame; headless
  stays idle (`0xFF`). Verified headlessly via a new `--press FRAME:KEYS` scripted-
  input hook: a no-input vs button-held run is byte-identical through the press,
  then the VDP state diverges 2 frames later and stays diverged — the game
  demonstrably responds. `SMS_PAD_*` bits + `glue_set_pad1/2` + `glue_set_input_cb`.
- **Frame pacing.** The window paced purely on vsync, so it ran at `refresh/60 ×`
  (2× on a 120 Hz display — the "too fast / time way off" report). Replaced with a
  precise wall-clock deadline limiter (`host_set_frame_cap`, drift-free advance
  model, coarse `SDL_Delay` + spin) locked to `glue_frame_rate()` (NTSC 59.92 fps);
  PRESENTVSYNC dropped so speed is independent of monitor refresh. Verified:
  marginal 60.2 fps over 600 frames (was unbounded). Also fixes the audio queue
  growth that 2× playback caused. Tradeoff: possible tearing without vsync
  (cosmetic; revisit with audio-driven sync later).

### Recomp-vs-hybrid execution split — INSTRUMENTED

`glue.c` now accumulates the Z80 cycles each dispatch-miss spends in the superzazu
interpreter (`g_hybrid_cyc` / `g_hybrid_calls`) and prints an `[exec]` line at
shutdown: `hybrid calls, N of M Z80 cyc = X% interp / Y% static`. Always-on, cheap.
See the measured numbers in DEBUG/handoff notes.

### Known issues — from the first live window runs (2026-06-21/22, user)

1. ~~Speed/timing too fast~~ — **FIXED** (wall-clock 60 fps cap; see above).
2. ~~No input~~ — **FIXED** (controller wired; see above).
3. **No background on the main menu.** A render gap on the menu screen (background
   absent while sprites/text show). Title + gameplay render fine, so it's screen-
   specific — suspect a nametable-base / scroll / mode detail the renderer doesn't
   yet handle on that screen. Needs investigation.

### Remaining

1. ~~GHZ band~~ — **CONFIRMED correct ROM art** via Emulicious (see above); closed.
   `--vdp-trace` + the VDP-write ring remain reusable for any future VRAM/CRAM bug.
2. Statically resolve the script-engine dispatch-miss targets (multi-bank
   `[jump_tables]`) to shrink the hybrid's hot set — optional; the hybrid covers
   them. Interactive play exercises more of them than headless attract (16 vs 8
   distinct misses in the first live session), so the hybrid's static-resolution
   backlog grows as more screens are reached — see the `[exec]` interp/static split.
3. **Main-menu background** (`dev/menu-bg`) — found to be a non-issue (the menu
   bg is intentionally black). Skipped.

## Game Gear bring-up — Sonic Blast (2026-06-22, `dev/gg-bringup`)

First end-to-end GG title. From-scratch recompile of `SonicBlastGG` (1 MB GG ROM).

### Bank-state A-tracker fix (shipped) — boots GG via hybrid

Sonic Blast dead-looped in reset: `bankstate_step` only matched the *tight*
`ld a,#n ; ld ($FFFE),a` idiom and missed `ld a,$02 ; ld ($D118),a ; ld
($FFFE),a` (an A-preserving store between the load and the frame-reg write), so
`func_509F` decoded under bank 1 (`01 CF 41…`) instead of bank 2 (`E5 C5 21…
otir ; jp $4E27`) and the reset's `call $509F` re-ran reset forever. Replaced the
2-insn idiom match with a proper known-`A` value tracker (`a_known`, per-slot
`known[3]`); shared by finder + codegen. Result: **boots + renders SEGA logo →
Chaos-emerald → SONIC BLAST title** (GG 12-bit CRAM + viewport correct) — but
the whole game ran in one non-returning superzazu hybrid call (computed `jp (hl)`
dispatch tree the static finder couldn't follow). See [[bankstate-a-value-tracker]].

### Automatic jump-table detection (shipped) — recompiles the main loop

`function_finder.c`: on an unresolved computed `jp (hl)` (not the `push;jp(hl)`
computed-CALL idiom), symbolically simulate the dispatch basic block to recover
`base + index*scale` (handles `ld de/hl,base ; add hl,de/bc/hl`, `ld h,0;ld l,a`,
`ex af,af'` index save/restore), then read entries (16-bit LE, **always stride
2**). Count: **PROVEN** from a guard chain (`cp/sub` bound on the index) → exact;
else **DENSE** = `(first_forward_target − base)/2`, bounded also by the first
known-code address (`funclist_has_code`) so interleaved code can't extend a
table. Accepted table bytes are marked DATA (`mark_data`) so the tracer won't
decode them as code; the registry is rebuilt each fixpoint pass so dense bounds
converge. Design conferred with ChatGPT (Z80: bound by index DOMAIN, not target
validity — almost every byte decodes). See [[gg-bringup-jumptable-plan]].

Result (Sonic Blast): **9 tables, discovery 51 → 172 functions**; the main loop
is recompiled — `[exec]` ~75 % static / 25 % interp (was ~0 % / 100 %),
`irq_taken` 331 over 950 frames (was 1), full real timing. **Sonic 1 SMS
UNCHANGED** (131 funcs, 0 truncated, irq/frame 1.00, 8 misses — detector fires on
0 tables there; its TOML/computed-call paths still cover it). No regression to
the validated SMS baseline.

### KNOWN REGRESSION (open) — Sonic Blast render breaks ~frame 264

Recompiling the main loop **exposed a latent recomp divergence** (NOT a
jump-table-logic bug per se): game RAM matches the `--interp` oracle byte-for-byte
through frame 236, then a title-state handler cluster (`0x4Fxx–0x53xx`, active
from frame 237) computes wrong values (a buffer at `$DB04` fills `$0222` where the
oracle has `$0111`; a counter at `$D131` is off by one), which feed an `$8000`
hybrid buffer-fill and corrupt RAM/VRAM → the title renders white by frame 900.
First VRAM divergence frame 203 (one byte `$0AC2`), first *persistent* RAM
divergence frame 237. The CPU logic is otherwise correct (RAM matches until 237).
**Next step:** bisect the `0x4Fxx–0x53xx` cluster — compare recomp vs interp
register state at the cluster entry, or audit its generated C for a mistranslated
instruction. Reusable instruments built this session: raw VRAM/RAM dumps
(`<png>.vram`/`.ram`), env-armed VRAM/RAM write watches (`SMS_VRAM_WATCH` /
`SMS_RAM_WATCH` + `_FRAME`, `-DSMS_TRACE_PC`), per-table jt log, RAM hash column
in `--vdp-trace`.

## ROMs — PRESENT and parser-verified

Five titles are in place as workspace-level game dirs (gitignored ROMs). All
parse correctly through `rom_parser` (platform/region/mapper/CRC all match the
known good dumps):

| Game | Dir | Platform | CRC32 | Size |
|------|-----|----------|-------|------|
| Sonic the Hedgehog | `SonicTheHedgehogSMS` | SMS-Export | `0xB519E833` | 256 KB |
| Sonic Blast | `SonicBlastGG` | GG-Export | `0x031B9DA9` | 1 MB |
| Sonic Chaos | `SonicChaosGG` | GG-Export | `0x663F2ABB` | 512 KB |
| Sonic & Tails (JP) | `SonicAndTailsGG` | GG-Japan | `0x8AC0DADE` | 512 KB |
| Sonic & Tails 2 (JP) | `SonicAndTails2GG` | GG-Japan | `0x496BCE64` | 512 KB |

Bring-up order: **SonicTheHedgehogSMS** first (SMS path), then **SonicBlastGG**
(GG path). The codegen phase is now UNBLOCKED.

## Environment note

Compile via the **PowerShell tool**, not the Bash tool (the Bash MSYS2 gcc
exits 1 silently here). See the memory note.
