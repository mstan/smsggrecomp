# Assisted coverage and reproducible builds

The 2026-10-18 pass uses the original North American compatible retail images:
Sonic 1 SMS CRC32 `B519E833` (the same USA/Europe/Brazil image), and Sonic Blast
GG CRC32 `031B9DA9` (World). Code generation and the generated runner reject a
different cartridge identity. Renaming a GG cartridge to `.bin` preserves its
generated GG platform; a conflicting platform override is rejected.

Both game repositories now use `cmake/SmsGame.cmake` and thin PowerShell
wrappers around `tools/build_game.ps1`. CMake builds the recompiler, regenerates
from the supplied ROM and manifest, and links the common runtime. On Windows
the helper calls native CMake, Ninja and GCC by absolute path. Build directories
can be absolute and the caller's current directory does not select the project.
The CMake graph also builds with GCC/Ninja on Linux.

## Results and limits

Sonic 1's manifest grows from 13 to 37 ROM-verified entries. All 27 surveyed
ordinary/coda and bonus selectors run with zero fallback, compared with
3,663,400 interpreted cycles across the original survey. All 81 pixel,
VRAM, CRAM and VDP-register captures agree with the interpreter. Two additional
routes select the original ending with zero or six emeralds and reach the staff
roll at frame 6,000, with zero fallback and eight matching rendered captures.
Selectors are written before the original level loader; its 40-byte headers
were checked against the supplied cartridge's original header table.

Sonic Blast's original round and character menus select all 15 acts for both
Sonic and Knuckles. Its manifest grows from 40 to 198 verified entries. The
30-case baseline contains 189,777,501 interpreted cycles; the final survey has
zero fallback. Adding entries exposed an existing bank-dispatch bug: a sole
compiled variant could receive calls for another live bank. Pageable targets
now dispatch using the actual mapper even when only one variant is known.

One Blast checkpoint differs by 33 pixels in the cropped ring icon: Knuckles,
Yellow Desert act 1, frame 3,470. Its state difference is one pending sprite
table byte at VRAM `3F25` and the WRAM upload mirror `DBA5`; the immediately
following frames agree byte-for-byte. The reports retain this difference.
They do not assert that every capture or complete CPU/WRAM state is equal.
Independent Mesen samples agree on the Blast menu/loading VRAM and palette;
moving-input samples still have different observation/input phases. Those
samples are not a claim of complete external-emulator parity.

This is assisted scene coverage, rather than an unassisted full playthrough.
The shared runtime has no widescreen renderer. SMS captures are 256x192 and GG
captures use the actual 160x144 viewport.

## Running a game suite

The game owns `tests/coverage/suite.json` and its ROM-free route files:

```text
python smsggrecomp/tools/run_coverage_suite.py --runtime build/Game.exe --rom owned.sms --suite tests/coverage/suite.json --out coverage-new
```

Use a fresh output directory. The tool runs separate native and interpreter
processes, preserves execution counters and surface comparisons, and requires
zero fallback for the coverage requirement. Surface disagreements remain
explicit in its JSON report. This comparison shares the bus and renderer; it
does not certify independent hardware or complete machine-state parity.

## Route controls

Pass `--route PATH --report result.json` to an ordinary generated runner. The
first non-comment line binds the route to the full cartridge CRC32:

```text
rom B519E833
300 pad B
310 pad -
1000 write D23E 0B
1000 write D2D3 0B
1000 or D206 10
1000 write D289 01
1250 capture loaded
```

Frames are decimal and ordered; addresses/values are hexadecimal. `write`,
`or` and `expect` accept WRAM `C000..DFFF` only. Pads use `UDLRABS`, with `-`
for release. `capture` writes a native-viewport PNG and WRAM/VRAM/CRAM/register
planes plus execution JSON. Relative names follow the copied route file.
Commands run at completed frame boundaries before the next guest instruction.
Guest code still performs loading, graphics, movement and collision.

Malformed routes, wrong ROMs, unsafe writes, failed expectations and explicit
frame limits shorter than the route fail. Do not combine routes with `--press`.
Windowed scripted input is preserved when the physical keyboard is idle.

The report includes total, interpreted and native Z80 cycles. Fallback is
accounted per instruction before a frame-limit unwind, including a routine
that never returns. `tools/test_coverage_route.py` builds a synthetic cartridge
and checks real execution, ROM identity, route rejection, in-flight accounting
and a sole compiled bank variant alongside an uncompiled live bank. It passes
with native Windows tools and Linux GCC/CMake/Ninja.

## References

- [Sonic 1 walkthrough](https://gamefaqs.gamespot.com/sms/563499-sonic-the-hedgehog/faqs/13942)
- [Sonic 1 original-ROM disassembly reference](https://github.com/iamgreaser/s1smsdisasm-gm)
- [Sonic Blast walkthrough](https://gamefaqs.gamespot.com/gamegear/586802-sonic-blast/faqs/6433)
- [Sonic Blast original round-select cheat](https://gamefaqs.gamespot.com/gamegear/586802-sonic-blast/cheats)
