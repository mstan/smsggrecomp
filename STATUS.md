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
| Clean-room SN76489 PSG | `runner/audio/sn76489.{c,h}` | ports directly (needs `sms_clocks.h`) |
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
  read/write, I/O ($BE/$BF/$7E/$7F/controllers; PSG + GG ports stubbed),
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
hybrid fix resolved it. The SDL host presents once per `g_frame` under vsync, so
the live window already runs at realtime.

### GHZ background band — characterised (NOT raster)

The colourful band in the GHZ frames is a **garbage strip in one region of the
background nametable/tile data** — it scrolls *with* the background (tracked
from upper-left at frame 6000, `r8=3D r9=D0`, to horizon-centre at frame 6090,
`r8=57 r9=C0`). It is NOT a per-scanline raster effect: at both frames `r10=FF`
and `r0` bit4 = 0, so line interrupts are OFF. Everything else (Sonic, ground,
palm trees, flowers, rings, water line, HUD) is pixel-accurate, so it is one
graphics strip (GHZ distant-scenery/horizon) that a load/decompress path didn't
populate correctly. Root-causing renderer-vs-recomp needs a VRAM diff against a
reference — i.e. the oracle below. (Probe shots: `runner/ghz_probe.png` frame
6000, `runner/ghz_6090.png` frame 6090.)

### Remaining

1. **Oracle** (dev-only) — reference mode BUILT, diff analysis pending.
   `--interp` (glue.c `glue_run_interp` + main.c) runs the WHOLE game under the
   vendored superzazu Z80 over the SAME clean-room VDP/bus/IO, so a renderer bug
   shows in both runs while a recomp bug shows only in `glue_run()`. Boots to the
   title screen (`runner/interp_300.png`). NEXT: run `--interp` to a GHZ frame
   and compare against the recomp PNG — if the reference shows the same garbage
   strip it's a renderer bug, else a recomp bug (then byte-diff VRAM at the first
   diverging frame). superzazu (MIT) is the only reference core used; no
   third-party SMS emulator is bundled.
2. Statically resolve the 8 script-engine targets (multi-bank `[jump_tables]`)
   to shrink the hybrid's hot set — optional; the hybrid already covers them.
3. **PSG** audio (clean-room `sn76489.c` exists; wire into `sms_io_out` + an
   audio host); **input**; then **GG bring-up** (SonicBlastGG).

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
