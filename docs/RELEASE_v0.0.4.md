# v0.0.4 release qualification

Owner authorized commits and all three releases on2026-11-02. This integrates
the qualified native/HLE package with current upstream coverage and ROM checks
(engine base a92717d, Sonic SMS base b643f54, Sonic Blast base87ff069).
Both game pins consume the published engine release. No ROM or generated
cartridge source is packaged. The standalone asset is SmsRecomp.exe.

The actual extracted Windows ZIPs passed clean-PATH startup with their packaged
SDL2.dll. Both were built through the shared CMake/PowerShell pipeline at O2
with native execution and GPU presentation available. `Play.cmd` uses ordinary
native presentation; `Play 16x.cmd` selects
faithful16x. Both use normal audio. Third-party license notices are included.

| Released package | Assisted routes, zero fallback | Rendered checkpoints |16x GPU p95 |16x frame-work p95 |
|---|---:|---:|---:|---:|
| Sonic the Hedgehog SMS |29/29 |89/89 |0.193536ms |0.8151ms |
| Sonic Blast GG |30/30 |119/120 |0.109568ms |0.7200ms |

The retained GG exception is the existing03-1 moved single-frame ring-icon
upload observation documented in COVERAGE.md. It is not counted as a pass.
These assisted routes do not certify full unassisted playthroughs.

Both6000-frame release routes match the qualified native checkpoint in guest
state, complete timeline and PCM. Every16x GPU pixel matches native output;
upload totals remain469837200 bytes SMS /631137360 bytes GG. Frame-work and GPU
p95 each remain below13.3505ms. Seven alternating runtime pairs per game resolve
release integration as neutral/nonregressing within shared-load variation;
no additional speedup is claimed. Median route CPU ms/frame is0.3515625->0.3515625
SMS and0.3984375->0.390625 GG. Raw mixed samples are retained.

The released recompiler reproduces all four generated inputs for each game
byte-for-byte and imports only KERNEL32.dll and msvcrt.dll. Synthetic coverage
checks validate ROM identity, spaced paths, unsafe-route rejection, in-flight
fallback accounting and live-bank dispatch. ROM-window/reload tests also pass. The shared CMake route/bank/ROM/fallback
checks pass on Ubuntu as well; the test removes its temporary fixture/build.

Hashes, exact raw paths and per-game results are in [RELEASE_v0.0.4.json](RELEASE_v0.0.4.json).
Task evidence is retained under G:/Temp/Codex/smsgg-host-20261101/release-v0.0.4.
The existing [host/HLE record](HOST_HLE_BURNDOWN.md) retains candidate decisions
and original optimization measurements. Owner visual acceptance is preserved.
