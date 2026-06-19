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

/* ---- per-slot bank tracking ---- */
void bankstate_init(BankState *bs, uint16_t entry, int entry_bank){
    bs->slot[0]=0; bs->slot[1]=1; bs->slot[2]=2;
    bs->slot2_known=false;
    int es = entry >> 14;            /* entry's slot (0/1/2) */
    if (entry_bank >= 0 && es < 3){
        bs->slot[es] = entry_bank;
        if (es == 2) bs->slot2_known = true;
    }
}
void bankstate_step(BankState *bs, const TracedInsn *cur, const TracedInsn *prev){
    if (cur->insn.prefix==Z80_PFX_NONE && cur->insn.opcode==0x32 && prev &&
        prev->insn.prefix==Z80_PFX_NONE && prev->insn.opcode==0x3E &&
        (uint16_t)(prev->addr + prev->insn.length) == cur->addr){
        int n = (uint8_t)prev->insn.imm;
        switch (cur->insn.imm){
            case 0xFFFD: bs->slot[0]=n; break;
            case 0xFFFE: bs->slot[1]=n; break;
            case 0xFFFF: bs->slot[2]=n; bs->slot2_known=true; break;
        }
    }
}
bool bankstate_target(const BankState *bs, uint16_t T, int *out_bank){
    if (T >= 0xC000) return false;           /* RAM, not code */
    int slot = T >> 14;
    if (slot == 2){
        if (!bs->slot2_known) return false;  /* defer to runtime */
        *out_bank = bs->slot[2];             /* slot 2 is always explicit */
        return true;
    }
    int b = bs->slot[slot];
    if (T < 0x0400) b = 0;                    /* fixed first 1 KB */
    *out_bank = (b == slot) ? -1 : b;         /* default mapping -> fixed (-1) */
    return true;
}

static const Z80Insn *tr_insn_at(const TraceResult *t, uint16_t a){
    for (int i=0;i<t->insn_count;i++) if (t->insns[i].addr==a) return &t->insns[i].insn;
    return NULL;
}
bool trace_computed_call(const TraceResult *t, uint16_t jp_addr, uint16_t *cont_addr){
    /* idiom: ld rr,nn (3B) @ jp-4 ; push rr (1B) @ jp-1 ; jp (hl/ix/iy) @ jp */
    const Z80Insn *push = tr_insn_at(t, (uint16_t)(jp_addr - 1));
    const Z80Insn *ld   = tr_insn_at(t, (uint16_t)(jp_addr - 4));
    if (!push || !ld) return false;
    if (push->prefix != Z80_PFX_NONE || ld->prefix != Z80_PFX_NONE) return false;
    int pr = (push->opcode==0xC5)?0 : (push->opcode==0xD5)?1 : (push->opcode==0xE5)?2 : -1;
    int lr = (ld->opcode==0x01)?0   : (ld->opcode==0x11)?1   : (ld->opcode==0x21)?2   : -1;
    if (pr < 0 || pr != lr || ld->imm_bits != 16) return false;
    *cont_addr = ld->imm;
    return true;
}
/* True for the computed-jump opcodes JP (HL)/(IX)/(IY). */
static bool is_computed_jp(const Z80Insn *in){
    return in->cf==Z80_CF_JUMP && !in->has_target && in->opcode==0xE9;
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

void ff_discover(const SmsRom *rom, FuncList *fl,
                 const uint16_t *blacklist, int blacklist_count){
    /* Iterate to a fixpoint: trace every known entry, harvest CALL targets as
     * new entries, repeat until no new entries appear. */
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 64){
        changed = false;
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
}
