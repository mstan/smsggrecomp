# PRINCIPLES.md
SMS/GG Static Recompiler — Debugging & Reverse-Engineering Principles

This document is the SINGLE source of truth for all rules.
All other files reference this. Nothing overrides this.

The first 15 principles are the platform-agnostic core, ported verbatim
from the recomp-template / `segagenesisrecomp`. Principles 16+ are
SMS/GG/Z80-specific addenda.

---

# 1. CORE PHILOSOPHY

We do not guess.
We do not explore blindly.
We do not fix symptoms.

We identify:
1. The exact point of divergence
2. The exact state difference
3. The exact instruction or function responsible

Then we fix that — and only that.

---

# 2. STATE OVER THEORY

If two systems behave differently, then their state differs.

All debugging reduces to:
- Capturing state
- Comparing state
- Finding the first difference

Do not theorize causes without state evidence.

---

# 3. FIRST DIVERGENCE (CRITICAL)

Never debug the final symptom.

Always find:
> The FIRST moment where expected ≠ actual

If you are not identifying the first divergence, you are doing it wrong.

---

# 4. TEMPORAL DEBUGGING

Bugs are about WHEN, not WHAT.

Reason in time:
- What happened before this?
- What changed?

---

# 5. WRITE VS READ BUGS

Determine the class:

Write-time:
- Wrong data written

Read-time:
- Correct data, wrong usage

Do not mix these.

---

# 6. TRACE THE WRITER

When state differs, find:
- WHO wrote it
- WHEN it was written
- WHY it differs

---

# 7. FIX THE SOURCE

Invalid:
- Clamping
- Skipping logic
- Hardcoding values

Valid:
- Fixing the producing logic
- Fixing execution order
- Reproducing missing state

---

# 8. MINIMAL FIXES

The correct fix:
- Smallest possible change
- Matches original system behavior

---

# 9. STRUCTURED DATA ONLY

Use:
- Ring buffers
- Frame snapshots
- Timeseries

Avoid:
- printf spam
- unstructured logs

---

# 10. BUILD TOOLS, NOT GUESSWORK

If you cannot answer something:
→ build a tool to answer it

---

# 11. NEVER DEBUG BLIND

If you say:
- "maybe"
- "likely"

You are missing data.

Stop and gather it.

---

# 12. STUBS (ABSOLUTE RULE)

NO STUBS — EVER

If execution reaches unknown code:
1. STOP
2. Identify target
3. Fix discovery/codegen

Never simulate behavior.

---

# 13. FUNCTION DISCOVERY

A dispatch miss is a graph failure.

Fix:
- function finder
- codegen

Never patch output.

---

# 13a. DISPATCH MISS LOOP (MANDATORY)

Dispatch misses are SILENT GAME-BREAKING BUGS.
A miss means the game jumped to an address with no generated function.
That code never executes. The game skips entire subroutines.

**After EVERY game run (manual, scripted, or test):**

1. Check `dispatch_misses.log` next to the executable
2. If it contains entries: resolve them via the disasm-driven discovery
   pipeline (NOT by hand-adding `[functions].extra` entries from the log
   alone — see Principle 16)
3. Regenerate (`SmsRecomp.exe <rom> --game game.toml`)
4. Rebuild and re-run
5. Repeat until `dispatch_misses.log` is empty

This is not optional. This is not a "later" task.
A game with dispatch misses is FUNDAMENTALLY BROKEN.

---

# 14. SUCCESS DEFINITION

A bug is fixed only when:
1. Root cause identified
2. Divergence explained
3. Fix addresses cause
4. Behavior matches reference

---

# 15. DISTRUST TOOLING

At the start of every session, validate that tools are doing what you
think they're doing. Run a known-good query, check the output by hand,
verify file paths resolve where you expect.

Never trust:
- That a previous session's tool still works the same way
- That generated output matches what you asked for
- That a grep/awk pipeline found all matches

When you build a new tool or instrument, verify its FIRST output manually
before relying on it for analysis.

---

# 16. DISASM / ROM IS GROUND TRUTH

Function discovery and jump-table inputs come from static analysis of the
ROM (and, where available, a community disassembly), NOT from runtime
feedback.

Adding `[functions].extra` entries to `game.toml` based on
`dispatch_misses.log` alone is a FALLBACK — every entry must be cross-
checked against the ROM bytes. Runtime feedback can identify *that* a
function exists; only static analysis tells you *where it begins* and
which labels are interior vs standalone.

The Z80 has no fixed instruction width, so a wrong start address yields a
completely different (garbage) decode. Boundary precision matters more
here than on the fixed-width 68K.

---

# 17. ALWAYS-ON RINGS, NEVER ARM-AND-RECORD

The runner has Tier-1 always-on ring buffers (bus_ring, frame_record,
reverse_debug, crash_report — see `DEBUG.md`). They start recording at
boot and never stop.

Probes operate as: **connect → query backward window → analyze.**

Do NOT design probes that:
1. Connect to the runner.
2. Arm a trace filter.
3. Run a workload.
4. Dump the trace.

By the time the LLM finishes setting up step 2, the workload has often
already executed step 3 unobserved. The "I observed no events" conclusion
is a lie of omission.

If the data you need isn't in an existing ring, EXTEND THE RING (add the
new event class to the always-on capture path), then query. Do not work
around it with arm-and-attach.

**Pause/step is the same anti-pattern in disguise.** If you find yourself
writing "let me pause both, step them in lockstep, then read state" —
STOP. Use the rings.

---

# 18. NO PRINTF TELEMETRY IN HOT PATHS

stderr is reserved for:
- One-shot loud-abort messages (`[ILLEGAL]`, watchdog, `assert`-style aborts).
- Startup banner / TCP-port announcement.
- The `[NOTE]` channel for one-time configuration warnings.

stderr is FORBIDDEN in:
- Z80 bus accessors (`z80_read*`, `z80_write*`, `z80_io_*`).
- VBlank / line-interrupt handlers.
- Any per-instruction or per-block hook.
- Any per-frame loop body.

If you need to observe a value: add it to a frame-record tail, the
bus_ring entry shape, or a Tier-1 entry. TCP queries pull the structured
data; printfs spam the terminal.

---

# 19. NEVER EDIT GENERATED C

Files under `*/generated/` are RECOMPILER OUTPUT.

If something in generated code is wrong:
1. Fix the recompiler (`recompiler/src/`).
2. Regenerate (`SmsRecomp.exe <rom> --game game.toml`).
3. Rebuild.

Hand-editing generated code is INVALID even as a temporary measure.

---

# 20. SUBMODULE / COMMIT ORDER

The engine (`smsggrecomp/`) is the shared recompiler + runner. Per-game
release repos (when they exist) consume it. When a change touches the
engine:
1. Commit the engine FIRST.
2. Bump the consuming release repo's pointer SECOND.

Never commit a release repo with an engine pointer that doesn't exist
upstream. **Never commit without explicit user instruction.**

---

# 21. PER-GAME DATA THROUGH SPEC + LAYOUT

Shared runner code reads:
- `g_game_spec.*` (function-pointer hooks — entry points, IRQ handlers,
  periodic callbacks, lifecycle hooks, debug commands, dispatch override).
  Defined in `runner/game_spec.h`.
- `g_game_layout.*` (per-game RAM addresses — game_mode, vblank counter,
  player object, stacks, etc.). Defined in `runner/game_layout.h` and
  populated from `[ram_layout]` in `game.toml`.

Shared runner code MUST NOT contain:
- Literal SMS/GG RAM addresses (system RAM `0xC000-0xDFFF`, mirrored to
  `0xE000-0xFFFF`).
- Per-game function names (`func_XXXX` referring to a specific ROM offset).
- Per-game compression / data-format magic.

`tools/audit_runner_purity.py` flags shared-runner files that violate this.

---

# 22. FREE-RUNNING DIFFERENCE OVER PAUSE/STEP

Cross-binary divergence detection (native vs oracle) uses always-on
rings, not pause/step lockstep.

1. Launch native + oracle on different TCP ports, both free-running.
2. Probe queries each binary's block-trace ring.
3. Find the first PC where (pc, AF, BC, DE, HL, IX, IY, SP) differ.
4. Report block index, frame, divergence.

---

# 23. USER VERIFIES END-TO-END

A change is not "done" because the recompiler builds, the runner builds,
or a test passes. A change is "done" only when the user has confirmed
end-to-end behavior matches the reference (visual + runtime + audio where
relevant). Do not claim "no regression" or "fix worked" without their
confirmation.

---

# 24. ONE RUNTIME INSTANCE AT A TIME

Use `taskkill //F //IM <exe>` before relaunching. Stale background
processes silently bind TCP ports and produce stale ring data.

---

# 25. SMS AND GG ARE ONE MACHINE WITH A FLAG

The Master System and Game Gear share the Z80 CPU, the SN76489 PSG, and a
VDP that is identical at the register/VRAM level. The Game Gear differs
ONLY in:
- Viewport: a centered 160×144 crop of the SMS 256×192 frame.
- CRAM: 12-bit (4096-color, 2 bytes/entry) vs SMS 6-bit (64-color, 1 byte).
- PSG stereo: GG adds a per-channel L/R enable latch at I/O port `$06`.
- A handful of GG-only system ports (`$00` start button / region).

These are a per-game `platform = "sms" | "gg"` flag in `game.toml` plus a
small set of VDP/PSG conditionals — NOT a forked engine, NOT a second
repo. Any "GG needs its own X" instinct is wrong unless it maps to one of
the four bullets above. The authentic SMS path must stay byte-identical
when `platform = "sms"`.

---

# 26. NO ROMs / DUMPS / SAVES IN THE REPO

`*.sms`, `*.gg`, `*.bin` ROM images, save files, and run logs are NEVER
committed. They are `.gitignore`d. Release binaries are built from the
clean-room own-backend runner (superzazu Z80 + own VDP + clean-room
SN76489) and contain zero ROM data and zero copyleft emulator-core code.
