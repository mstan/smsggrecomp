# SLJIT.md — Tier-2 sljit shard JIT for smsggrecomp

**Status: implemented + byte-exact, OPT-IN (`-DSMS_HAVE_JIT`), NOT default-on (2026-06-22).**
The middle execution tier — an in-process off-thread sljit JIT — is built, validated
byte-exact, and gated off by default. Companion to `PRINCIPLES.md`. Sibling references:
`../psxrecomp/SLJIT.md` (the originating MIPS design, shipped + live-validated)
and `../gbarecomp/gbarecomp/docs/SLJIT.md` (the ARM port, on-by-default). **Read
those for the family rationale; this doc only states what is the same and what is
deliberately different for the Z80 SMS/GG engine.**

> **STRATEGIC CALL (2026-06-22) — keep opt-in, do NOT pursue default-on for this engine.**
> Measured headless: pure interpretation already runs Sonic Blast GG at ~3,000× realtime
> on a modern PC — there is **no perf bottleneck to solve** on SMS/GG. JIT-ON is in fact
> *slower* than JIT-OFF (worker-validation + per-dispatch lookup + Bus-indirection cost),
> and the shards reduce interp by ~0% net (28.31% → 28.15%): they shard the small
> jump-table *dispatcher prologues*, while the heavy interp cycles live in routines using
> `IN`/`ED`/live device state that the precision gate correctly refuses to shard. So
> coverage % rises without a perf win. **Value delivered = the byte-exact OFF-THREAD
> reference design** for the family (psxrecomp/gbarecomp, where on-thread JIT caused the
> audio stutters and the target systems are genuinely demanding). Default-on here would
> regress speed for zero user-visible benefit. The productive next step is porting this
> off-thread model to the siblings, not widening this engine's emitter. See §8.6 / §8.7.

---

## 0. Where smsggrecomp is today

Two tiers, no JIT:

- **Tier 1 — static native (AOT C).** `recompiler/src/code_generator.c` emits one
  C function per discovered Z80 subroutine over a shared `Z80State`; gcc lowers it.
  Computed/known targets dispatch through the generated `call_by_address()` switch.
- **Tier 3 — interpreter floor.** A computed target the static finder can't resolve
  hits the switch `default:` → `sms_dispatch_miss()` → `hybrid_interpret()`
  (vendored superzazu Z80 over the live bus, runs until the routine RETs).

The gap is **Tier 2**: there is no JIT between static C and the interpreter. Every
dispatch-missed routine is interpreted **every time** it runs. For Sonic Blast GG
that is ~29% of all executed cycles (the bank-dispatched per-frame routine `0x8000`
and the line-IRQ raster routines run interpreted on every frame). This doc adds the
sljit shard tier so those routines become native after first encounter.

---

## 1. Tier model (locked)

```
Tier 1  STATIC NATIVE   AOT-recompiled corpus. Generated call_by_address() switch.
                        Fastest. Unchanged.
Tier 2  SLJIT SHARD     In-process JIT of one discovered function, produced the
                        first time a PC would fall to Tier 3. Validated against
                        Tier 3, then run native on subsequent hits. NEW.
Tier 3  INTERP FLOOR    superzazu hybrid_interpret(). Correctness floor AND the
                        differential oracle every shard is validated against.
                        Unchanged.
```

Priority on every computed dispatch: **Tier 1 → Tier 2 (if a trusted shard exists)
→ Tier 3**. A shard never replaces a static function; it only ever fills the
Tier-3 path.

A **shard = one discovered function**, `[entry, RET)` — the same unit as a static
function and the same unit `hybrid_interpret()` and the existing diff harness
already operate on. Not a basic block, not a page. Internal branches, loops, and
calls are handled inside the one fragment; calls re-enter the dispatcher (any tier).

---

## 2. Off-thread only — the family's new direction

> **The game thread NEVER compiles, validates, or generates/writes cache. All sljit
> compilation, differential validation, and shard-cache I/O happen exclusively on a
> dedicated worker thread. The game thread's only shard interaction is a lock-free
> table lookup.**

psxrecomp and gbarecomp currently JIT shards **synchronously on the game thread**
(sub-ms, on the miss path) — and that has been causing **audible audio stutters**:
the JIT + validation stall, even when short, lands inside the real-time frame/audio
loop and underruns the audio buffer. The whole recomp family is therefore moving
**all** ecosystems off-thread. smsggrecomp implements the off-thread model from the
start, so it serves as the **reference design** for that migration — not a one-off
divergence.

The model: compilation, validation, and cache I/O run on a dedicated worker thread;
the game thread never blocks on a shard. The miss path is asynchronous —
interp-this-frame, native-later (§3). This keeps the 60 fps loop and the SDL audio
callback stall-free regardless of JIT/validation cost.

Consequence — the miss path is **asynchronous, interp-this-time/native-later**:

```
game thread, computed dispatch to addr:
  1. Tier-1 known?            -> run static C
  2. shard_table[addr] TRUSTED? (lock-free read) -> run shard native
  3. else: hybrid_interpret(addr)              (Tier 3, this frame)
           + enqueue_compile_request(addr, entry_snapshot)   (non-blocking, dedup)
```

The first N encounters of an addr always run on Tier 3; the worker compiles and
validates in the background; once promoted, encounter N+1 runs native. No game-thread
stall, ever. This mirrors how the parents run their *gcc-DLL* producer (async
worker) — we apply that discipline to the *sljit* producer too.

---

## 3. The worker thread

A single long-lived `shard_worker` thread (created at runtime init, joined at
shutdown). Owns: the sljit compiler, the validation sandbox, and the cache file.

```
loop:
  req = request_queue.pop_blocking()        // {addr, bank-context, entry snapshot}
  if shard already exists for (addr, code-crc): record another validation pass; continue
  frag = emit_shard(req)                     // Z80 decode -> sljit IR -> native (§5)
  if frag == NULL: blacklist (addr, code-crc) // emitter declined; stays Tier 3 forever
  else: validate_offthread(frag, req.snapshot) // §6 differential gate
        if clean: publish(frag) into shard_table  // §7 lock-free publish
```

The worker pulls work; it never touches the live machine. Everything it needs (entry
register state, the bank mapping, and a copy-on-validate view of RAM/VRAM/CRAM/PSG)
arrives in the request snapshot. Backpressure: a bounded queue; if full, drop the
request (the addr simply stays Tier 3 and will be re-enqueued on a later miss).

---

## 4. Dispatch wiring (game side)

Smallest possible change to the hot path. `sms_dispatch_miss(addr)` (glue.c:835) is
the single hook. New shape:

```c
void sms_dispatch_miss(uint16_t addr){
    Shard *sh = shard_table_lookup(addr);      // lock-free, hot-path safe
    if (sh && atomic_load(&sh->trusted)){
        shard_run(sh, &g_z80);                  // Tier 2: native, on live ctx
        return;
    }
    shard_request_enqueue(addr);                // non-blocking; dedup; snapshots entry
    /* ... existing first-miss logging ... */
    hybrid_interpret(addr);                     // Tier 3 this time
}
```

`shard_table_lookup` is an open-addressed hash `addr -> Shard*` (the parents'
`g_healed`/candidate-chain model). Multiple shards per addr are disambiguated by a
**live-code CRC** (banked code at the same Z80 address differs across banks — same
problem PS1 overlays / GBA address-reuse solve), exactly as the parents do.

`call_by_address()` itself (generated) is unchanged — it still routes known targets
to static C and unknown ones to `sms_dispatch_miss`. Tier 2 lives entirely inside the
miss handler, so **no regeneration of game C is required** to add the tier.

---

## 5. The Z80 → sljit emitter (the real work)

`runner/jit/z80_sljit.c` — walks the same `Z80Insn` decode the C codegen and the
finder already use (`recompiler/src/z80_decoder.*`), emitting sljit LIR per
instruction instead of C text. It is a **parallel of the interpreter**, and the
interpreter (superzazu) is its parity oracle.

- **ABI — context-passing, not globals.** A shard is
  `void shard(Z80State *s, const Bus *bus)`. It must NOT reference the runtime
  globals (`g_z80`, `g_ram`, `sms_read8`…) directly, because the worker validates it
  **off-thread on a snapshot** (§6) and the game thread runs it on the live context;
  both require the state+bus to be parameters, not globals. `Bus` is a small vtable
  (`read8/write8/io_in/io_out`) — live bus for real runs, sandbox bus for validation.
  (This is psxrecomp's `CPUState*` ABI and gbarecomp's `g_cpu` discipline, adapted so
  the operand is passed, never global — the price of the off-thread requirement.)
- **Register allocation.** Block-local: cache the hot Z80 regs (A/F/HL/BC/DE) in sljit
  virtual registers, flush to `*s` before any transfer or `bus`/helper call, reload at
  join points. Start simple (flush every reg before every call; optimize later).
- **Control flow.** Internal JR/JP/DJNZ/conditional → sljit labels/jumps within the
  fragment. CALL/RST → flush + helper `shard_call(s, bus, target)` which re-enters the
  dispatcher (so a callee is any tier). RET / `jr (hl)` tail → set `s->pc`, return; the
  dispatcher re-enters. Mirrors the CALL/RET-depth contract the hybrid already uses.
- **Timing parity is mandatory.** Each emitted instruction must advance `s->cyc`
  exactly as the static/interp paths do (the same per-instruction T-state table in
  `code_generator.c::cyc_base` + the conditional extras). The IRQ-sampling model
  (sync-first, see the timing memory) must hold in shards too: a shard polls the sync
  deadline at instruction boundaries identically. A one-cycle delta fails the gate.

### Precision over recall — the decline gate

`runner/jit/z80_sljit_supports.c` is the **one** place that decides what the emitter
can lower. On ANY unsupported opcode, prefix form, or shape (initially: ED block ops,
undocumented DD/FD half-register ops, anything touching `r`/refresh in an observable
way, IM2 vectoring), `emit_shard` returns NULL → that addr stays Tier 3 **forever**
(blacklisted). A shard can decline; it can **never** mis-compile. A missed function is
free (interp runs it); a wrong one is fatal. Coverage grows by widening the supported
set, each addition gated by the differential harness.

---

## 6. Differential gate (off-thread) — we already have most of it

smsggrecomp **already** has the on-thread version: `sms_diff_enter` /
`diff_run_super` / `diff_compare` (glue.c) replay a function recompiled-vs-superzazu
from one frozen snapshot and diff exit regs + RAM + `cyc`. The Tier-2 gate is that
harness, moved to the worker and pointed at a shard instead of the static function:

```
validate_offthread(frag, snapshot):
  // two private copies of the machine, seeded identically from the snapshot:
  stateA, busA = clone(snapshot)   // run the SHARD
  stateB, busB = clone(snapshot)   // run the INTERPRETER (superzazu)
  shard_run(frag, &stateA, &busA)
  interp_run(snapshot.addr, &stateB, &busB)
  clean = (stateA.regs == stateB.regs) && (busA.write_log == busB.write_log)
          && (stateA.cyc == stateB.cyc)
```

- **Pure function of the snapshot** — no live-machine access, fully thread-safe.
- **I/O is compared, not applied.** The sandbox bus records a write log (RAM + VDP/PSG
  port writes) rather than mutating live devices, so an I/O-touching routine validates
  by comparing write logs. (This is cleaner than psxrecomp's live device-touch trapping
  and matches gbarecomp's snapshot-and-rerun gate.)
- **Consecutive-clean promotion.** One clean snapshot proves one path. A shard is
  marked `trusted` only after **K distinct entry snapshots** validate clean (default
  K=8, tunable). Until promoted, the shard exists but the game thread keeps using Tier
  3. Any divergence blacklists the shard (back to Tier 3 permanently). This bounds the
  branch-coverage risk of validating on single entry states.
- **Un-snapshottable routines pin to Tier 3.** Anything whose result depends on state
  the snapshot can't capture deterministically (true mid-routine IRQ acceptance timing,
  external link-port I/O) is never promoted — it just stays on the interpreter.

---

## 7. Thread-safety & publish

- **Shard table:** lock-free for readers (game thread). The worker publishes a fully-
  built, immutable `Shard` by an atomic store of the table slot pointer + an atomic
  `trusted` flag set last (release), read with acquire on the hot path. Executable
  memory is allocated and made RX by the worker before publish; never freed while the
  game runs (shards live for the process; bounded count).
- **Request queue:** SPSC/MPSC bounded ring, game thread produces, worker consumes.
  Dedup by addr+crc so a hot miss doesn't flood it.
- **No shared mutable state** between a running shard (game thread) and the worker: the
  worker only writes table slots not yet published. Snapshots are owned by the request.

---

## 8. Two-tier shard cache — both scopes off-thread

A compiled shard lives at two scopes. The game thread participates in neither — it
only ever reads the in-memory table.

**(a) In-memory — current session (the primary payoff).** On compile + promote the
worker allocates RX executable memory and publishes the shard into the in-memory
`shard_table` (§7); it stays resident for the whole process. Every subsequent
occurrence of that address *in this session* takes the native path via the lock-free
lookup — **no disk round-trip**. A shard is never discarded after one use.

**(b) On disk — future sessions (warm start).** The worker also records promoted
shards to `recomp_cache/<rom-crc>/shards.txt`. Two possible forms:

- **v1 — re-JIT manifest (chosen).** Store only the shard *identity*:
  `<addr> <bank-context> <code-crc>` — **not** the native bytes. On the next launch the
  worker reads the manifest and re-JITs + re-validates those functions in the
  background, ideally before the game needs them, so they reach native far sooner than
  rediscovering from misses. Native code is regenerated each session. Simple, and
  immune to ASLR/toolchain drift because nothing executable is persisted.
- **v2 — serialized native blob (deferred).** Persist the compiled bytes via
  `sljit_serialize_compiler`/`sljit_deserialize_compiler`, skipping re-JIT on load.
  Requires making the shard position-independent first (every host-helper call routed
  through a cpu-relative pointer table instead of a baked absolute address) so it
  reloads under a new ASLR base. No in-session benefit, real extra discipline →
  deferred until the re-JIT warm start proves insufficient.

So: **native-in-memory for the current session immediately; a lightweight identity
manifest on disk so the next session re-JITs the same shards early.** All cache
read/write and any re-JIT run on the worker thread; the disk path never touches the
game thread.

---

## 8.5 The shard sync model (CRITICAL — discovered in P1e)

A shard must replicate what the static path's `sms_tick()` does, not just bump `cyc`.

In the static corpus, every instruction calls `sms_tick(n)`, which is **sync-first**:
`if (cyc >= g_sync_deadline) sms_sync(); cyc += n;`. `sms_sync()` advances the VDP to
the current cycle and **accepts a pending IRQ** (`take_irq` → the IM1 handler) at the
instruction boundary. The interpreter does the same per step. **A correct shard must
do this too** — otherwise, for any routine that runs long enough to cross a scanline /
IRQ boundary, the VDP never advances and interrupts never fire mid-routine, and the
shard's behavior (and the whole frame's timing) diverges from the oracle.

P1e proved this the hard way: `0x8000` (Sonic Blast's per-frame main routine) compiled
and **validated byte-exact off-thread**, then broke the game live (CRAM/REG fell to
~34%/72%). The shard's `emit_tick` only did `cyc += n`. Short shards (e.g. `0x0DE8` =
a bare RET) never cross a deadline, so they stayed correct; `0x8000` runs a whole frame
and crosses many. **The off-thread gate cannot catch this**: validation runs on a
*frozen* snapshot (VDP/IRQs disabled), so the no-sync shard matches the no-sync oracle
there but diverges against the live, syncing static/interp path.

**Design for the fix (next milestone, before re-enabling OUT/CALL):**
- `emit_tick` must emit, per instruction, the sync-first sequence routed through the
  `Bus`: `if (cyc >= *bus->sync_deadline) bus->sync(s); cyc += n; s->ei_block = 0;`.
  Live `bus->sync` = a wrapper over `sms_sync()` (advance VDP + `take_irq`); the
  off-thread validation `bus->sync` is a no-op with `sync_deadline = UINT64_MAX` (VDP
  frozen). Add `sync_deadline` (a `uint64_t*`) and `sync` to the `Bus`.
- The tick must precede the instruction body (sync-first), matching the static path's
  IRQ-sampling phase — i.e. emit the tick *before* each op's effect, not after (the
  current emitter ticks after). This is the same sync-first contract as
  `[[timing-model-sync-first-and-real-pc-push]]`.
- Because the live shard syncs identically to the static path, a correct emit is
  live-correct **regardless** of what the frozen-snapshot validation can prove; the gate
  remains an emit-bug catcher, not a liveness guarantee.

**P1f outcome — the sync model alone did NOT fix it.** The sync-first emit above is
implemented (Bus `sync_deadline` + `sync`; live wraps `sms_sync`, off-thread is a no-op;
all harnesses still pass). But re-enabling OUT/CALL with the sync model live, `0x8000`
still diverged **identically** (VRAM/CRAM/REG 23/34/72% — bit-for-bit the same failure as
without sync). So sync is necessary but **not the (whole) cause**. Remaining suspects for
the `0x8000` divergence, to investigate next:
- **Bank-aliased code.** `0x8000` is a *bank-dispatched* dispatch-miss; the bytes at
  `$8000` depend on the mapped ROM bank. The shard table currently keys on address only
  (no `code_crc` check), so a shard compiled for one bank could run when a different bank
  is mapped → wrong code. Add the `code_crc` re-check before native dispatch (the spec's
  candidate-chain disambiguation, §4) and see if `0x8000` stops matching.
- **1-pass validation insufficiency.** Promote only after **K consecutive-clean** snapshots
  (the spec's K=8), and verify against the live run, not just one frozen snapshot.
- **Frozen-snapshot unsoundness for frame routines.** A whole-frame routine's behavior
  depends on mid-routine IRQs whose handlers read *live* device state (V-counter / VDP
  status via `IN`); the frozen snapshot can't reproduce that, so such routines may be
  fundamentally un-validatable off-thread and should be **pinned to interp**.

**Interim safety (committed):** OUT and CALL stay *declined* (they are what let
frame-spanning routines compile), so only short, non-sync-crossing routines shard and the
game is byte-exact (CRAM/REG 100%, verified). The sync model + OUT/CALL emit + gate
handling are all implemented and harness-verified, gated off pending the investigation
above.

**RESOLVED (2026-06-22) — it was bank-aliased code.** Confirmed with an always-on
dispatch-miss probe (`glue.c` `mb_record`/`mb_dump`, env `SMS_MISSBANK`): at every
`$8000` dispatch the mapped `bank[2]` cycles `0x1B → 0x3B → 0x3A` (first seen frames
57 / 187 / 615), and the entry bytes differ from byte 0 (`C5 D5 E5…` PUSH BC/DE/HL vs
`3A 2F D1…` LD A,(D12F) vs `3A 3F D1…`). So `$8000` hosts **three different functions**;
the address-only shard table ran the bank-`0x1B` shard under banks `0x3B`/`0x3A` in the
title era (frames 187–810) → exactly the measured 23/34/72% divergence.

Fix (three pieces, all required):
1. **Code-CRC identity.** The emitter exposes the reachable extent `z80_sljit_last_span`;
   each `Shard` stores `(addr, span, crc)` (CRC over the bytes it compiled from). The
   game-thread `sms_jit_lookup` recomputes the CRC over the *currently mapped* live bytes
   and runs the shard only on a match — else it keeps scanning / falls to interp.
2. **Candidate chain.** The open-addressed table holds multiple shards per address; the
   lookup scans them all and picks the bank-matching one.
3. **Dedup by `(addr, code-version)`.** The address-only `g_requested` bitmap is replaced
   by an `(addr, window-hash)` set, so each bank variant enqueues independently instead of
   the first one permanently blocking the rest.

With this, OUT/CALL are re-enabled and the game is byte-exact to the `--interp` oracle:
CRAM 810/810, REG 810/810; VRAM 799/810 (all 11 in the benign intro window 469–651, zero
in the title era). Sonic 1 tripwire unchanged (JIT inert, 0 shards). The three `$8000`
variants now behave correctly: `0x1B` publishes byte-exact (runs frames 57–186); `0x3B`/
`0x3A` decline cleanly on unsupported ops (`jp (hl)` jump table @ `800F`; out-of-window
branch @ `A4A0`) and stay Tier-3. None mis-compile.

**Remaining (next milestone — coverage, not correctness):** title-era interp% is still
~32% because those two `$8000` variants decline on *emitter gaps* (computed `jp (hl)`,
branch outside the 2 KB window), and line-IRQ raster routine `1AAF` fails off-thread
validation (hypothesis #3 — reads live device state mid-frame; correctly pinned to interp).
Lowering interp% needs emitter coverage (jump tables, wider windows), tracked separately.

---

## 8.6 Emitter coverage pass (2026-06-22, after §8.5 fix)

Widened the emitter, all byte-exact (harnesses + game CRAM/REG 810/810 maintained
throughout; shard count 1 → 14 on Sonic Blast; interp 32% → 29.6% over 2400 frames):

- **`JP (HL)` (tail dispatch).** The jump-table dispatcher idiom (`ld a,(hl); inc hl;
  ld h,(hl); ld l,a; jp (hl)`). Pass 1 treats `jp (hl)` (op 0xE9, no prefix) as a
  terminator; pass 2 emits `z80h_jp_hl` = `bus->call(HL)` then return. No table decode
  needed — the runtime resolves HL and the dispatcher runs the target (any tier). If a
  site were a computed CALL (`push ret; jp (hl)`) the gate catches the divergence and
  declines, so the tail form is precision-safe. This unblocked the bank-`0x3B` `$8000`
  dispatcher and cascaded into its targets.
- **Far `JP/JR nn` (tail dispatch).** A static jump whose target is outside the function
  window `[base, base+2KB)` is a tail jump to another function: `z80h_jp_to` + return
  (unconditional inline; conditional via `wire_jump`/explicit skip). Same tail model.
- **CB group** (RLC/RRC/RL/RR/SLA/SRA/SLL/SRL, BIT, RES, SET on r and (HL)) via one
  `z80h_cb(s,bus,cb_byte)` helper backed by z80_ops.h. BIT n,(HL) X/Y flags use WZ,
  which the shard doesn't track — such a shard simply fails the gate and stays interp.
- **`INC/DEC (HL)`** (memory RMW) via `z80h_inc_hlm`/`z80h_dec_hlm`.
- **Dropped-request dedup bug fixed.** The `(addr,winhash)` set was marking a request as
  seen BEFORE the queue-full check, so a request dropped on a full queue was never
  retried. Now it marks only after a successful enqueue.
- **Diff harness extended** (CB + INC/DEC (HL)) and made self-modify-safe by
  write-protecting the code page on both sides (a shard runs precompiled code and can't
  self-modify mid-run — the faithful model), so the generator keeps full opcode coverage.

**Still interp (next buckets, by cycle weight):** the bank-`0x3A` `$8000` variant no
longer top-level-dispatches in the 810f window (reached nested-in-interp; a dispatch-
dynamics question, not an emitter gap); ED block ops/`LDIR` (`8AF4`, `8284`, `8254`);
DD/FD IX/IY (`5476`, `53EA`, `8150`); `LD (nn),HL`/`LD HL,(nn)` (op 0x22/0x2A — `3645`,
`1E0F`); `IN` (`1A42`); `1AAF`/`81DD` fail validation (hypothesis #3, live device state).

---

## 8.7 Performance measurement (2026-06-22) — why this stays opt-in

Headless flat-out timing (frame cap is windowed-only), Sonic Blast GG, JIT-ON
(`-DSMS_HAVE_JIT`, static + 14 shards) vs JIT-OFF (static + hybrid-interp for misses,
= the shipped behavior):

- **Realtime headroom:** JIT-OFF runs at ~180K fps headless = **~3,000× realtime**. The
  interpreter is nowhere near a bottleneck on a modern PC.
- **JIT-ON is slower, never faster** across repeated 12k/60k-frame runs (−26% to −118%;
  variance is the off-thread validation worker's one-time warm-up burst — it idles after
  15 compiles, confirmed identical compile counts at 2.4k and 12k frames).
- **Shards reduce interp by ~0% net:** JIT-OFF 28.31% interp vs JIT-ON 28.15% (deterministic,
  noise-free). The shards cover the tiny dispatcher prologues; the heavy cycles stay interp.

Conclusion: on SMS/GG-on-PC the JIT cannot pay for itself — the thing it optimizes isn't a
bottleneck, and the cycles it *could* save are gated out by precision-over-recall. It is a
**correct, byte-exact, off-thread reference implementation**, valuable to the family, kept
opt-in. To make it a real win you would need (a) to shard the `IN`/`ED`/device-state routines
(solving hypothesis #3 — validating routines that read live device state) AND (b) to remove
the live shard's Bus-indirection (which exists so the same binary validates off-thread on a
snapshot) — a large effort whose payoff only appears on hardware far weaker than a PC.

---

## 9. Reused verbatim vs. new

**Reused (no per-ISA work):**
- The vendored **sljit** library — copy `../psxrecomp/lib/sljit` (BSD-2-Clause),
  single-TU `sljitLir.c` with `SLJIT_CONFIG_AUTO`. Targets all host arches already.
- The **dispatch-spine pattern** (table + crc-chain + priority ladder), the
  **precision-over-recall contract**, the **differential-gate** logic (we have the
  on-thread harness already), and the **re-JIT manifest** persistence shape.

**New (smsggrecomp-specific):**
- `runner/jit/z80_sljit.c` — the Z80 `Z80Insn` → sljit emitter (the dominant cost).
- `runner/jit/z80_sljit_supports.c` — the decline classifier.
- `runner/jit/shard_table.c` + `shard_worker.c` — lock-free table, request queue,
  worker thread, off-thread gate driver, manifest I/O.
- The `Bus` vtable indirection + a sandbox bus for validation.
- Threading the **context-passing ABI** through the miss path.

Z80 is a much smaller ISA than MIPS/ARM, so the emitter is the smallest in the family
in absolute terms; the threading + sandbox-bus is the genuinely new design surface.

---

## 10. Milestones

- **P0 — scaffold.** Vendor `lib/sljit`; build it (single TU); `shard_table` +
  `shard_worker` skeleton (worker thread that pops requests and does nothing yet);
  wire `sms_dispatch_miss` to enqueue + lookup (always falls through to Tier 3). A
  sljit smoke test (hand-built leaf fn returns 42). **No emitter.** *Stop, review.*
- **P1 — emitter core + L1 harness.** Emit the common Z80 subset (8-bit ld/alu, 16-bit
  ld/inc/dec, JR/JP/DJNZ/CALL/RET/cond, push/pop). Per-opcode differential harness
  (decode one insn → emit shard → run vs superzazu from random states → assert regs +
  flags + `cyc` identical). Decline everything outside the subset.
- **P2 — off-thread gate live.** Wire the snapshot clone + sandbox bus + consecutive-
  clean promotion on the worker. Shards promote and run native; everything unproven
  stays Tier 3. Verify on Sonic Blast `0x8000` (the 29%-interp routine).
- **P3 — coverage + manifest persistence.** Widen the supported opcode set guided by
  blacklist logs; add the re-JIT manifest. Measure interp% drop on Sonic Blast and
  confirm Sonic 1 SMS is byte-identical (it is 100% static — shards should be inert
  there, a good null test).
- **P4 — perf + regalloc.** Smarter block-local register allocation; measure.

Each milestone keeps both games' oracle agreement green (Sonic Blast title byte-exact,
Sonic 1 GHZ pixel-exact) — shards are gated to be byte-identical to the interp, so a
correct port cannot regress rendering.

## 11. Risks / open questions

- **Context ABI vs. static C globals.** Static C uses globals; shards use a passed
  context. They interop only through the dispatcher (helper calls), so this is fine,
  but it means shards and static functions don't share inlined bus access. Acceptable
  (shards target the interp tier, not the static tier). Revisit if we ever want the
  static path context-passing too.
- **Mid-routine IRQ timing in a shard.** A shard must poll the sync deadline at
  instruction boundaries like the static path (sync-first model). Getting this
  byte-identical under the gate is the subtle part; routines where it can't be proven
  pin to Tier 3.
- **Snapshot cost.** Cloning RAM (8 KB) + VRAM (16 KB) + CRAM + device regs per
  validation is cheap and off-thread; the entry snapshot captured on the game thread at
  enqueue must be cheap — capture lazily/coalesced so a flood of misses doesn't tax the
  game thread.
- **Bounded shard count.** Sega games have few dispatch-missed routines (Sonic Blast:
  ~9); the table is tiny. No eviction needed initially.
