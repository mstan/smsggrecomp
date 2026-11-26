# Shared runtime services

SMS and Game Gear consumers compile one host runtime archive and retain their
own original-image identity, generated code, game configuration and packaging.
The current public Z80 ABI remains pinned. Native dispatch gaps terminate with
an explicit coverage error instead of entering an interpreter.

The SMS/GG SN76489 provider replaces the engine's duplicated synthesizer. It
retains the accepted native oscillator, stereo-routing and filtering policy.
The host consumes its stereo PCM through the shared clock bridge in an SDL
device callback, with device exclusion around producer/reset operations.
The shared presentation pacer owns rational host deadlines; existing native
VDP jobs and GPU rendering are retained.

SDL owns controller handle/event lifetime and publishes standard snapshots to
the input provider. The runtime and launcher share the same seven action
bindings. The launcher offers the original cartridge picker, Home/Play,
Controller and an independent Gameplay diagnostics mod. Recording starts on
by default and remembers an explicit saved off choice. Atomic settings writes
and status publication use the persistence provider.

The diagnostics provider owns loopback transport and the bounded input writer.
The engine owns the `SMSGG_INPUT_V1` header and `I <frame> <mask>` rows. Full
execution snapshots are not supported: generated native call continuations
cannot be serialized as machine memory. Repeatable scenarios use restart and
ROM-bound preparation routes.

The mod provider binds the original SHA-256 and validates expected bytes,
overlap claims and rollback for live RAM and cartridge transactions. Both are
writable destinations. Original files are opened only for reading. Cartridge
offsets address the loaded image, and RAM offsets address its 8 KiB RAM array.
This adapter does not claim ZIP catalog/manifest adoption: this runtime did
not previously expose a package catalog, and its built-in diagnostics feature
is engine policy.

The CD-image provider does not match raw `.sms`/`.gg` cartridges and is not
linked. Likewise, this change does not claim new widescreen content rendering;
the faithful SMS viewport and Game Gear crop remain the renderer's contract.

## Native runtime control

`--debug-port 0 --status-file <path>` publishes an available loopback port.
Commands are newline-delimited; each reply is one JSON line:

- `status`, `pause`, `resume`, `quit`
- `step <positive-frame-count>` while paused
- `capture <prefix>` for native PNG, memory planes and execution report
- `pad <0..127>` or `pad live`
- `record on` or `record off`
- `read <ram|cartridge> <hex-offset> <byte-count>`
- `patch <ram|cartridge> <hex-offset> <expected-hex> <replacement-hex>`

Pause/resume never opens the launcher or a settings screen. `--pause-at N`
prepares a finite scene, and `--show-at-ready` reveals its initially hidden
window only once that scene is paused. Preparation skips live PCM delivery;
normal audio and realtime pacing resume when play starts. Background checks
use hidden windows and zero output volume or mute.
