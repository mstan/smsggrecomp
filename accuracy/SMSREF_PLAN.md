# smsref — a dedicated SMS/GG reference oracle (proposal)

Replace the Mesen-Lua oracle with a purpose-built `smsref`, modeled on the
sibling projects' ref tools (`psxref`, `snesref`, `mdref`). For *future
debugging* — the Mesen tooling already built stays usable and oracle-agnostic.

## Why move off Mesen (friction hit this session)
- **No headless audio** — WAV capture is GUI Sound Recorder only; no Lua sample
  API, no `--recordAudio`. Audio had to be deferred.
- **Lua API quirks** — docs stale: `emu.memCallbackType` is nil (it's
  `emu.callbackType.exec`); `getState()` is a *flat* dotted-key table.
- **Per-run boot non-determinism** — frame-1 guest-cycle offset varies
  (13,681 vs 58,825) unless driving a `.mmo` movie.
- **~1 minute per measurement** — each diff = a fresh Mesen process launch to
  the target frame. No persistent, queryable state.

## The sibling pattern (psxref / snesref / mdref)
| | wraps | interface | audio |
|---|---|---|---|
| psxref | DuckStation (submodule + patch) | TCP/JSON :4371, 36k frame ring | — |
| snesref | Snes9x (libretro dll) | standalone JSONL + TCP :4377 | — |
| **mdref** | **clownmdemu (submodule)** | **TCP/JSON :4379 + chip_ring** | **synth_replay** |

Common shape:
1. Accurate emulator **compiled into an oracle build only** (gated by a define
   — `SONIC_ORACLE_BUILD` etc.); the **shipped recomp carries zero oracle
   code** (licensing + size). Enforced like the genesis `LICENSING.md`.
2. **Line-delimited JSON over TCP**, dedicated port; persistent server you
   query live (`get_frame`, `get_registers`, `read_ram`, `frame_timeseries`).
3. **Always-on rings** (frame ring + chip/write rings), queried after the fact —
   never arm-then-capture.
4. Python compare scripts diff the native-server vs oracle-server on a state
   surface (these are **oracle-agnostic** — they just diff two streams).

## smsref design
**Wrap: Genesis Plus GX** (`ekeeke/genesis-plus-gx`) SMS/GG core — single clean
C codebase, native SMS+GG (Z80 + mode-4 VDP + SN76489 + YM2413 FM + Sega/CM
mappers), embeddable like clownmdemu. Oracle-build-gated (`-DSMS_ORACLE_BUILD`),
never shipped (its non-commercial license is handled exactly like genesis gates
clownmdemu/AGPL). Cycle model is accuracy-tuned but **not strictly
cycle-accurate** → keep the existing Mesen cycle harness as the *strict-cycle
second opinion* (GREEN wants cross-ref against an independent reference anyway).

Components (mirroring mdref):
1. **TCP/JSON server** (port 4380), commands `get_frame` / `get_registers` /
   `read_vram` / `read_cram` / `read_ram` / `psg_state` / `frame_timeseries` /
   `run_frames` / `pause`. The recomp runner already has the symmetric dumps;
   diffs reuse `vdp_diff.py` / `state_diff.py` unchanged.
2. **Always-on `chip_ring`** — instrument the recomp's `sms_io_out` PSG writes
   into a ring tagged `(frame, scanline, z80_cyc, pc, val)` (the `[mmio]` tally
   added this session is the seed). The oracle records the same.
3. **`synth_replay`** (the audio win — standalone headless tool):
   - Parse `chip_ring` into an absolute Z80-cycle write timeline.
   - Replay the **identical** writes through (a) our `sn76489.c` and (b) an
     **independent** SN76489 (GPGX `sound/sn76489.c`, which models real-chip
     period-0 = 0x400, the actual volume curve, real LFSR).
   - Both at native 223,721 Hz → diff with `audio_diff.py` (drift-tolerant
     envelope/pitch already built + self-tested).
   - **This makes audio accurately diffable** — both synths get identical
     inputs, so CPU-timing phase is removed; the only differences are genuine
     synthesis differences (period-0 clamp, volume table, LFSR taps/reseed).
     It directly tests the Axis-5 risk list.

## What this resolves / reuses
- **Re-opens the audio axis** (was deferred as "not accurately diffable"):
  synth_replay IS the accurate diff — register-stream replay vs an independent
  chip model. Validates/refutes the period-0 bug immediately.
- **Faster, headless, deterministic** state diffs (persistent TCP vs 1-min
  Mesen launches); sync by PC+regs (psxref-style) avoids the boot-offset issue.
- **Reuses everything built this session**: the recomp-side dumps/rings and ALL
  diff scripts (`vdp_diff`, `state_diff`, `cyc_compare`, `audio_diff`) are
  oracle-agnostic — smsref just feeds them a better oracle stream.
- **Mesen stays** as the independent strict-cycle + second-opinion cross-check
  (two independent emulators satisfies the GREEN dual-reference rule).

## Scope / phasing (recommendation)
1. **synth_replay first** (highest marginal value, smallest scope): vendor an
   independent SN76489 (GPGX `sn76489.c`), build the chip_ring tap + replay
   tool. Reopens + likely closes the period-0 audio question. ~½ day.
2. **smsref TCP state server** wrapping GPGX SMS/GG (submodule + oracle build +
   JSON server). Replaces the per-frame Mesen launches for VDP/CPU/RAM diffs.
   ~1–2 days.
3. Migrate the cyc/vdp/state diff loop to query smsref; keep Mesen as the
   strict-cycle cross-check. Retire the Mesen-Lua dump scripts (keep
   `mesen_trace.lua` for the cycle cross-check).

Open decision for the user: build now (and how far — synth_replay only, or full
smsref), or keep as the documented plan.
</content>
