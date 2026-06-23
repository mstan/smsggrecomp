/*
 * shard_jit.c — Tier-2 shard table, request queue, and worker thread (P0 scaffold).
 *
 * Threading invariant (SLJIT.md §2): the game thread NEVER compiles/validates/writes
 * cache. It only does a lock-free table lookup and a non-blocking request enqueue.
 * A single worker thread drains requests; in P0 it does nothing else yet.
 *
 * Built only with -DSMS_HAVE_JIT (the whole file compiles to nothing otherwise).
 */
#ifdef SMS_HAVE_JIT

#include "shard_jit.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shard table: lock-free for the reader (game thread) ----------------- *
 * Open-addressed, power-of-two. The worker publishes a fully-built Shard with a
 * release store of the slot pointer and sets `trusted` last; the reader uses
 * acquire loads. In P0 nothing is ever published, so lookup always misses. */
typedef struct Shard {
    uint16_t       addr;
    uint8_t        bank_ctx;     /* mapped bank(s) at compile time (disambiguates aliased code) */
    uint32_t       code_crc;     /* CRC of the live code bytes; re-checked before native dispatch */
    ShardFn        fn;
    _Atomic int    trusted;      /* set last, after K consecutive-clean validations (P2) */
} Shard;

#define SHARD_BITS 12
#define SHARD_SIZE (1u << SHARD_BITS)
#define SHARD_MASK (SHARD_SIZE - 1u)
static Shard *_Atomic g_slots[SHARD_SIZE];   /* zero-init: all empty */

static inline uint32_t shard_hash(uint16_t addr){
    return (uint32_t)((addr * 2654435761u) >> (32 - SHARD_BITS)) & SHARD_MASK;
}

ShardFn sms_jit_lookup(uint16_t addr){
    uint32_t h = shard_hash(addr);
    for (uint32_t i = 0; i < SHARD_SIZE; i++){
        Shard *s = atomic_load_explicit(&g_slots[(h + i) & SHARD_MASK], memory_order_acquire);
        if (!s) return NULL;                 /* empty slot ends the probe chain */
        if (s->addr == addr && atomic_load_explicit(&s->trusted, memory_order_acquire))
            return s->fn;                    /* (P1 also re-checks code_crc vs live bytes) */
    }
    return NULL;
}

/* ---- request queue: single-producer (game thread) / single-consumer (worker) *
 * Bounded ring + dedup bitmap so a hot miss doesn't flood it. The producer only
 * touches g_requested[] and the ring head; the consumer only the ring tail. */
#define RQ_CAP 256
static uint16_t        g_rq[RQ_CAP];
static _Atomic uint32_t g_rq_head;           /* producer writes */
static _Atomic uint32_t g_rq_tail;           /* consumer writes */
static uint8_t        *g_requested;          /* 64K: addr already enqueued this session */
static _Atomic uint64_t g_req_count;         /* total accepted requests (debug) */
static _Atomic uint64_t g_req_dropped;       /* dropped because the ring was full (debug) */

static pthread_t        g_worker;
static pthread_mutex_t  g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int      g_run;               /* 1 while the worker should keep running */
static int              g_started;

void sms_jit_request(uint16_t addr){
    /* dedup: enqueue each address at most once per session (P0). Producer-only. */
    if (g_requested && g_requested[addr]) return;
    if (g_requested) g_requested[addr] = 1;

    uint32_t head = atomic_load_explicit(&g_rq_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_rq_tail, memory_order_acquire);
    if (head - tail >= RQ_CAP){              /* full: drop (addr stays Tier-3; may re-request later) */
        if (g_requested) g_requested[addr] = 0;
        atomic_fetch_add_explicit(&g_req_dropped, 1, memory_order_relaxed);
        return;
    }
    g_rq[head & (RQ_CAP - 1)] = addr;
    atomic_store_explicit(&g_rq_head, head + 1, memory_order_release);
    atomic_fetch_add_explicit(&g_req_count, 1, memory_order_relaxed);

    pthread_mutex_lock(&g_mx);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
}

static int rq_pop(uint16_t *out){
    uint32_t tail = atomic_load_explicit(&g_rq_tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&g_rq_head, memory_order_acquire);
    if (tail == head) return 0;              /* empty */
    *out = g_rq[tail & (RQ_CAP - 1)];
    atomic_store_explicit(&g_rq_tail, tail + 1, memory_order_release);
    return 1;
}

static void *worker_main(void *arg){
    (void)arg;
    while (atomic_load_explicit(&g_run, memory_order_acquire)){
        uint16_t addr;
        if (rq_pop(&addr)){
            /* P0: nothing to do yet. P1 will: decode [addr,RET) -> emit sljit shard;
             * P2 will: differential-validate vs the interpreter on a snapshot, then
             * publish into g_slots[] and append to the on-disk re-JIT manifest. */
            (void)addr;
            continue;
        }
        /* queue empty: sleep until signalled (or shutdown). */
        pthread_mutex_lock(&g_mx);
        if (atomic_load_explicit(&g_rq_head, memory_order_acquire) ==
            atomic_load_explicit(&g_rq_tail, memory_order_acquire) &&
            atomic_load_explicit(&g_run, memory_order_acquire))
            pthread_cond_wait(&g_cv, &g_mx);
        pthread_mutex_unlock(&g_mx);
    }
    return NULL;
}

void sms_jit_init(void){
    if (g_started) return;
    g_requested = (uint8_t*)calloc(0x10000, 1);
    atomic_store_explicit(&g_run, 1, memory_order_release);
    if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0){
        fprintf(stderr, "[jit] worker thread create failed; Tier-2 disabled this run\n");
        atomic_store_explicit(&g_run, 0, memory_order_release);
        return;
    }
    g_started = 1;
    fprintf(stderr, "[jit] shard worker started (scaffold; no emitter yet)\n");
}

void sms_jit_shutdown(void){
    if (!g_started) return;
    atomic_store_explicit(&g_run, 0, memory_order_release);
    pthread_mutex_lock(&g_mx);
    pthread_cond_signal(&g_cv);
    pthread_mutex_unlock(&g_mx);
    pthread_join(g_worker, NULL);
    g_started = 0;
    fprintf(stderr, "[jit] shard worker stopped: %llu requests (%llu dropped)\n",
            (unsigned long long)atomic_load(&g_req_count),
            (unsigned long long)atomic_load(&g_req_dropped));
    free(g_requested); g_requested = NULL;
}

#endif /* SMS_HAVE_JIT */
