/*
 * shard_jit.h — Tier-2 sljit shard JIT interface (see SLJIT.md).
 *
 * The game thread only ever does: sms_jit_lookup() (lock-free) and, on a miss,
 * sms_jit_request() (non-blocking enqueue). ALL compilation / validation / cache
 * I/O happens on a dedicated worker thread — never the game thread (this is the
 * locked invariant: on-thread JIT caused audio stutters family-wide).
 *
 * Gated behind -DSMS_HAVE_JIT. When the flag is off, every entry point is a no-op
 * inline so call sites stay unconditional and the hot path is unchanged.
 *
 * P0 scaffold status: the worker runs and drains requests but does not compile
 * yet; sms_jit_lookup() always returns NULL, so execution always falls through to
 * the Tier-3 interpreter. The emitter + gate land in P1/P2.
 */
#ifndef SMS_SHARD_JIT_H
#define SMS_SHARD_JIT_H

#include <stdint.h>
#include "sms_runtime.h"
#include "jit_abi.h"          /* Bus + ShardFn (void shard(Z80State*, const Bus*)) */

#ifdef SMS_HAVE_JIT

uint64_t sms_jit_published(void);        /* live coverage counters (any thread) */
uint64_t sms_jit_requested(void);
uint64_t sms_jit_declined(void);
void    sms_jit_init(void);              /* start the worker thread */
void    sms_jit_shutdown(void);          /* signal + join the worker thread */
ShardFn sms_jit_lookup(uint16_t addr);   /* lock-free; NULL until a trusted shard exists */
void    sms_jit_request(uint16_t addr);  /* non-blocking; deduped; snapshots happen worker-side later */

#else  /* !SMS_HAVE_JIT — no-op stubs keep call sites unconditional */

static inline void sms_jit_init(void){}
static inline void sms_jit_shutdown(void){}
static inline void sms_jit_request(uint16_t addr){ (void)addr; }

#endif /* SMS_HAVE_JIT */

#endif /* SMS_SHARD_JIT_H */
