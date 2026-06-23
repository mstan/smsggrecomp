/*
 * jit_abi.h — Tier-2 shard ABI (see SLJIT.md §5).
 *
 * A compiled shard is `void shard(Z80State *s, const Bus *bus)`. Memory and I/O go
 * through the passed Bus rather than runtime globals, so the SAME shard binary runs
 * on the live machine (game thread) or on a snapshot (worker thread, for off-thread
 * differential validation) without touching globals.
 */
#ifndef SMS_JIT_ABI_H
#define SMS_JIT_ABI_H

#include <stdint.h>
#include "sms_runtime.h"

typedef struct Bus {
    uint8_t (*read8 )(void *ctx, uint16_t addr);
    void    (*write8)(void *ctx, uint16_t addr, uint8_t val);
    uint8_t (*io_in )(void *ctx, uint8_t port);
    void    (*io_out)(void *ctx, uint8_t port, uint8_t val);
    /* run the subroutine at `target` to its RET (live dispatcher, or superzazu on a
     * snapshot during off-thread validation), updating *s. */
    void    (*call  )(void *ctx, Z80State *s, uint16_t target);
    /* per-instruction sync-first (mirrors sms_tick): the shard, before each op,
     * does `if (s->cyc >= *sync_deadline) sync(s)`. Live: sync wraps sms_sync
     * (advance VDP + accept IRQ). Off-thread validation: sync is a no-op and
     * *sync_deadline is UINT64_MAX, so the VDP/IRQs stay frozen. (See SLJIT.md §8.5.) */
    const uint64_t *sync_deadline;
    void    (*sync )(Z80State *s);
    void    *ctx;
} Bus;

typedef void (*ShardFn)(Z80State *s, const Bus *bus);

#endif /* SMS_JIT_ABI_H */
