# DEBUG.md — SMS/GG runner observability

Follows PRINCIPLES #17 (always-on rings) and #22 (free-running diff). This is
the inventory of always-on capture and the TCP command surface. Rings record
from boot; probes **connect → query a backward window → analyze**. Never
arm-then-record. Never pause/step to synchronize two observers.

> Scaffold note: the rings and TCP server below are the *intended* surface,
> mirrored from `segagenesisrecomp/runner/cmd_server.c`. As the runner lands,
> each ring is implemented in the always-on path (Release too); this file is
> the contract they implement against.

## Always-on rings (Tier-1)

| Ring | Captures | Entry shape (key fields) |
|------|----------|--------------------------|
| `bus_ring` | every Z80 memory access | frame, cyc, pc, addr, value, is_write, bank |
| `io_ring` | every `IN`/`OUT` | frame, cyc, pc, port, value, is_out |
| `frame_record` | full per-frame state | frame, regs (AF/BC/DE/HL/IX/IY/SP/PC), vdp regs, mapper frames, game_data tail |
| `reverse_debug` | every RAM write `$C000-$DFFF` | frame, addr, value, caller_pc |
| `crash_report` | recent function entries | ring of (pc, name) for the last N calls |

If the data you need isn't here, EXTEND the ring (add the event class to the
always-on path), then query. Do not work around with arm-and-attach.

## TCP command server

The runner opens a line-oriented TCP command server on `--port <N>` (default
off). Commands are newline-terminated; replies are JSON. Probes live in
`tools/`.

| Command | Purpose |
|---------|---------|
| `ping` | liveness / version |
| `state` | current Z80 regs + frame + cycle |
| `bus_ring <count>` | last N bus accesses |
| `io_ring <count>` | last N port accesses |
| `frame <n>` | frame_record for frame n (or latest) |
| `rdb_window <addr> <count>` | reverse-debug writes to addr, newest first |
| `crash` | recent function-entry ring |
| `dispatch_misses` | unresolved jump targets seen this session |
| `screenshot <path>` | dump current framebuffer as PNG |
| `quit` | clean shutdown |

## Cross-binary divergence (native vs oracle)

1. Launch native + oracle on different ports, both free-running.
2. Probe queries each binary's block-trace via `state` / `frame`.
3. Find the first PC where (pc, AF, BC, DE, HL, IX, IY, SP) differ.
4. Report block index, frame, divergence.

The oracle build links a reference SMS/GG emulator core (dev-only; chosen and
wired up when divergence work begins). The shipped native build links zero
reference-core code.

## Dispatch-miss loop (see PRINCIPLES #13a)

After every run: `cat dispatch_misses.log` next to the exe. Non-empty means
the game jumped to an un-generated address — resolve via static ROM analysis,
regenerate, rebuild, repeat until empty.
