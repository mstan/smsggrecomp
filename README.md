# SMS/GG Z80 Static Recompiler

A static recompiler that translates **Sega Master System** and **Game Gear**
Z80 ROM binaries into native C code, paired with a clean-room runner that
emulates the rest of the machine (VDP, SN76489 PSG, controller/system I/O,
mapper). One engine, both platforms — the Game Gear is the Master System with
a cropped viewport, a wider palette, and stereo sound (see PRINCIPLES.md #25).

> **Status: bring-up / scaffold.** The recompiler tool and runner compile and
> are architecturally complete; no game has been generated yet (needs a ROM).
> First targets: the **Sonic the Hedgehog SMS ports** (Sonic 1 / Sonic 2 /
> Sonic Chaos / Sonic Blast on SMS) and **Sonic Blast / Sonic 2 on Game Gear**.

## How it works

The recompiler (`recompiler/src/`) decodes every reachable Z80 instruction in
the ROM and emits equivalent C. Each Z80 subroutine becomes a C function
operating on a shared `Z80State` (AF, BC, DE, HL, IX, IY, SP, PC, the shadow
set, I/R, flags) and the same 64 KB address space + paged ROM as the original.
**The rest of the machine is not recompiled** — VDP rendering, the SN76489
PSG, controller/system ports, and the Sega/Codemasters mapper all run in the
runner. Same model as the sibling projects: recompile the CPU, emulate the
silicon.

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
| `sonic1sms/` | First per-game target: `game.toml` + `generated/` (READ-ONLY) + spec. |
| `tools/` | Platform-agnostic probes and the release packager. |
| `docs/` | Design notes. |

## Platform support

Targets **Windows (MSVC)**, **macOS**, and **Linux**. SDL2 handles windowing,
rendering, audio, and gamepads.

## Building the recompiler

```bash
cd recompiler
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # Windows
cmake --build build --config Release
# macOS/Linux:  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build
```

## Regenerating a game

```bash
cd sonic1sms
../recompiler/build/Release/SmsRecomp.exe sonic1.sms --game game.toml
# overwrites generated/<prefix>_full.c + <prefix>_dispatch.c, then rebuild the runner.
```

## ROMs

ROMs are **never** committed (`.gitignore`d). Drop a legally-obtained ROM into
the game directory and point `game.toml` at it. SMS ROMs are `.sms`, Game Gear
`.gg`; both are raw Z80 images with the `TMR SEGA` footer near the end of the
first 32 KB.

## License

Project code: PolyForm Noncommercial 1.0.0 (see `LICENSE.md` once added).
Vendored `superzazu/z80.c` is MIT (`runner/external/superzazu/LICENSE`). The
clean-room SN76489 carries no third-party copyleft.
