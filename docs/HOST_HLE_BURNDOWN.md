# SMS/GG host HLE and faithful 16x rendering

Owner-approved scope, 2026-11-01: shared engine, Sonic the Hedgehog SMS and
Sonic Blast GG. Tracking: `beads-xecp.1.6`, related to `beads-2wy8.1`.
One bounded helper was authorized for PSG implementation and a focus review.
Owner authorized commits and v0.0.4 releases on 2026-11-02. Release integration
and artifact verification are tracked separately in `beads-xecp.1.7`.

**Complete for the approved two-pilot scope, 2026-11-02.** The initial
rendering package did not exhaust the broader native/HLE work. The follow-up
implemented and evaluated production instruction hooks, guarded native calls,
register execution spans, quiet IRQ event jobs and PSG event spans. Both games
pass the final combined gate and the owner confirmed both visual checks pass.
There are no remaining B0-B7 acceptance items or queued tests in this scope.
This does not qualify other games or lower-end GPU targets.

## Contract

Required: real 16x GPU internal targets at normal guest refresh with headroom
on the owner's PC, preserving original tile pixels, art and motion. SMS is
4096x3072; the GG visible crop is 2560x2304. No smoothing or invented detail.
CPU preparation and uploads must be independent of density. GPU jobs and
resources never enter guest memory, state hashes or canonical serialization.
Retain build-selected scalar/reference paths. Per-game regressions cannot be
offset by another game's gain. Old save-state compatibility is unnecessary.

Required headroom: p95 frame work excluding pacing and p95 GPU timestamp cost
each below 80% of the emulated frame period, without accumulated backlog.
Record outliers separately. 32x/64x and lower-end 16x are unqualified follow-ups.

## Implementation register

| ID | Job | Current evidence / disposition |
|---|---|---|
| B0 | Baselines and qualification | Qualified. Original engine `224d5bb2c150a2c295033d35dec629ef9ee42940`; SMS `1058d49e7527d35d6fd5bb208ec6eb44da8bc72e`; GG `aa25dbc92b722b179c91832a91a15ebba8d064d1`. Final timing arms share corrected generated code; codegen fixes are not credited as speedups. |
| B1 | Production/publication work | Qualified. Removed duplicate rendering and GPU-path CPU pixels; native instruction hooks remove diagnostic counters/probes while retaining guest time, EI/IRQ boundaries and entry breadcrumbs. Reference builds retain instruction diagnostics. Legacy generated code keeps unconditional interrupted-PC publication. |
| B2 | Compiler, execution and bus | Qualified O2/direct ROM windows, live-bank guarded calls, register-only native spans and quiet IRQ event jobs. Original instruction path remains at event boundaries/after EI. Both routes execute100% static with zero interpreter work, so further interpreter tuning is rejected as cold. HALT already jumps deadlines. Deferring observable PSG/IRQ boundaries is rejected for correctness. |
| B3 | Native VDP production | Immutable inline tile-row sources, palettes, ordered sprite lists and scanline capture shared by native/GPU output. 512 snapshot and 16 changing-raster cases match the scalar reference. Scanline completion sampling improves on final-frame snapshots; dot-exact raster timing is not claimed. |
| B4 | Resident GPU 16x | Qualified required16x on the RTX3080Ti. OpenGL3.3 integer sampling, changed-row residency, bounded targets. All GPU pixels match native output on both6000-frame routes; upload totals are exactly equal at1x/4x/16x. 48 component cases additionally cover source mutation, resize/reset and context recreation. |
| B5 | PSG and audio publication | Qualified borrowed-scratch publication and native constant-output/event spans, cached stereo mixes and analytical muted clocks. Exact integer filter recurrence retained. 56309 full-state/PCM component comparisons and both6000-frame built-game routes pass. Current-build save/load rebuilds derived caches; original synthesis remains build-selected. |
| B6 | Host lifecycle and pacing | Qualified. Rejected GPU as the ordinary native default after slower/variable measurements; keep SDL native and engage GPU on explicit internal resolution. Per-pass timestamps, bounded memory and native fallback pass. Reference builds present scalar pixels. Scripted input survives window callbacks. |
| B7 | Combined gate and owner handoff | Final native package passes exact state/timeline/PCM, every16x pixel, unchanged upload totals and per-game performance gates. Both owner visual checks passed. Final derivative matches those accepted outputs; no repeated handoff. Changes are uncommitted; consumer pins are preserved. |

## Qualified measurements

Raw pairs, exact source/ROM/binary identities, profiles and limitations:
[HOST_HLE_RESULTS.json](HOST_HLE_RESULTS.json). Do not attribute package gains
to an individual member. The snapshot-to-scanline accuracy change is explicit
in the baseline description.

| Metric | Sonic SMS | Sonic Blast GG |
|---|---:|---:|
| Native windowed route CPU ms/frame, accepted checkpoint -> final | 0.3984375 -> 0.3515625 | 0.4375 -> 0.3984375 |
| Median route CPU reduction, final incremental package | 11.76% | 8.93% |
| Whole native-window CPU reduction / disposition | 8.97% / positive | 5.99% / positive |
| Final16x GPU p95, upload + render + present | 0.194560ms | 0.111616ms |
| Final16x frame-work p95 | 0.9474ms | 0.8109ms |
| Worst16x frame-work p95 across qualification | 6.6291ms | 7.6859ms |
| Admission/headroom limit | 13.3505ms | 13.3505ms |
| Upload bytes/frame, identical at1x/4x/16x | 78,306.2 | 105,189.56 |
| GPU color target at16x | 48MiB | 22.5MiB |
| Process peak at explicit GPU1x ->16x, 600-frame startup probe | 69.09 ->68.55MiB | 72.11 ->72.14MiB |

The final incremental measurements use five adjacent alternating pairs against
the actual owner-accepted `qualified-final-o2` binaries. Shared-load variation
and every raw sample are retained. Separate three-pair register-span results
are neutral/inconclusive in both games; do not attribute the package gains to
spans alone. Individual hooks/dispatch/events/PSG arms also ran both complete
routes with exact state and PCM. PSG improved every isolated route sample;
smaller CPU changes are retained as neutral where variation prevents attribution.

The preceding accepted package completed1200 normally paced frames at16x in
20.44-20.56s including process/driver startup. Paced frame-work p95 was
2.1823ms SMS /5.5895ms GG. The GG paced run retained a69.8002ms wall-work
outlier under shared host load; this is not a hitch-free claim. Raw samples
and the active host census are retained. Earlier GPU-default native1x runs
were slower and variable and are rejected as the default presentation policy.

## Build and use

The two consumer scripts default to qualified O2/native execution and accept
`-EngineRoot` (alias of `-Framework`), `-Build`, `-Optimization`,
`-PortableDisplay`, `-ReferenceVDP`
and `-ReferenceExecution`. The last switch restores original instruction hooks
and PSG synthesis for diagnostics. It was built through the normal SMS script
and matches the complete6000-frame state/timeline/PCM. For a sibling engine checkout, use `-EngineRoot F:/Projects/smsggrecomp/smsggrecomp` to build the
working implementation. Existing pinned older engines still build normally.

`--window 3 --internal-scale 16` selects normal paced 16x presentation;
the ordinary native default keeps SDL presentation. Explicit `--internal-scale 1`
selects the GPU path at native density for comparisons. `--benchmark` renders native
frames and synthesizes audio even headless; it removes pacing when windowed.
`--benchmark-start N` separates route CPU time from startup. `--hidden`,
`--benchmark-output`, `--gpu-report` and `--gpu-dump` support bounded QA.
Automated SDL audio uses a process-local dummy device. Owner handoffs use
normal audio and ordinary windows.

`SMS_VDP_REFERENCE` selects the scalar line renderer; `SMS_BUS_REFERENCE`
selects original modulo bus reads. Omit `SMS_GPU` for portable SDL presentation.
Production scripts select `SMS_NATIVE_HOOKS`, `SMS_NATIVE_DISPATCH`,
`SMS_NATIVE_EVENTS` and `SMS_NATIVE_SPANS` only when the engine advertises that
contract. `SMS_PSG_REFERENCE` restores per-sample audio. Native spans require
native hooks; generated headers version the conditional-PC contract, and old
generated code safely retains unconditional PC stores. Instruction diagnostic
environment variables fail clearly in a native build instead of silently
changing their meaning; use `-ReferenceExecution` for those jobs.
Jobs are borrowed synchronously during frame publication; retaining one requires
an owned copy. OpenGL copies changed source rows before the guest resumes.
GPU targets fall back to a lower effective density on admission failure and
report it. Missing OpenGL falls back to native SDL, never a CPU 16x image.

## Evidence and limits

Retained task root: `G:/Temp/Codex/smsgg-host-20261101`. `baseline/` contains the
original sources plus common measurement-only entrypoint changes and hidden
baseline windows. `original-generated/` preserves original generated inputs.
Build/run JSON, binary hashes, original/raw comparisons and entry disassembly
are retained alongside component and built-game outputs. Early startup-only
measurements do not qualify active gameplay; intermediate GG native builds
with the newly exposed bank-alias defect are explicitly excluded.

Portable jobs build and run on Ubuntu/WSL and the physical Steam Deck;
the Deck executable was removed after its passing run. No Deck GPU/16x claim.
Final native hooks/spans also pass Windows and Ubuntu boundary tests. A full
portable Ubuntu native SMS build completes the6000-frame route with exact
binary guest dumps/native PNG/PCM; text dumps and timeline differ only in OS
newlines. Its temporary executable and WAV were removed.
The current runner lacks complete save/load, rewind and netplay products;
this campaign does not invent those features. Current renderer lifecycle
tests cover reset and source invalidation. Existing sprite-limit/ordering,
collision-status and dot-timing accuracy gaps remain separate unless proven
to block the exercised contract. The independent oracle remains oracle-only.

Design references: the Desktop GPU high-resolution guide; GBA immutable-job
commits `4edc278`, `ba32c62`, `f646f81`; CD-i HLE burndown; NES/SNES performance
notes; Virtual Boy shared/native burndowns; PSX shared optimization record.
Hardware reference: https://www.smspower.org/Development/SMSOfficialDocs .
No third-party renderer implementation was imported.

## Owner handoff and retained data

The completed owner handoff used
`G:/Temp/Codex/smsgg-host-20261101/PLAYTEST.ps1`: SMS, then GG after SMS
closes, using normal windows, normal audio and16x. Arrows move; Z/X are the
buttons; Enter is GG Start. Check movement/jump, scrolling/HUD, resize and
sound; exercise GG pause/resume and retry where practical. SMS Pause/NMI
input is a pre-existing unwired feature, not an accepted new check.
`human-qa.json` retains launch/exit records without treating exit as a Pass.
Owner confirmed both visual checks pass on 2026-11-02; this is recorded in
`human-qa.json`. Do not repeat the visual handoff. Exact state/PCM/pixels must
still qualify later execution changes against those accepted builds.

The raw task root is retained for reproducibility. Current qualified binaries
are in `native-final-o2/<game>/game.exe`; `native-source.json` records their
exact inputs and hashes. `remaining-results.json` retains individual candidate
arms; `native-final-results.json` retains final paired CPU/GPU evidence.
Raw PCM is archived losslessly as `.wav.gz` with verified uncompressed SHA256
in `pcm-archives.json`; disposable decoded copies are removed. The temporary
Deck executable and Ubuntu `/tmp` test were removed. The installed recompiler
at `recompiler/build/SmsRecomp.exe` is retained as the current project tool;
repeated regeneration reproduces the qualified generated sources exactly.
No delivery ZIP, native save, owner setting, commit or release was replaced.
