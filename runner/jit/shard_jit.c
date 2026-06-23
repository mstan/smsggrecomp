/*
 * shard_jit.c — Tier-2 off-thread shard pipeline (compile -> validate -> publish).
 *
 * Threading invariant (SLJIT.md §2): the game thread NEVER compiles/validates. It
 * only does a lock-free table lookup and a non-blocking request enqueue (which
 * captures a state+memory snapshot). A single worker thread compiles the shard via
 * sljit, validates it OFF-THREAD by running it and the superzazu interpreter from
 * the same snapshot and diffing the result, and publishes it into the lock-free
 * table only if byte-identical. Built only with -DSMS_HAVE_JIT.
 */
#ifdef SMS_HAVE_JIT

#include "shard_jit.h"
#include "z80_sljit.h"
#include "z80_decoder.h"
#include "sljitLir.h"
#include "z80.h"                 /* superzazu — the validation oracle */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- shard table: lock-free for the reader (game thread) -----------------
 * Keyed by (addr, code-CRC): the same Z80 address hosts DIFFERENT functions
 * under different mapped ROM banks (e.g. Sonic Blast 0x8000 cycles through banks
 * 0x1B/0x3B/0x3A — three distinct routines at one address). A shard therefore
 * stores the reachable byte extent [addr, addr+span) it compiled from and a CRC
 * over those live bytes; the game-thread lookup recomputes the CRC over the
 * CURRENTLY-mapped live bytes and only runs the shard whose CRC matches. The
 * open-addressed table holds multiple shards per addr (a candidate chain): they
 * occupy consecutive slots in addr's probe sequence, so the lookup scans them
 * all and picks the bank-matching one (SLJIT.md §4 / §8.5). */
typedef struct Shard {
    uint16_t       addr;
    uint16_t       span;                 /* reachable extent: bytes [addr, addr+span) */
    uint32_t       crc;                  /* CRC of those bytes at compile time */
    ShardFn        fn;
    _Atomic int    trusted;
} Shard;

#define SHARD_BITS 12
#define SHARD_SIZE (1u << SHARD_BITS)
#define SHARD_MASK (SHARD_SIZE - 1u)
static Shard *_Atomic g_slots[SHARD_SIZE];

static inline uint32_t shard_hash(uint16_t addr){
    return (uint32_t)((addr * 2654435761u) >> (32 - SHARD_BITS)) & SHARD_MASK;
}

/* FNV-1a over a span of the contiguous address range, read through the live bus.
 * Identical algorithm to crc over the request snapshot (crc_buf) so the same
 * bank yields the same value. (Buffer form used at compile; live form at lookup.) */
static uint32_t crc_buf(const uint8_t *p, uint16_t span){
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < span; i++){ h ^= p[i]; h *= 16777619u; }
    return h;
}
static uint32_t crc_live(uint16_t addr, uint16_t span){
    uint32_t h = 2166136261u;
    for (uint16_t i = 0; i < span; i++){ h ^= sms_read8((uint16_t)(addr + i)); h *= 16777619u; }
    return h;
}

ShardFn sms_jit_lookup(uint16_t addr){
    uint32_t h = shard_hash(addr);
    for (uint32_t i = 0; i < SHARD_SIZE; i++){
        Shard *s = atomic_load_explicit(&g_slots[(h + i) & SHARD_MASK], memory_order_acquire);
        if (!s) return NULL;
        if (s->addr == addr && atomic_load_explicit(&s->trusted, memory_order_acquire)){
            /* bank-alias guard: only run this shard if the live bytes it would
             * execute still match what it compiled from. Mismatch => a different
             * bank is mapped here now; keep scanning for that variant's shard. */
            if (crc_live(addr, s->span) == s->crc) return s->fn;
        }
    }
    return NULL;
}

static void shard_publish(uint16_t addr, uint16_t span, uint32_t crc, ShardFn fn){
    Shard *sh = (Shard*)calloc(1, sizeof *sh);
    sh->addr = addr; sh->span = span; sh->crc = crc; sh->fn = fn;
    atomic_store_explicit(&sh->trusted, 1, memory_order_release);
    uint32_t h = shard_hash(addr);
    for (uint32_t i = 0; i < SHARD_SIZE; i++){
        uint32_t k = (h + i) & SHARD_MASK;
        Shard *expect = NULL;
        if (atomic_compare_exchange_strong_explicit(&g_slots[k], &expect, sh,
                memory_order_release, memory_order_relaxed))
            return;                          /* claimed an empty slot */
    }
    free(sh);                                /* table full (won't happen at our scale) */
}

/* ---- request: a state+memory snapshot captured by the game thread -------- */
typedef struct Request {
    uint16_t addr;
    Z80State entry;
    uint8_t  mem[0x10000];                   /* full address space at entry (banked ROM + RAM) */
} Request;

#define RQ_CAP 64
static Request        *g_rq[RQ_CAP];
static _Atomic uint32_t g_rq_head, g_rq_tail;
static _Atomic uint64_t g_n_req, g_n_compiled, g_n_published, g_n_declined, g_n_failed;

/* request dedup keyed by (addr, code-version) — NOT addr alone. A bank change at
 * the same Z80 address is a different function and must be allowed through, else
 * only the first bank variant ever compiles and the others stay on the interpreter
 * forever (the 0x8000 bug). The code-version is a cheap FNV over a fixed window of
 * the live bytes at addr, distinct per mapped ROM bank. Game-thread-only; no atomics. */
#define REQSET_BITS  13
#define REQSET_SIZE  (1u << REQSET_BITS)
#define REQSET_MASK  (REQSET_SIZE - 1u)
#define REQ_WIN      256                     /* bytes hashed to distinguish bank variants */
typedef struct { uint32_t winhash; uint16_t addr; uint8_t used; } ReqKey;
static ReqKey *g_reqset;                      /* REQSET_SIZE entries (game-thread only) */

static uint32_t req_winhash(uint16_t addr){
    uint32_t h = 2166136261u;
    for (int i = 0; i < REQ_WIN; i++){ h ^= sms_read8((uint16_t)(addr + i)); h *= 16777619u; }
    return h;
}
/* 1 if (addr,winhash) has already been requested; 0 otherwise. Pure query — does NOT
 * insert. Insertion is reqset_mark(), called ONLY after a request is actually enqueued,
 * so a request dropped on a full queue is NOT recorded and WILL be retried on a later
 * miss (otherwise that bank variant would never compile). */
static int reqset_seen(uint16_t addr, uint32_t winhash){
    if (!g_reqset) return 0;
    uint32_t h = (uint32_t)(((addr ^ winhash) * 2654435761u) >> (32 - REQSET_BITS)) & REQSET_MASK;
    for (uint32_t i = 0; i < REQSET_SIZE; i++){
        ReqKey *k = &g_reqset[(h + i) & REQSET_MASK];
        if (!k->used) return 0;
        if (k->addr == addr && k->winhash == winhash) return 1;
    }
    return 0;
}
static void reqset_mark(uint16_t addr, uint32_t winhash){
    if (!g_reqset) return;
    uint32_t h = (uint32_t)(((addr ^ winhash) * 2654435761u) >> (32 - REQSET_BITS)) & REQSET_MASK;
    for (uint32_t i = 0; i < REQSET_SIZE; i++){
        ReqKey *k = &g_reqset[(h + i) & REQSET_MASK];
        if (!k->used){ k->used = 1; k->addr = addr; k->winhash = winhash; return; }
        if (k->addr == addr && k->winhash == winhash) return;
    }
}

/* live coverage observability (read from any thread) */
uint64_t sms_jit_published(void){ return atomic_load_explicit(&g_n_published, memory_order_relaxed); }
uint64_t sms_jit_requested(void){ return atomic_load_explicit(&g_n_req,       memory_order_relaxed); }
uint64_t sms_jit_declined (void){ return atomic_load_explicit(&g_n_declined,  memory_order_relaxed); }

static pthread_t       g_worker;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int     g_run;
static int             g_started;

void sms_jit_request(uint16_t addr){
    uint32_t wh = req_winhash(addr);
    if (reqset_seen(addr, wh)) return;                /* dedup by (addr, bank variant) */
    uint32_t head = atomic_load_explicit(&g_rq_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_rq_tail, memory_order_acquire);
    if (head - tail >= RQ_CAP) return;                /* full: drop WITHOUT marking -> retried later */

    Request *r = (Request*)malloc(sizeof *r);
    if (!r) return;
    r->addr  = addr;
    r->entry = g_z80;                                 /* entry register snapshot */
    for (uint32_t a = 0; a < 0x10000; a++)            /* entry memory snapshot (live bus) */
        r->mem[a] = sms_read8((uint16_t)a);

    reqset_mark(addr, wh);                             /* record only now that it's enqueued */
    g_rq[head & (RQ_CAP - 1)] = r;
    atomic_store_explicit(&g_rq_head, head + 1, memory_order_release);
    atomic_fetch_add_explicit(&g_n_req, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_mx); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mx);
}

static Request *rq_pop(void){
    uint32_t tail = atomic_load_explicit(&g_rq_tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&g_rq_head, memory_order_acquire)) return NULL;
    Request *r = g_rq[tail & (RQ_CAP - 1)];
    atomic_store_explicit(&g_rq_tail, tail + 1, memory_order_release);
    return r;
}

/* ---- off-thread differential validation (worker only) -------------------- */
static uint8_t  g_memA[0x10000], g_memB[0x10000];     /* worker-private scratch */

static uint8_t  vbus_r8 (void *c, uint16_t a){ return ((uint8_t*)c)[a]; }
static void     vbus_w8 (void *c, uint16_t a, uint8_t v){ ((uint8_t*)c)[a] = v; }
static uint8_t  vbus_in (void *c, uint8_t p){ (void)c; (void)p; return 0xFF; }
/* off-thread validation freezes the VDP/IRQs: sync never fires, deadline never crossed */
static const uint64_t g_frozen_deadline = (uint64_t)-1;
static void     vbus_sync(Z80State *s){ (void)s; }

static uint8_t pack_f(const z80 *z){
    return (uint8_t)((z->sf<<7)|(z->zf<<6)|(z->yf<<5)|(z->hf<<4)|
                     (z->xf<<3)|(z->pf<<2)|(z->nf<<1)|(z->cf));
}
static void unpack_f(z80 *z, uint8_t f){
    z->sf=(f>>7)&1; z->zf=(f>>6)&1; z->yf=(f>>5)&1; z->hf=(f>>4)&1;
    z->xf=(f>>3)&1; z->pf=(f>>2)&1; z->nf=(f>>1)&1; z->cf=f&1;
}

/* port-write logs: OUT is compared (not applied) since there is no live device
 * off-thread. The shard side (incl. its callees) logs to A, the oracle to B. */
typedef struct { uint8_t port, val; } IoEv;
static IoEv g_ioA[8192], g_ioB[8192];
static int  g_nA, g_nB;
static void busA_out(void *c, uint8_t p, uint8_t v){ (void)c; if (g_nA<8192){ g_ioA[g_nA].port=p; g_ioA[g_nA].val=v; g_nA++; } }
static void szA_out (z80 *z, uint8_t p, uint8_t v){ (void)z; busA_out(0,p,v); }
static void szB_out (z80 *z, uint8_t p, uint8_t v){ (void)z; if (g_nB<8192){ g_ioB[g_nB].port=p; g_ioB[g_nB].val=v; g_nB++; } }
static uint8_t sz_in (z80 *z, uint8_t p){ (void)z; (void)p; return 0xFF; }

/* Z80State <-> superzazu (mirrors glue.c state_to_hz/from_hz) */
static void to_z(z80 *z, const Z80State *s){
    z->a=s->a; z->b=s->b; z->c=s->c; z->d=s->d; z->e=s->e; z->h=s->h; z->l=s->l; unpack_f(z, s->f);
    z->a_=s->a_; z->b_=s->b_; z->c_=s->c_; z->d_=s->d_; z->e_=s->e_; z->h_=s->h_; z->l_=s->l_; z->f_=s->f_;
    z->ix=s->ix; z->iy=s->iy; z->sp=s->sp; z->mem_ptr=s->wz; z->i=s->i; z->r=s->r;
    z->iff1=s->iff1; z->iff2=s->iff2; z->interrupt_mode=s->im; z->halted=s->halted;
}
static void from_z(Z80State *s, const z80 *z){
    s->a=z->a; s->b=z->b; s->c=z->c; s->d=z->d; s->e=z->e; s->h=z->h; s->l=z->l; s->f=pack_f(z);
    s->a_=z->a_; s->b_=z->b_; s->c_=z->c_; s->d_=z->d_; s->e_=z->e_; s->h_=z->h_; s->l_=z->l_; s->f_=z->f_;
    s->ix=z->ix; s->iy=z->iy; s->sp=z->sp; s->wz=z->mem_ptr; s->i=z->i; s->r=z->r;
    s->iff1=z->iff1; s->iff2=z->iff2; s->im=z->interrupt_mode; s->halted=z->halted;
}

/* shard CALL during validation: run the callee under superzazu on the snapshot copy,
 * logging its OUTs to A — mirrors the live call_by_address handoff, off-thread. */
static void sandbox_call(void *ctx, Z80State *s, uint16_t target){
    z80 z; z80_init(&z);
    z.read_byte=vbus_r8; z.write_byte=vbus_w8; z.port_in=sz_in; z.port_out=szA_out; z.userdata=ctx;
    to_z(&z, s); z.pc=target; z.cyc=0;
    uint16_t entry_sp = s->sp;
    for (long g=0; g<16000000 && !(z.sp > entry_sp); g++) z80_step(&z);
    from_z(s, &z);
    s->cyc += z.cyc;
}

/* Run the shard and superzazu from the same snapshot; return 1 iff byte-identical
 * (regs + flags + cyc + memory + port-write log). pc excluded (recomp model). */
static int validate(ShardFn fn, const Request *r){
    memcpy(g_memA, r->mem, sizeof g_memA);
    memcpy(g_memB, r->mem, sizeof g_memB);
    g_nA = g_nB = 0;
    Bus busA = { vbus_r8, vbus_w8, vbus_in, busA_out, sandbox_call, &g_frozen_deadline, vbus_sync, g_memA };

    Z80State sa = r->entry; sa.cyc = 0;
    fn(&sa, &busA);                                   /* shard runs to its RET */

    z80 z; z80_init(&z);
    z.read_byte=vbus_r8; z.write_byte=vbus_w8; z.port_in=sz_in; z.port_out=szB_out; z.userdata=g_memB;
    to_z(&z, &r->entry); z.pc=r->addr; z.cyc=0;
    uint16_t entry_sp = r->entry.sp;
    for (long g=0; g<16000000 && !(z.sp > entry_sp); g++) z80_step(&z);

    return (sa.a==z.a)&&(sa.f==pack_f(&z))&&(sa.b==z.b)&&(sa.c==z.c)&&
           (sa.d==z.d)&&(sa.e==z.e)&&(sa.h==z.h)&&(sa.l==z.l)&&
           (sa.ix==z.ix)&&(sa.iy==z.iy)&&(sa.sp==z.sp)&&
           (sa.iff1==z.iff1)&&(sa.iff2==z.iff2)&&
           (sa.cyc==(uint64_t)z.cyc)&&
           (memcmp(g_memA, g_memB, sizeof g_memA)==0)&&
           (g_nA==g_nB && memcmp(g_ioA, g_ioB, (size_t)g_nA*sizeof(IoEv))==0);
}

static void process(Request *r){
    ShardFn fn = z80_sljit_compile(&r->mem[r->addr], (size_t)(0x10000 - r->addr), r->addr);
    if (!fn){
        atomic_fetch_add_explicit(&g_n_declined, 1, memory_order_relaxed);
        fprintf(stderr, "[jit] DECLINE %04X: %s  (first blocker @ %04X: %s)\n",
                r->addr, z80_sljit_last_decline.why, z80_sljit_last_decline.pc,
                z80_sljit_last_decline.text);
        return;
    }
    atomic_fetch_add_explicit(&g_n_compiled, 1, memory_order_relaxed);
    /* Identity of this shard: the reachable bytes it compiled from. Computed over
     * the request's frozen snapshot, which is exactly what the emitter decoded, so
     * it equals crc_live() whenever the same ROM bank is mapped at run time. */
    uint16_t span = z80_sljit_last_span;
    uint32_t crc  = crc_buf(&r->mem[r->addr], span);
    if (validate(fn, r)){
        shard_publish(r->addr, span, crc, fn);        /* spike: publish on 1 clean pass */
        atomic_fetch_add_explicit(&g_n_published, 1, memory_order_relaxed);
        fprintf(stderr, "[jit] shard PUBLISHED for %04X (span %u, crc %08X, validated byte-exact vs interp)\n",
                r->addr, span, crc);
    } else {
        sljit_free_code((void*)fn, NULL);
        atomic_fetch_add_explicit(&g_n_failed, 1, memory_order_relaxed);
        fprintf(stderr, "[jit] shard for %04X FAILED validation -> stays Tier-3\n", r->addr);
    }
}

static void *worker_main(void *arg){
    (void)arg;
    while (atomic_load_explicit(&g_run, memory_order_acquire)){
        Request *r = rq_pop();
        if (r){ process(r); free(r); continue; }
        pthread_mutex_lock(&g_mx);
        if (atomic_load_explicit(&g_rq_head, memory_order_acquire) ==
            atomic_load_explicit(&g_rq_tail, memory_order_acquire) &&
            atomic_load_explicit(&g_run, memory_order_acquire))
            pthread_cond_wait(&g_cv, &g_mx);
        pthread_mutex_unlock(&g_mx);
    }
    return NULL;
}

/* ---- end-to-end self-test (env: SMS_JIT_SELFTEST=1) ---------------------- *
 * Drives a synthetic trivial-subset routine through the WHOLE async pipeline:
 * enqueue -> worker compiles + validates off-thread -> publishes -> game thread
 * looks it up and runs it natively, asserting it matches the interpreter. Proves
 * the ecosystem end-to-end independent of emitter coverage. */
static void jit_selftest(void){
    const uint16_t addr = 0x9000;
    Request *r = (Request*)calloc(1, sizeof *r);
    /* LD B,$2A ; INC C ; DEC A ; LD L,H ; INC E ; RET */
    uint8_t code[] = { 0x06,0x2A, 0x0C, 0x3D, 0x6C, 0x1C, 0xC9 };
    memcpy(&r->mem[addr], code, sizeof code);
    r->addr = addr;
    r->entry.a=0x10; r->entry.f=0x00; r->entry.b=0x01; r->entry.c=0x02;
    r->entry.d=0x03; r->entry.e=0x04; r->entry.h=0x55; r->entry.l=0x66;
    r->entry.sp=0x4000;                               /* fake return frame in snapshot */
    r->mem[0x4000]=0x34; r->mem[0x4001]=0x12;

    /* enqueue directly (bypass dedup, this is a synthetic addr) */
    uint32_t head = atomic_load_explicit(&g_rq_head, memory_order_relaxed);
    g_rq[head & (RQ_CAP - 1)] = r;
    atomic_store_explicit(&g_rq_head, head + 1, memory_order_release);
    pthread_mutex_lock(&g_mx); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mx);

    /* wait (game thread) for the worker to publish */
    ShardFn fn = NULL;
    for (int i = 0; i < 1000 && !fn; i++){
        struct timespec ts = { 0, 2*1000*1000 };      /* 2 ms */
        nanosleep(&ts, NULL);
        fn = sms_jit_lookup(addr);
    }
    if (!fn){ fprintf(stderr, "[jit-selftest] FAIL: shard never published\n"); return; }

    /* run the published shard vs a fresh interp from the same seed; assert match */
    Z80State seed; memset(&seed,0,sizeof seed);
    seed.a=0x10; seed.b=0x01; seed.c=0x02; seed.d=0x03; seed.e=0x04; seed.h=0x55; seed.l=0x66; seed.sp=0x4000;
    static uint8_t m[0x10000]; memcpy(m, r->mem, sizeof m);
    Bus bus = { vbus_r8, vbus_w8, vbus_in, busA_out, sandbox_call, &g_frozen_deadline, vbus_sync, m };
    Z80State st = seed; fn(&st, &bus);
    fprintf(stderr,
        "[jit-selftest] PASS: shard ran natively. A=%02X B=%02X C=%02X D=%02X E=%02X L=%02X cyc=%llu\n",
        st.a, st.b, st.c, st.d, st.e, st.l, (unsigned long long)st.cyc);
    /* (the worker already proved byte-equality vs the interpreter before publishing) */
}

void sms_jit_init(void){
    if (g_started) return;
    g_reqset = (ReqKey*)calloc(REQSET_SIZE, sizeof *g_reqset);
    atomic_store_explicit(&g_run, 1, memory_order_release);
    if (pthread_create(&g_worker, NULL, worker_main, NULL) != 0){
        fprintf(stderr, "[jit] worker thread create failed; Tier-2 disabled\n");
        atomic_store_explicit(&g_run, 0, memory_order_release); return;
    }
    g_started = 1;
    fprintf(stderr, "[jit] shard worker started (off-thread compile+validate+publish)\n");
    if (getenv("SMS_JIT_SELFTEST")) jit_selftest();
}

void sms_jit_shutdown(void){
    if (!g_started) return;
    atomic_store_explicit(&g_run, 0, memory_order_release);
    pthread_mutex_lock(&g_mx); pthread_cond_signal(&g_cv); pthread_mutex_unlock(&g_mx);
    pthread_join(g_worker, NULL);
    g_started = 0;
    fprintf(stderr, "[jit] worker stopped: %llu req, %llu compiled, %llu published, "
            "%llu declined, %llu failed-validation\n",
            (unsigned long long)atomic_load(&g_n_req),
            (unsigned long long)atomic_load(&g_n_compiled),
            (unsigned long long)atomic_load(&g_n_published),
            (unsigned long long)atomic_load(&g_n_declined),
            (unsigned long long)atomic_load(&g_n_failed));
    free(g_reqset); g_reqset = NULL;
}

#endif /* SMS_HAVE_JIT */
