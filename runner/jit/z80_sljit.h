/*
 * z80_sljit.h — Z80 -> sljit shard emitter (see SLJIT.md §5).
 *
 * Walks the Z80 function whose bytes start at `bytes` (at least `len` readable),
 * located at guest address `base`, from base until its terminating unconditional
 * RET, emitting one native shard. Precision over recall: if ANY instruction is
 * outside the supported subset (or the function doesn't terminate within len),
 * returns NULL — the caller keeps that address on the interpreter. The emitter
 * can decline; it never mis-compiles.
 */
#ifndef SMS_Z80_SLJIT_H
#define SMS_Z80_SLJIT_H

#include <stddef.h>
#include <stdint.h>
#include "jit_abi.h"

ShardFn z80_sljit_compile(const uint8_t *bytes, size_t len, uint16_t base);

#endif /* SMS_Z80_SLJIT_H */
