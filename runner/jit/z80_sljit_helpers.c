/*
 * z80_sljit_helpers.c — flag-exact helpers the emitted shards CALL.
 *
 * Each wraps the SAME z80_ops.h semantic core the generated static C uses, so a
 * shard's result (value + flags on Z80State) is identical to the static path by
 * construction — and the static path is validated against superzazu. The word-
 * sized signatures match the sljit call ABI (SLJIT_ARGS*(W, ...)).
 */
#ifdef SMS_HAVE_JIT

#include "jit_abi.h"
#include "z80_ops.h"

long z80h_inc8(Z80State *s, long v){ return (long)z80_inc8(s, (uint8_t)v); }
long z80h_dec8(Z80State *s, long v){ return (long)z80_dec8(s, (uint8_t)v); }

#endif /* SMS_HAVE_JIT */
