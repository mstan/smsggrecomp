# smsref — SMS/GG reference oracle (Genesis Plus GX, headless)

A dedicated accuracy oracle for the SMS/GG recompiler, modeled on the sibling
projects' `psxref`/`snesref`/`mdref`. Wraps **Genesis Plus GX** headlessly so we
can diff the recomp's state against an independent accurate emulator without
Mesen's GUI/Lua friction. **Oracle-only — GPGX is never linked into the shipped
runner** (license + size), exactly like the genesis project gates clownmdemu.

## Layout
- `osd.h`, `smsref.c` — the headless frontend (in this repo).
- GPGX core — **external clone, NOT committed** (treat like a submodule):
  ```
  git clone --depth 1 https://github.com/ekeeke/genesis-plus-gx.git \
    F:\Projects\smsggrecomp\smsref-ext\genesis-plus-gx
  ```

## Build + run
```
tools/smsref/build.ps1                 # -> tools/smsref/smsref.exe
smsref.exe <rom.sms> --frames N --dump "450,1200,1600" --out PREFIX
```
Per dumped frame F: `PREFIX_F.vram` (16 KB), `PREFIX_F.cram` (32 B, de-strided
from GPGX's 2-byte SMS layout), `PREFIX_F.cpu` (labeled Z80 regs). Same format
the recomp emits → diff with `tools/oracle/{vdp_diff,state_diff}.py` unchanged.

## Validated (2026-06-28)
Sonic 1 SMS, frames 450/1200/1600/2000: GPGX **VRAM 16 KB + CRAM 32 B
byte-identical to the recomp** (which already matches Mesen) at every frame.
smsref is a confirmed independent oracle — headless, deterministic, one process
for all frames (<1 s vs ~1 min per Mesen launch).

## Status / next (per accuracy/SMSREF_PLAN.md)
- [x] Phase A: headless state dump (VRAM/CRAM/Z80), validated.
- [ ] Phase B: TCP/JSON server (persistent, queryable) — replace per-frame launches.
- [ ] Phase C: chip_ring + synth_replay — replay identical PSG writes through our
      sn76489 vs GPGX's psg.c (real period-0=0x400) → reopens the audio axis.
</content>
