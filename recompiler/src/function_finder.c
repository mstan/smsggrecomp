/*
 * function_finder.c — static reachability + per-function trace.
 * See function_finder.h and PRINCIPLES #16.
 */
#include "function_finder.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- FuncList ---- */
void funclist_init(FuncList *fl){ fl->items=NULL; fl->count=0; fl->cap=0; }
void funclist_free(FuncList *fl){ free(fl->items); fl->items=NULL; fl->count=fl->cap=0; }

/* Any discovered entry at this address, regardless of bank (code-vs-data oracle
 * for dense jump-table bounding: a table cannot extend across known code). */
static bool funclist_has_code(const FuncList *fl, uint16_t addr){
    for (int i=0;i<fl->count;i++)
        if (fl->items[i].addr==addr && fl->items[i].is_entry) return true;
    return false;
}
int funclist_find(const FuncList *fl, uint16_t addr, int bank){
    for (int i=0;i<fl->count;i++)
        if (fl->items[i].addr==addr && fl->items[i].bank==bank) return i;
    return -1;
}
int funclist_add(FuncList *fl, uint16_t addr, int bank, const char *name,
                 uint8_t source, bool is_entry){
    int idx = funclist_find(fl, addr, bank);
    if (idx >= 0){
        fl->items[idx].source |= source;
        if (is_entry) fl->items[idx].is_entry = true;
        if (name && name[0] && fl->items[idx].name[0]=='\0')
            snprintf(fl->items[idx].name,sizeof(fl->items[idx].name),"%s",name);
        return idx;
    }
    if (fl->count==fl->cap){ fl->cap = fl->cap? fl->cap*2:64;
        fl->items=(FuncEntry*)realloc(fl->items,(size_t)fl->cap*sizeof(FuncEntry)); }
    FuncEntry *e=&fl->items[fl->count];
    memset(e,0,sizeof(*e));
    e->addr=addr; e->bank=bank; e->source=source; e->is_entry=is_entry;
    if (name && name[0]) snprintf(e->name,sizeof(e->name),"%s",name);
    else if (bank >= 0)  snprintf(e->name,sizeof(e->name),"func_%04X_b%d",addr,bank);
    else                 snprintf(e->name,sizeof(e->name),"func_%04X",addr);
    return fl->count++;
}

void ff_seed_vectors(FuncList *fl){
    funclist_add(fl, Z80_RESET_VECTOR, -1, "vec_reset", FUNC_SRC_VECTOR, true);
    funclist_add(fl, Z80_IRQ_VECTOR,   -1, "vec_irq",   FUNC_SRC_VECTOR, true); /* 0x38 (== RST 38) */
    funclist_add(fl, Z80_NMI_VECTOR,   -1, "vec_nmi",   FUNC_SRC_VECTOR, true);
    for (int i=0;i<8;i++){
        char nm[16]; snprintf(nm,sizeof(nm),"rst_%02X",Z80_RST_VECTORS[i]);
        funclist_add(fl, Z80_RST_VECTORS[i], -1, nm, FUNC_SRC_VECTOR, true);
    }
}

/* ---- tracer ---- */
static void labels_add(TraceResult *t, uint16_t a){
    for (int i=0;i<t->label_count;i++) if (t->labels[i]==a) return;
    if (t->label_count==t->label_cap){ t->label_cap=t->label_cap?t->label_cap*2:16;
        t->labels=(uint16_t*)realloc(t->labels,(size_t)t->label_cap*sizeof(uint16_t)); }
    t->labels[t->label_count++]=a;
}
static bool insns_have(TraceResult *t, uint16_t a){
    for (int i=0;i<t->insn_count;i++) if (t->insns[i].addr==a) return true;
    return false;
}
static void insns_add(TraceResult *t, uint16_t a, const Z80Insn *in){
    if (t->insn_count==t->insn_cap){ t->insn_cap=t->insn_cap?t->insn_cap*2:64;
        t->insns=(TracedInsn*)realloc(t->insns,(size_t)t->insn_cap*sizeof(TracedInsn)); }
    t->insns[t->insn_count].addr=a; t->insns[t->insn_count].insn=*in; t->insn_count++;
}
static bool is_entry_addr(const uint16_t *entries,int n,uint16_t a){
    for (int i=0;i<n;i++) if (entries[i]==a) return true;
    return false;
}

/* ---- per-slot bank tracking ---- *
 * The mapper idiom is `ld a,#n ; <stores that preserve A> ; ld ($FFFx),a`. A
 * naive "previous insn must be ld a,#n" match misses the very common case where
 * the loaded bank number is first stashed to a RAM mirror, e.g.
 *   ld a,$02 ; ld ($D118),a ; ld ($FFFE),a   (Sonic Blast GG reset)
 * so we instead carry the statically-known value of A across instructions and
 * apply it whenever a frame register is written. When A's value is not
 * statically determinable at a frame-register write, the slot bank becomes
 * runtime-determined and its targets are deferred to call_by_address. */
void bankstate_init(BankState *bs, uint16_t entry, int entry_bank){
    bs->slot[0]=0;     bs->slot[1]=1;     bs->slot[2]=2;
    bs->known[0]=true; bs->known[1]=true; bs->known[2]=false;
    bs->a_known = -1;
    int es = entry >> 14;            /* entry's slot (0/1/2) */
    if (entry_bank >= 0 && es < 3){
        bs->slot[es]  = entry_bank;
        bs->known[es] = true;
    }
}
/* True if `in` overwrites A with a value we don't model below (so A's tracked
 * value must drop to unknown). ld a,#n / inc a / dec a / xor a / sub a / and a /
 * or a are handled explicitly in bankstate_step; everything here is the rest of
 * the A-clobbering set. CP (no A write) and stores/ld r,a (A preserved) are not. */
static bool insn_clobbers_a(const Z80Insn *in){
    switch (in->prefix){
    case Z80_PFX_NONE: {
        uint8_t op = in->opcode;
        if (op >= 0x78 && op <= 0x7F) return true;       /* ld a,r / ld a,(hl)        */
        if (op >= 0x80 && op <= 0xB7) return true;        /* add..or a,r (cp 0xB8-BF: no) */
        switch (op){
            case 0x0A: case 0x1A: case 0x3A:              /* ld a,(bc)/(de)/(nn)       */
            case 0x07: case 0x0F: case 0x17: case 0x1F:   /* rlca rrca rla rra         */
            case 0x27: case 0x2F:                          /* daa cpl                   */
            case 0x08: case 0xF1:                          /* ex af,af' ; pop af        */
            case 0xC6: case 0xCE: case 0xD6: case 0xDE:   /* add/adc/sub/sbc a,n       */
            case 0xE6: case 0xEE: case 0xF6:              /* and/xor/or n (cp n 0xFE:no)*/
                return true;
        }
        return false;
    }
    case Z80_PFX_ED: {
        switch (in->opcode){
            case 0x44: case 0x4C: case 0x54: case 0x5C:   /* neg (and mirrors)         */
            case 0x64: case 0x6C: case 0x74: case 0x7C:
            case 0x57: case 0x5F:                          /* ld a,i ; ld a,r           */
            case 0x67: case 0x6F:                          /* rrd ; rld                 */
                return true;
        }
        return false;
    }
    case Z80_PFX_DD: case Z80_PFX_FD: {
        uint8_t op = in->opcode;
        if (op == 0x3E || op == 0x3C || op == 0x3D) return true;  /* ld/inc/dec a (rare) */
        if (op >= 0x78 && op <= 0x7F) return true;        /* ld a,(ix+d)/ixh/ixl       */
        if (op >= 0x80 && op <= 0xB7) return true;        /* alu a,(ix+d)/ixh/ixl      */
        return false;
    }
    case Z80_PFX_CB: {                                     /* rot/shift & res/set on A  */
        uint8_t op = in->opcode;
        if ((op & 0x07) == 0x07 && (op < 0x40 || op >= 0x80)) return true;
        return false;
    }
    default:                                               /* DDCB/FDCB act on (IX/IY+d)*/
        return false;
    }
}
static void bank_set(BankState *bs, int slot){
    if (bs->a_known >= 0){ bs->slot[slot] = bs->a_known; bs->known[slot] = true; }
    else                   bs->known[slot] = false;       /* runtime-determined: defer */
}
void bankstate_step(BankState *bs, const TracedInsn *cur, const TracedInsn *prev){
    (void)prev;
    const Z80Insn *in = &cur->insn;
    if (in->prefix == Z80_PFX_NONE){
        switch (in->opcode){
        case 0x3E: bs->a_known = (uint8_t)in->imm; break;                         /* ld a,n */
        case 0x3C: if (bs->a_known>=0) bs->a_known=(bs->a_known+1)&0xFF; break;    /* inc a  */
        case 0x3D: if (bs->a_known>=0) bs->a_known=(bs->a_known-1)&0xFF; break;    /* dec a  */
        case 0xAF: bs->a_known = 0; break;                                        /* xor a  */
        case 0x97: bs->a_known = 0; break;                                        /* sub a  */
        case 0xA7: case 0xB7: break;                          /* and a / or a: A unchanged  */
        case 0x32:                                            /* ld (nn),a: A preserved     */
            switch (in->imm){
                case 0xFFFD: bank_set(bs,0); break;
                case 0xFFFE: bank_set(bs,1); break;
                case 0xFFFF: bank_set(bs,2); break;
            }
            break;
        default:
            if (insn_clobbers_a(in)) bs->a_known = -1;
            break;
        }
    } else if (insn_clobbers_a(in)){
        bs->a_known = -1;
    }
}
bool bankstate_target(const BankState *bs, uint16_t T, int *out_bank){
    if (T >= 0xC000) return false;           /* RAM, not code */
    int slot = T >> 14;
    if (!bs->known[slot]) return false;       /* bank runtime-determined: defer */
    int b = bs->slot[slot];
    if (T < 0x0400) b = 0;                    /* fixed first 1 KB */
    *out_bank = (b == slot) ? -1 : b;         /* default mapping -> fixed (-1) */
    return true;
}

static const Z80Insn *tr_insn_at(const TraceResult *t, uint16_t a){
    for (int i=0;i<t->insn_count;i++) if (t->insns[i].addr==a) return &t->insns[i].insn;
    return NULL;
}

/* Does `in` modify any part of register pair `preg` (0=BC 1=DE 2=HL 3=AF 4=IX
 * 5=IY)?  Used to trace a pushed register back to its defining `ld rr,nn` across
 * intervening instructions: the trace stops the moment the pair is redefined by
 * anything other than that immediate load. Errs toward TRUE (stop) on ambiguous
 * forms so a stale immediate is never mistaken for a live continuation. */
static bool insn_writes_rp(const Z80Insn *in, int preg){
    uint8_t op = in->opcode;
    switch (in->prefix){
    case Z80_PFX_NONE:
        if (op==0xEB) return (preg==1 || preg==2);            /* ex de,hl   */
        if (op==0xE3) return (preg==2);                        /* ex (sp),hl */
        switch (preg){
        case 0: /* BC */ if (op==0x01||op==0x03||op==0x0B||op==0x06||op==0x0E||op==0xC1) return true;
                         return (op>=0x40 && op<=0x4F);        /* ld b,r / ld c,r */
        case 1: /* DE */ if (op==0x11||op==0x13||op==0x1B||op==0x16||op==0x1E||op==0xD1) return true;
                         return (op>=0x50 && op<=0x5F);        /* ld d,r / ld e,r */
        case 2: /* HL */ if (op==0x21||op==0x23||op==0x2B||op==0x26||op==0x2E||op==0x2A||op==0xE1) return true;
                         if (op==0x09||op==0x19||op==0x29||op==0x39) return true; /* add hl,rr */
                         return (op>=0x60 && op<=0x6F);        /* ld h,r / ld l,r */
        case 3: /* AF */ if (op==0xF1||op==0x08) return true; return insn_clobbers_a(in);
        default: return false;                                 /* unprefixed leaves IX/IY untouched */
        }
    case Z80_PFX_DD: case Z80_PFX_FD: {
        int self = (in->prefix==Z80_PFX_DD)?4:5;
        if (preg==self){
            if (op==0x21||op==0x23||op==0x2B||op==0x2A||op==0xE1) return true;
            if (op==0x09||op==0x19||op==0x29||op==0x39) return true;
            return (op==0x26||op==0x2E||(op>=0x60 && op<=0x6F));
        }
        if (preg==4||preg==5) return false;                    /* the other index reg is untouched */
        return true;                                           /* DD/FD ld/alu may hit B/C/D/E/A: conservative */
    }
    case Z80_PFX_ED:
        if (preg==0 && op==0x4B) return true;                  /* ld bc,(nn) */
        if (preg==1 && op==0x5B) return true;                  /* ld de,(nn) */
        if (preg==2 && (op==0x42||op==0x52||op==0x62||op==0x72||
                        op==0x4A||op==0x5A||op==0x6A||op==0x7A||op==0x6B||op==0x7B)) return true;
        if (op==0xA0||op==0xA1||op==0xA8||op==0xA9||
            op==0xB0||op==0xB1||op==0xB8||op==0xB9) return true; /* block ops touch BC/DE/HL */
        if (preg==3) return insn_clobbers_a(in);
        return false;
    case Z80_PFX_CB:
        if (op>=0x40 && op<=0x7F) return false;                /* BIT: no write */
        switch (op&7){ case 0: case 1: return preg==0; case 2: case 3: return preg==1;
                       case 4: case 5: return preg==2; case 7: return preg==3; default: return false; }
    default: return true;                                      /* DDCB/FDCB etc.: conservative */
    }
}
bool trace_computed_call(const TraceResult *t, uint16_t jp_addr, uint16_t *cont_addr){
    /* The Z80 synthesises `call (hl)` as `ld rr,ret ; push rr ; ... ; jp (hl)`.
     * The push is often immediately before the jp, but it can be SEPARATED from
     * it by the address computation that reloads the jump register — notably a
     * jump-table fetch: `ld hl,ret ; push hl ; ld hl,base ; add hl,bc ; ld c,(hl)
     * ; inc hl ; ld h,(hl) ; ld l,c ; jp (hl)`. So recover the continuation as
     * the 16-bit immediate that fed whatever value is still on TOP of the stack
     * at the jp (balancing push/pop over the contiguous block). */
    int ji=-1; for (int i=0;i<t->insn_count;i++) if (t->insns[i].addr==jp_addr){ ji=i; break; }
    if (ji<0) return false;
    int pend=0;                                 /* pops seen going backward, awaiting a push */
    for (int i=ji-1; i>=0; i--){
        const TracedInsn *cur=&t->insns[i], *nxt=&t->insns[i+1];
        if ((uint16_t)(cur->addr + cur->insn.length) != nxt->addr) break;  /* left the block */
        const Z80Insn *in=&cur->insn;
        if (in->cf==Z80_CF_RET) break;          /* a prior block's terminator */
        int preg=-1, ispop=0;
        if (in->prefix==Z80_PFX_NONE){
            switch (in->opcode){
                case 0xC5: preg=0; break; case 0xD5: preg=1; break;
                case 0xE5: preg=2; break; case 0xF5: preg=3; break;
                case 0xC1: case 0xD1: case 0xE1: case 0xF1: ispop=1; break;
            }
        } else if (in->prefix==Z80_PFX_DD || in->prefix==Z80_PFX_FD){
            if      (in->opcode==0xE5) preg=(in->prefix==Z80_PFX_DD)?4:5;
            else if (in->opcode==0xE1) ispop=1;
        }
        if (ispop){ pend++; continue; }
        if (preg>=0){
            if (pend>0){ pend--; continue; }    /* this push is popped before the jp */
            /* Top-of-stack push of `preg`: trace it back to the `ld preg,nn16`
             * that produced the value, skipping instructions that don't write
             * preg (e.g. the `jp` between `ld hl,ret` and `push hl` in a
             * table-dispatch trampoline). Stop if preg is otherwise redefined. */
            for (int j=i-1; j>=0; j--){
                const Z80Insn *ld=&t->insns[j].insn;
                int lr = (ld->prefix==Z80_PFX_NONE)
                       ? (ld->opcode==0x01?0:ld->opcode==0x11?1:ld->opcode==0x21?2:-1)
                       : (((ld->prefix==Z80_PFX_DD||ld->prefix==Z80_PFX_FD)&&ld->opcode==0x21)
                           ?(ld->prefix==Z80_PFX_DD?4:5):-1);
                if (lr==preg && ld->imm_bits==16){ *cont_addr=ld->imm; return true; }
                if (insn_writes_rp(ld, preg)) break;  /* redefined by a non-immediate */
            }
            return false;                       /* couldn't resolve to an immediate */
        }
    }
    return false;
}
/* True for the computed-jump opcodes JP (HL)/(IX)/(IY). */
static bool is_computed_jp(const Z80Insn *in){
    return in->cf==Z80_CF_JUMP && !in->has_target && in->opcode==0xE9;
}

/* ---- data intervals (accepted jump tables) the tracer must NOT decode as code.
 * A jump table's pointer bytes often sit between the dispatcher and its first
 * target (idiom A); without this, a fall-through or mis-aligned trace would
 * decode the pointer data as instructions. Populated by jump-table detection. */
static struct { uint16_t lo, hi; int bank; } g_data_iv[512];
static int g_data_iv_n;
static void mark_data(uint16_t lo, uint16_t hi, int bank){
    for (int i=0;i<g_data_iv_n;i++)
        if (g_data_iv[i].lo==lo && g_data_iv[i].hi==hi && g_data_iv[i].bank==bank) return;
    if (g_data_iv_n < (int)(sizeof(g_data_iv)/sizeof(g_data_iv[0]))){
        g_data_iv[g_data_iv_n].lo=lo; g_data_iv[g_data_iv_n].hi=hi;
        g_data_iv[g_data_iv_n].bank=bank; g_data_iv_n++;
    }
}
static bool in_data_iv(uint16_t a, int bank){
    for (int i=0;i<g_data_iv_n;i++)
        if (a>=g_data_iv[i].lo && a<g_data_iv[i].hi &&
            (g_data_iv[i].bank<0 || bank<0 || g_data_iv[i].bank==bank)) return true;
    return false;
}

void trace_function(const SmsRom *rom, uint16_t start, int bank,
                    const uint16_t *entries, int entry_count, TraceResult *out){
    memset(out,0,sizeof(*out));
    out->start=start;
    int start_slot = start >> 14;
    /* simple worklist of pending addresses within this function */
    uint16_t stack[4096]; int sp=0;
    stack[sp++]=start;
    while (sp>0){
        uint16_t pc=stack[--sp];
        if (insns_have(out,pc)) continue;
        /* ROM bank for pc's slot: addresses in the entry's own slot read under
         * the entry bank (so e.g. a bank-3 slot-1 function at $4006 decodes ROM
         * $C006); other slots use the default reset mapping (slot i = bank i). */
        int slot = pc >> 14;
        int slot_bank = (slot == start_slot && bank >= 0) ? bank : slot;
        if (in_data_iv(pc, slot_bank)){ out->truncated=true; continue; }  /* jump-table data, not code */
        size_t off = rom_z80_to_offset(rom, pc, slot_bank);
        if (off==SIZE_MAX || off>=rom->size){ out->truncated=true; continue; }
        uint8_t buf[4];
        for (int k=0;k<4;k++) buf[k]=rom_read_offset(rom,off+(size_t)k);
        Z80Insn in;
        z80_decode(buf,4,pc,&in);
        if (in.illegal){ out->truncated=true; continue; }
        insns_add(out,pc,&in);

        /* intra-function jumps -> labels + follow; external targets ignored */
        if ((in.cf==Z80_CF_JUMP || in.cf==Z80_CF_JUMP_COND) && in.has_target){
            if (!is_entry_addr(entries,entry_count,in.target)){
                labels_add(out,in.target);
                if (sp<(int)(sizeof(stack)/sizeof(stack[0]))) stack[sp++]=in.target;
            }
        }
        /* computed CALL (push ret; jp (hl)) -> follow the continuation */
        if (is_computed_jp(&in)){
            uint16_t cont;
            if (trace_computed_call(out, pc, &cont) &&
                !is_entry_addr(entries,entry_count,cont)){
                labels_add(out,cont);
                if (sp<(int)(sizeof(stack)/sizeof(stack[0]))) stack[sp++]=cont;
            }
        }
        /* fall-through */
        if (z80_cf_falls_through(in.cf) && !in.is_halt){
            uint16_t next=(uint16_t)(pc+in.length);
            if (sp<(int)(sizeof(stack)/sizeof(stack[0]))) stack[sp++]=next;
        } else if (in.is_halt){
            uint16_t next=(uint16_t)(pc+in.length);
            if (sp<(int)(sizeof(stack)/sizeof(stack[0]))) stack[sp++]=next;
        }
    }
    /* sort insns by address (simple insertion sort; functions are small) */
    for (int i=1;i<out->insn_count;i++){
        TracedInsn key=out->insns[i]; int j=i-1;
        while (j>=0 && out->insns[j].addr>key.addr){ out->insns[j+1]=out->insns[j]; j--; }
        out->insns[j+1]=key;
    }
}
void trace_free(TraceResult *t){ free(t->insns); free(t->labels); memset(t,0,sizeof(*t)); }
bool trace_is_label(const TraceResult *t, uint16_t addr){
    for (int i=0;i<t->label_count;i++) if (t->labels[i]==addr) return true;
    return false;
}

/* ---- discovery: collect all CALL/tail-jump targets as entries ---- */
static bool in_blacklist(const uint16_t *bl,int n,uint16_t a){
    for (int i=0;i<n;i++) if (bl[i]==a) return true;
    return false;
}

/* ===================== automatic jump-table detection ===================== *
 * Z80 indexed dispatch ends in `jp (hl)` after reading a 16-bit LE pointer from
 * a table: `... ld rr,base ; ... add hl,rr ; ld r,(hl) ; inc hl ; ld h,(hl) ;
 * ld l,r ; jp (hl)`. We recover (base,stride,index-range) by symbolically
 * simulating the dispatch basic block, then read exactly the number of entries
 * the index range PROVES; if the range can't be proven we fall back to the
 * dense physical-layout count = (first_forward_target - base)/stride. Bounding
 * by the index DOMAIN (not target validity) is essential on Z80 — almost every
 * byte decodes, so "walk until illegal target" never terminates. */
#define JT_HARDCAP 256
#define JT_MIN_ENTRIES 2

typedef struct {
    bool     matched;   /* dispatch idiom recognized; base/iscale valid */
    uint16_t base;      /* table base to read entries from (index lower bound folded in) */
    int      iscale;    /* index scale (bytes of address per index unit): hl = base+index*iscale */
    bool     proven;    /* index range proven -> count exact */
    int      count;     /* proven entry count */
} JtDetect;
#define JT_STRIDE 2     /* 16-bit LE pointer per entry (always, for this idiom) */

/* The 4-instruction pointer-load tail before `jp (hl)` at insns[k]; returns the
 * index of the `ld r,(hl)` (where HL == table pointer), or -1 if not the idiom. */
static int jt_terminal_load(const TraceResult *t, int k){
    if (k < 4) return -1;
    const TracedInsn *jp=&t->insns[k], *l1=&t->insns[k-1], *l2=&t->insns[k-2];
    const TracedInsn *l3=&t->insns[k-3], *l4=&t->insns[k-4];
    if (l1->insn.prefix||l2->insn.prefix||l3->insn.prefix||l4->insn.prefix) return -1;
    if ((uint16_t)(l4->addr+l4->insn.length)!=l3->addr) return -1;
    if ((uint16_t)(l3->addr+l3->insn.length)!=l2->addr) return -1;
    if ((uint16_t)(l2->addr+l2->insn.length)!=l1->addr) return -1;
    if ((uint16_t)(l1->addr+l1->insn.length)!=jp->addr) return -1;
    if (l3->insn.opcode!=0x23) return -1;       /* inc hl   */
    if (l2->insn.opcode!=0x66) return -1;       /* ld h,(hl)*/
    uint8_t lo=l1->insn.opcode, hi=l4->insn.opcode;  /* ld l,r  / matching ld r,(hl) */
    bool pair=(lo==0x6F&&hi==0x7E)||(lo==0x69&&hi==0x4E)||(lo==0x68&&hi==0x46)||
              (lo==0x6A&&hi==0x56)||(lo==0x6B&&hi==0x5E);
    return pair ? k-4 : -1;
}

/* Symbolically simulate the dispatch basic block to recover base+index*stride
 * and the proven index range. */
static JtDetect detect_jump_table(const TraceResult *t, int k){
    JtDetect r; memset(&r,0,sizeof r);
    int loadidx = jt_terminal_load(t, k);
    if (loadidx < 0) return r;
    /* Walk back over the contiguous instruction run feeding the dispatch (a
     * bounded window; not stopping at intra-block labels — the index commonly
     * survives a `jr`/`ex af,af'` between its load and the table fetch). The
     * forward sim starts from all-unknown, so only the most recent defs of the
     * base/index matter; the dense self-bound + target guards catch any noise. */
    int bstart = loadidx;
    int window = 0;
    while (bstart > 0 && window < 64){
        const TracedInsn *p=&t->insns[bstart-1], *c=&t->insns[bstart];
        if ((uint16_t)(p->addr+p->insn.length)!=c->addr) break;   /* not contiguous */
        bstart--; window++;
    }
    /* HL = base + (has_index ? index*scale : 0) */
    struct { bool known, has_index; int scale; uint16_t base; } hl = {0,0,0,0};
    bool de_c=false, bc_c=false; uint16_t de_v=0, bc_v=0;
    bool c_idx=false, b_zero=false, e_idx=false, d_zero=false, h_zero=false;
    bool a_idx=false; int ilo=0, ihi=255; bool ibounded=false;
    bool ap_idx=false; int ap_lo=0, ap_hi=255; bool ap_bounded=false;  /* shadow AF' */
    bool has_cp=false; int cpv=0;
    for (int j=bstart;j<loadidx;j++){
        const Z80Insn *in=&t->insns[j].insn;
        if (in->prefix!=Z80_PFX_NONE){ if (insn_clobbers_a(in)) a_idx=false; continue; }
        uint8_t op=in->opcode;
        switch (op){
        case 0x08: {                                              /* ex af,af' : swap index status */
            bool ti=a_idx; int tl=ilo,th=ihi; bool tb=ibounded;
            a_idx=ap_idx; ilo=ap_lo; ihi=ap_hi; ibounded=ap_bounded;
            ap_idx=ti; ap_lo=tl; ap_hi=th; ap_bounded=tb; has_cp=false; break;
        }
        case 0x3A: case 0x0A: case 0x1A: case 0x7E:                /* ld a,(...) : load index */
            a_idx=true; ilo=0; ihi=255; ibounded=false; has_cp=false; break;
        case 0x3E: a_idx=false; has_cp=false; break;              /* ld a,n */
        case 0xFE: has_cp=true; cpv=(uint8_t)in->imm; break;      /* cp n */
        case 0x30: case 0xD2:                                     /* jr/jp nc : fallthrough A<cpv */
            if (has_cp && a_idx){ if (cpv-1<ihi) ihi=cpv-1; ibounded=true; } has_cp=false; break;
        case 0x38: case 0xDA:                                     /* jr/jp c : fallthrough A>=cpv */
            if (has_cp && a_idx){ if (cpv>ilo) ilo=cpv; } has_cp=false; break;
        case 0xD6: if (a_idx){ ilo-=(uint8_t)in->imm; ihi-=(uint8_t)in->imm; if (ilo<0) ilo=0; } break; /* sub n */
        case 0xE6: if (a_idx){ ilo=0; ihi=(uint8_t)in->imm; ibounded=true; } break;   /* and n */
        case 0x3D: if (a_idx){ ilo--; ihi--; if (ilo<0) ilo=0; } break;               /* dec a */
        case 0x3C: if (a_idx){ ilo++; ihi++; } break;                                 /* inc a */
        case 0x87: a_idx=false; break;                            /* add a,a : scaling unmodeled -> bail safe */
        case 0x4F: c_idx=a_idx; break;                            /* ld c,a */
        case 0x06: b_zero=((uint8_t)in->imm==0); break;           /* ld b,n */
        case 0x5F: e_idx=a_idx; break;                            /* ld e,a */
        case 0x16: d_zero=((uint8_t)in->imm==0); break;           /* ld d,n */
        case 0x26: h_zero=((uint8_t)in->imm==0); break;           /* ld h,n */
        case 0x6F: if (h_zero&&a_idx){ hl.known=1;hl.has_index=1;hl.scale=1;hl.base=0; } else hl.known=0; break; /* ld l,a */
        case 0x69: if (h_zero&&c_idx){ hl.known=1;hl.has_index=1;hl.scale=1;hl.base=0; } else hl.known=0; break; /* ld l,c */
        case 0x21: hl.known=1; hl.has_index=0; hl.scale=0; hl.base=in->imm; break;     /* ld hl,nn */
        case 0x11: de_c=1; de_v=in->imm; e_idx=0; break;                               /* ld de,nn */
        case 0x01: bc_c=1; bc_v=in->imm; c_idx=0; b_zero=((in->imm>>8)==0); break;     /* ld bc,nn */
        case 0x29: if (hl.known){ hl.base=(uint16_t)(hl.base*2); hl.scale*=2; } break; /* add hl,hl */
        case 0x19: if (hl.known){ if (e_idx&&d_zero){ hl.has_index=1; hl.scale+=1; }   /* add hl,de */
                                  else if (de_c) hl.base=(uint16_t)(hl.base+de_v); else hl.known=0; } break;
        case 0x09: if (hl.known){ if (c_idx&&b_zero){ hl.has_index=1; hl.scale+=1; }   /* add hl,bc */
                                  else if (bc_c) hl.base=(uint16_t)(hl.base+bc_v); else hl.known=0; } break;
        case 0x23: if (hl.known) hl.base++; break;                /* inc hl */
        case 0x2B: if (hl.known) hl.base--; break;                /* dec hl */
        default: if (insn_clobbers_a(in)) a_idx=false; break;
        }
    }
    if (!hl.known || !hl.has_index || hl.scale<1) return r;
    r.matched=true; r.base=hl.base; r.iscale=hl.scale;
    /* Entries are JT_STRIDE bytes apart; an index unit advances the address by
     * hl.scale bytes, so it steps (JT_STRIDE/hl.scale) index units per entry.
     * Proven count = number of entries the index range spans. */
    if (ibounded && ihi>=ilo && ihi<256){
        int span = (ihi-ilo)*hl.scale;                   /* address span of the range */
        if (span>=0 && (span % JT_STRIDE)==0){
            int cnt = span/JT_STRIDE + 1;
            if (cnt>=JT_MIN_ENTRIES && cnt<=JT_HARDCAP){
                r.base   = (uint16_t)(hl.base + ilo*hl.scale);  /* fold lower bound into base */
                r.count  = cnt;
                r.proven = true;
            }
        }
    }
    return r;
}

/* Read a table's entries and seed each target as a function entry. Returns the
 * number of newly-seeded entries. */
static int ff_seed_jt(const SmsRom *rom, FuncList *fl, const TraceResult *t, int k,
                      const BankState *bs, const uint16_t *bl, int bln){
    JtDetect d = detect_jump_table(t, k);
    if (!d.matched || d.iscale<1) return 0;
    if (d.base >= 0xC000) return 0;                       /* table in RAM: bail */
    int tslot = d.base >> 14;
    if (!bs->known[tslot]) return 0;                      /* table bank runtime-unknown */
    int tbank = (d.base < 0x0400) ? 0 : bs->slot[tslot];  /* bank holding the table bytes */

    uint16_t ptr[JT_HARDCAP]; int navail=0;
    uint16_t min_fwd=0xFFFF; bool have_fwd=false;
    uint16_t code_end=0; bool have_code_end=false;        /* first known-code addr > base */
    int want = d.proven ? d.count : JT_HARDCAP;
    for (int e=0; e<want && e<JT_HARDCAP; e++){
        uint16_t ea=(uint16_t)(d.base + e*JT_STRIDE);     /* 16-bit pointers: stride 2 */
        if (!d.proven && e>0 && have_fwd && ea >= min_fwd) break;  /* dense: reached first target */
        /* dense: the table cannot extend across an address that is itself known
         * code (a discovered dispatcher/handler) — common when pointer tables
         * are interleaved with code. Bound the table there. */
        if (!d.proven && e>0 && funclist_has_code(fl, ea)){ code_end=ea; have_code_end=true; break; }
        size_t off=rom_z80_to_offset(rom, ea, tbank);
        if (off==SIZE_MAX || off+1>=rom->size) break;
        uint16_t T=(uint16_t)(rom_read_offset(rom,off) | (rom_read_offset(rom,off+1)<<8));
        ptr[e]=T; navail=e+1;
        if ((T>>14)==tslot && T>d.base && T<min_fwd){ min_fwd=T; have_fwd=true; }
    }
    int count = d.proven ? d.count : 0;
    if (!d.proven){
        uint16_t bound = min_fwd;
        if (have_code_end && (!have_fwd || code_end < bound)) bound = code_end;  /* code wins if nearer */
        if (bound==0xFFFF || bound<=d.base) return 0;     /* no self-bound -> not a dense table */
        count = (bound - d.base)/JT_STRIDE;
        if (count<JT_MIN_ENTRIES || count>JT_HARDCAP) return 0;
    }
    if (count > navail) count = navail;
    /* A proven index bound (e.g. `cp E4`) can claim more entries than physically
     * fit: the first forward target is the start of handler CODE, so the table
     * cannot extend into or past it without overlapping that handler. Clamp the
     * entry count there so the handler is never mis-marked as table data. (Dense
     * tables already bound at min_fwd, so this only ever tightens proven ones.) */
    if (have_fwd){
        int maxcount = (int)(((uint16_t)(min_fwd - d.base)) / JT_STRIDE);
        if (maxcount < count) count = maxcount;
    }
    if (count < JT_MIN_ENTRIES) return 0;

    int seeded=0;
    for (int e=0;e<count;e++){
        uint16_t T=ptr[e];
        if (T>=0xC000) continue;                          /* RAM target: not code */
        if (in_blacklist(bl,bln,T)) continue;
        int b;
        if (!bankstate_target(bs,T,&b)) continue;         /* defer runtime-unknown bank */
        if (funclist_find(fl,T,b)<0){ funclist_add(fl,T,b,NULL,FUNC_SRC_TABLE,true); seeded++; }
    }
    mark_data(d.base, (uint16_t)(d.base + count*JT_STRIDE), tbank);  /* table bytes are data, not code */
    if (seeded>0)
        printf("[SmsRecomp]   jt @%04X: base=%04X bank=%d count=%d %s (+%d new)\n",
               t->insns[k].addr, d.base, tbank, count,
               d.proven?"proven":"dense", seeded);
    return seeded;
}

void ff_discover(const SmsRom *rom, FuncList *fl,
                 const uint16_t *blacklist, int blacklist_count){
    /* Iterate to a fixpoint: trace every known entry, harvest CALL targets as
     * new entries, repeat until no new entries appear. */
    int jt_seeded = 0;
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 64){
        changed = false;
        /* Rebuild the table-data registry each pass: as more code is discovered,
         * dense table bounds shrink (stopping at newly-known code), so the marks
         * must be re-derived rather than accreted. The final pass is correct. */
        g_data_iv_n = 0;
        jt_seeded = 0;
        /* snapshot current entry addresses for the "external" set */
        int n = fl->count;
        uint16_t *entries = (uint16_t*)malloc((size_t)(n>0?n:1)*sizeof(uint16_t));
        for (int i=0;i<n;i++) entries[i]=fl->items[i].addr;

        for (int i=0;i<n;i++){
            if (!fl->items[i].is_entry) continue;
            uint16_t start = fl->items[i].addr;
            int bank = fl->items[i].bank;
            if (in_blacklist(blacklist,blacklist_count,start)) continue;
            TraceResult tr;
            trace_function(rom, start, bank, entries, n, &tr);
            /* Track all three slot banks via the contiguous
             * `ld a,#imm ; ld ($FFFD/$FFFE/$FFFF),a` idiom (linear address-order
             * approximation; the runtime dispatch-miss loop resolves anything we
             * can't see statically). */
            BankState bs; bankstate_init(&bs, start, bank);
            for (int k=0;k<tr.insn_count;k++){
                bankstate_step(&bs, &tr.insns[k], k>0 ? &tr.insns[k-1] : NULL);
                const Z80Insn *in=&tr.insns[k].insn;
                /* computed jp (hl): a jump-table dispatch (not the push;jp(hl)
                 * computed-CALL idiom) -> auto-detect the table + seed targets. */
                if (is_computed_jp(in)){
                    uint16_t cont;
                    if (!trace_computed_call(&tr, tr.insns[k].addr, &cont)){
                        int s = ff_seed_jt(rom, fl, &tr, k, &bs, blacklist, blacklist_count);
                        if (s>0){ jt_seeded += s; changed=true; }
                    }
                }
                bool is_call = (in->cf==Z80_CF_CALL || in->cf==Z80_CF_CALL_COND);
                bool is_tailjump = (in->cf==Z80_CF_JUMP && in->has_target &&
                                    !trace_is_label(&tr,in->target) &&
                                    !is_entry_addr(entries,n,in->target));
                if (!(is_call || is_tailjump) || !in->has_target) continue;
                uint16_t T=in->target;
                if (in_blacklist(blacklist,blacklist_count,T)) continue;
                int tbank;
                if (!bankstate_target(&bs, T, &tbank)) continue;  /* RAM / defer */
                uint8_t src = is_call ? FUNC_SRC_CALL : FUNC_SRC_JUMP;
                if (funclist_find(fl,T,tbank)<0){
                    funclist_add(fl,T,tbank,NULL,src,true);
                    changed=true;
                }
            }
            trace_free(&tr);
        }
        free(entries);
    }
    if (g_data_iv_n || jt_seeded)
        printf("[SmsRecomp] auto jump-tables: %d table(s), %d target(s) seeded\n",
               g_data_iv_n, jt_seeded);
}
