# Flat single-step Z80 output

`SmsRecomp --flat-step` emits a scheduler-friendly static Z80 backend for a
flat binary image. It is intended for a Z80 used as a coprocessor, where the
host machine must regain control after every instruction to interleave CPUs,
devices, reset, bus ownership, and interrupts.

```sh
SmsRecomp --game path/to/game.toml --flat-step
```

For a cartridge that uploads or patches more than one code image over time,
add one or more captures:

```sh
SmsRecomp --game path/to/game.toml --flat-step \
  --flat-step-variant path/to/later-z80-ram.bin
```

The command writes `generated/<prefix>_step.c` and `_step.h`. The generated
`<prefix>_step()` executes exactly one decoded instruction from `g_z80.pc` and
returns. Every byte offset in the input image is emitted as a possible PC, so
computed control flow does not depend on a profiling manifest.

The generated cases guard their compiled instruction bytes against live
memory. Captured variants become alternate compiled byte sequences at the same
PC. An incomplete upload, different revision, or uncaptured self-modifying code
calls the host's `sms_dispatch_miss()` fallback. Matching code has no runtime
opcode decode; the host retains the fallback policy and scheduler.

For stable opcode shapes whose operand bytes are commonly self-modified, the
generated case keeps the opcode statically decoded and reads the live immediate
or indexed displacement at execution time. This avoids treating ordinary
operand changes as new code.

The host consumes `z80-recomp-core` and supplies its `sms_runtime.h` bus
functions around the shared `Z80State`. If
it already defines a symbol named `call_by_address`, compile with
`SMS_RUNTIME_NO_CALL_BY_ADDRESS` to omit the function-form runtime declaration.

This mode does not replace the normal SMS/Game Gear function-form output. It
shares the same decoder, semantic emitters, flag representation, and timing
tables.
