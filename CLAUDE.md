# CLAUDE.md — smsggrecomp

Entry point for any Claude Code session on the SMS/GG static recompiler.
Read `PRINCIPLES.md` first; this file is the orientation layer on top of it.

## What this repo is

A static recompiler that translates Sega Master System **and** Game Gear Z80
ROMs into native C, paired with a clean-room runner (own VDP/bus/IO, clean-
room SN76489 PSG, MIT superzazu Z80 for the hybrid interpreter path). One
engine serves both platforms; GG is SMS + a `platform` flag (PRINCIPLES #25).

First targets: the Sonic the Hedgehog SMS ports and Sonic Blast (Game Gear).

## Repo topology

Game dirs are **workspace-level siblings** of the engine (the snesrecomp /
nesrecomp model), each tagged with its platform:

```
F:\Projects\smsggrecomp\              ← workspace (NOT a git repo)
├── smsggrecomp\                      ← THIS engine repo (git)
│   ├── PRINCIPLES.md                 ← rules; READ FIRST
│   ├── CLAUDE.md / README.md / DEBUG.md / STATUS.md
│   ├── recompiler\src\               ← C tool; builds SmsRecomp.exe
│   │   ├── main_sms.c                ← entry            (ROM phase)
│   │   ├── rom_parser.{c,h}          ← TMR SEGA header, mapper, CRC   ✓
│   │   ├── z80_decoder.{c,h}         ← full Z80 ISA decode            ✓ tested
│   │   ├── function_finder.{c,h}     ← static reachability + tracer   ✓ tested
│   │   ├── code_generator.{c,h}      ← Z80 → C          (ROM phase)
│   │   ├── game_config.{c,h}         ← game.toml schema (ROM phase)
│   │   └── toml.{c,h}                ← vendored TOML parser
│   ├── runner\                       ← SHARED ENGINE
│   │   ├── include\sms_runtime.h     ← Z80State, bus/IO, globals      ✓
│   │   ├── include\z80_ops.h         ← flag/ALU semantic core         ✓ tested
│   │   ├── video\sms_vdp.*           ← VDP (mode 4 + GG palette)  (ROM phase)
│   │   ├── audio\sn76489.*           ← clean-room PSG (+ GG stereo)   ✓ vendored
│   │   ├── external\superzazu\       ← MIT Z80 interpreter            ✓
│   │   ├── game_spec.h, game_layout.h, main.c, glue.c  (ROM phase)
│   │   └── sms_clocks.h              (ROM phase)
│   └── tests\                        ← decoder / ops / frontend self-tests ✓
├── SonicTheHedgehogSMS\   sonicthehedgehog.sms   crc B519E833  (primary SMS)
├── SonicBlastGG\          sonicblast.gg          crc 031B9DA9  (primary GG)
├── SonicChaosGG\          sonicchaos.gg          crc 663F2ABB
├── SonicAndTailsGG\       sonicandtails.gg       crc 8AC0DADE
└── SonicAndTails2GG\      sonicandtails2.gg      crc 496BCE64
```

Each game dir holds `game.toml`, the (gitignored) ROM, and `generated/`. Per-
game source (`<prefix>_spec.c`, etc.) lands there in the ROM phase.

**Topology invariant**: the shared runner is at `smsggrecomp/runner/`. Per-game
code lives in that game's workspace-level directory and reaches the engine via
`../smsggrecomp/`.

## Per-game contract

Shared runner reads two tables:

- **`g_game_spec`** (`runner/game_spec.h`) — function-pointer hooks: identity
  (name, CRC32, ROM size, platform), entry/IRQ/NMI callbacks, lifecycle hooks
  (`on_post_reset`, `on_frame_pre/post`), CLI handler, dispatch override,
  per-game TCP commands. Each game provides exactly one TU defining
  `const GameSpec g_game_spec`.
- **`g_game_layout`** (`runner/game_layout.h`) — per-game RAM addresses,
  populated from `[ram_layout]` in `game.toml` by the recompiler, emitted as
  `<prefix>_layout.c`.

Never put literal per-game RAM addresses or function names in shared runner
code (PRINCIPLES #21).

## The dispatch-miss loop

After EVERY game run, check `dispatch_misses.log` next to the executable.
Empty → done. Non-empty → resolve via static ROM analysis (NOT by hand-adding
`extra` entries from the log alone — PRINCIPLES #16), regenerate, rebuild,
re-run. Repeat until empty.

## Z80-specific notes (vs the 68K/6502 siblings)

- **Variable-length instructions.** A wrong function-start address decodes to
  garbage. Boundary precision is paramount (PRINCIPLES #16).
- **Prefix groups.** `CB` (bit/rotate), `ED` (extended), `DD`/`FD` (IX/IY),
  and the `DDCB`/`FDCB` displacement-then-opcode forms all need explicit
  decode paths. The undocumented `DD`/`FD` half-register ops (IXH/IXL/IYH/IYL)
  appear in real SMS code — handle them, don't `STOP`.
- **Flags are exact.** Z80 S/Z/Y/H/X/P/N/C semantics (incl. the undocumented
  X/Y flags from bits 3/5) must match `superzazu/z80.c`. Sonic's SMS engine
  leans on carry/zero from `CP`, `DEC`, and the `ADD HL,rr` half-carry.
- **I/O ports are the hardware interface**, not memory-mapped registers:
  VDP data `$BE` / control `$BF`, PSG `$7F` (write), V-counter `$7E`,
  H-counter `$7F` (read), controller `$DC`/`$DD`, memory-control `$3E`,
  GG stereo `$06` + GG system `$00-$05`. `IN`/`OUT` route through the runner.
- **Memory map**: `$0000-$BFFF` paged ROM (Sega mapper frame regs at
  `$FFFC-$FFFF`; Codemasters at `$0000/$4000/$8000`), `$C000-$DFFF` 8 KB RAM
  mirrored to `$E000-$FFFF`.

## Build commands

### Recompiler
```bash
cd recompiler && cmake -S . -B build -A x64 && cmake --build build --config Release
```

### Regen + run a game (once a ROM is present)
```bash
cd sonic1sms
../recompiler/build/Release/SmsRecomp.exe sonic1.sms --game game.toml
# build the runner (CMake target per game), then:
taskkill //F //IM Sonic1SmsRecomp.exe 2>/dev/null
./Sonic1SmsRecomp.exe sonic1.sms --port 4390
```

## Hard rules (PRINCIPLES cheat sheet)

- ROM/static analysis is ground truth (#16). No discovery from
  `dispatch_misses.log` alone.
- Always-on rings; never arm-then-attach; no pause/step (#17, #22).
- No printf telemetry in hot paths (#18).
- Never edit generated C (#19). Fix recompiler → regenerate.
- Per-game data through `g_game_spec` / `g_game_layout` (#21).
- SMS and GG are one machine + a flag (#25). No fork, no second repo.
- No ROMs/saves/logs committed (#26).
- User verifies end-to-end (#23). Never claim "fixed" without confirmation.
- **Never commit without explicit user instruction.**
