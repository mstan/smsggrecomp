# SMS/GG Z80 Static Recompiler

A static recompiler that translates **Sega Master System** and **Game Gear**
Z80 ROM binaries into native C code, paired with a clean-room runner that
emulates the rest of the machine (VDP, SN76489 PSG, controller/system I/O,
mapper). One engine, both platforms — the Game Gear is the Master System with
a cropped viewport, a wider palette, and stereo sound (see PRINCIPLES.md #25).

This is **not** an emulator and **not** a hand-port: the game's Z80 machine
code becomes native C that runs directly; only the surrounding silicon is
modeled by the runtime.

> **Status: early (v0.0.2) — pre-release, expect bugs.** Two games are in
> bring-up: **Sonic the Hedgehog (SMS)** and **Sonic Blast (Game Gear)**. Across
> the title + attract-demo path exercised so far (~60s), both run with no
> interpreter-fallback dispatch miss and render byte-exact (palette + system RAM)
> to the vendored superzazu interpreter used as an oracle. Neither game has been
> played end to end, so full-gameplay coverage is unverified. Other SMS/GG Sonic
> titles are future targets.

## Per-game runner repos

smsggrecomp is the shared framework. Each game lives in its own companion repo
that supplies the per-game `game.toml`, build glue, and a pre-built release. The
ROM and the generated C are **never** committed — you bring your own legally
dumped ROM and regenerate locally.

- **Sonic the Hedgehog (SMS)** —
  [mstan/SonicTheHedgehogSMSRecomp](https://github.com/mstan/SonicTheHedgehogSMSRecomp).
  Boots and plays Green Hill Zone; renders byte-exact to the oracle. Early.
- **Sonic Blast (Game Gear)** —
  [mstan/SonicBlastGGRecomp](https://github.com/mstan/SonicBlastGGRecomp).
  Boots through the intro to the title screen, byte-exact to the oracle. Early.

## How it works

The recompiler (`recompiler/src/`) decodes every reachable Z80 instruction in
the ROM and emits equivalent C. Each Z80 subroutine becomes a C function
operating on a shared `Z80State` (AF, BC, DE, HL, IX, IY, SP, PC, the shadow
set, I/R, flags) and the same 64 KB address space + paged ROM as the original.
**The rest of the machine is not recompiled** — VDP rendering, the SN76489
PSG, controller/system ports, and the Sega/Codemasters mapper all run in the
runner. Same model as the sibling projects: recompile the CPU, emulate the
silicon. Computed jumps the static analysis can't resolve fall back to the
vendored superzazu Z80 interpreter over the live bus (the "hybrid" path).

Key pieces:
- **`z80_decoder.c`** — full Z80 ISA decode incl. the `CB`/`ED`/`DD`/`FD`/
  `DDCB`/`FDCB` prefix groups; classifies control flow (JP/JR/CALL/RET/RST/
  DJNZ) for the function finder. Semantics anchored to the vendored MIT
  `superzazu/z80.c`.
- **`code_generator.c`** — Z80 → C translation, one C function per
  subroutine, computed jumps routed through `call_by_address`, per-
  instruction T-state accumulation for line/frame timing.
- **`function_finder.c`** — static reachability from the reset/IRQ/NMI/RST
  vectors plus `[functions].extra` seeds and jump tables.

## What's in this repo

| Directory | Purpose |
|-----------|---------|
| `recompiler/src/` | The recompiler tool — analyzes the ROM, emits native C. Builds `SmsRecomp.exe`. |
| `runner/` | Shared clean-room runtime: Z80 interpreter (hybrid fallback), VDP, SN76489 PSG, I/O, mapper, SDL2 host, glue. |
| `runner/include/sms_runtime.h` | Shared interface: `Z80State`, bus/IO access, runtime globals. |
| `runner/external/superzazu/` | Vendored MIT Z80 core — interpreter + codegen reference. |
| `tools/` | Platform-agnostic probes and the release packager. |
| `docs/` | Design notes. |
| _(per-game repos)_ | Each game has its own companion repo — see **Per-game runner repos** above. |

## Platform support

Targets **Windows (MSVC / MinGW)**, **macOS**, and **Linux**. SDL2 handles
windowing, rendering, audio, and gamepads.

## Building the recompiler

```bash
cd recompiler
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # Windows
cmake --build build --config Release
# macOS/Linux:  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build
```

## Regenerating a game

From a game's companion repo checked out as a sibling of this engine:

```bash
# regenerate the native C from your own ROM:
../smsggrecomp/recompiler/build/SmsRecomp.exe sonicthehedgehog.sms --game game.toml
# overwrites generated/<prefix>_{full,dispatch,layout}.c, then rebuild the runner.
```

## ROMs

ROMs are **never** committed (`.gitignore`d), and neither is the generated C
(it is a derivative of the ROM). Drop a legally-obtained ROM into the game
directory and point `game.toml` at it. SMS ROMs are `.sms`, Game Gear `.gg`;
both are raw Z80 images with the `TMR SEGA` footer near the end of the first
32 KB.

## Acknowledgements

- **superzazu** — the vendored MIT Z80 interpreter (`runner/external/superzazu/`)
  serves as both the semantic reference for the recompiler and the oracle the
  generated code is validated against.

## License

Not yet declared. Code in this repo is original except where noted in
**Acknowledgements** above: the vendored `runner/external/superzazu/z80.c` is
MIT (see its own `LICENSE`). The clean-room SN76489 PSG is original to this
project.
