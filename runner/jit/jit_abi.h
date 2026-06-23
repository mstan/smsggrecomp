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
    void    *ctx;
} Bus;

typedef void (*ShardFn)(Z80State *s, const Bus *bus);

#endif /* SMS_JIT_ABI_H */
