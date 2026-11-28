/*
 * code_generator.c - Z80 -> C emission for the SMS/GG recompiler.
 *
 * cg_probe: coverage report (what opcodes/CF shapes the ROM actually uses).
 * cg_emit:  the real translator - every discovered function becomes a C
 *           function; every instruction becomes faithful C built on the
 *           verified z80_ops.h semantic core. There are NO stubs: an opcode
 *           the emitter cannot translate is a hard generation error
 *           (cg_fail -> exit), per PRINCIPLES #12.
 *
 * Model (matches the nesrecomp/genesis sibling model):
 *   - One C function per (bank,addr) entry; intra-function jumps are C `goto`
 *     to L_<addr> labels; tail jumps to other entries are `callee(); return;`.
 *   - CALL pushes the Z80 return address onto the modeled stack and C-calls the
 *     callee; RET pops it and `return;`s. So s->sp stays the authentic Z80 SP
 *     (POP-the-return-address idioms work) while the C call stack mirrors the
 *     Z80 call graph.
 *   - Computed transfers (JP (HL)/(IX)/(IY), unknown targets) route through
 *     call_by_address(), which the runner resolves (generated function or
 *     hybrid interpreter) and which logs a dispatch miss on the unknown path.
 *   - Per-instruction T-states accumulate into s->cyc (drives line/frame
 *     timing in the runner).
 */
#include "code_generator.h"
#include "z80_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define CG_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define CG_MKDIR(p) mkdir(p, 0755)
#endif

/* ======================================================================== *
 *  cg_probe - coverage report (unchanged)                                  *
 * ======================================================================== */

/* histogram key: prefix (0..6) << 8 | opcode */
static inline int hkey(const Z80Insn *in){ return (in->prefix << 8) | in->opcode; }

static const char *PFX[] = {"--","CB","ED","DD","FD","DDCB","FDCB"};

void cg_probe(const SmsRom *rom, const FuncList *fl){
    int n = fl->count;
    uint16_t *entries = (uint16_t*)malloc((size_t)(n>0?n:1)*sizeof(uint16_t));

    static int hist[7*256]; memset(hist,0,sizeof(hist));
    long total_insn=0, total_bytes=0;
    int  pfx_count[7]={0};
    int  n_call=0, n_call_cond=0, n_ret=0, n_jump=0, n_jump_cond=0;
    int  n_computed_jump=0;
    int  n_truncated=0, n_illegal=0;
    int  traced_funcs=0;
    uint16_t maxaddr=0;

    for (int i=0;i<n;i++){
        if (!fl->items[i].is_entry) continue;
        TraceResult tr;
        int visible=ff_visible_entries(fl,fl->items[i].addr,fl->items[i].bank,entries,n);
        trace_function(rom, fl->items[i].addr, fl->items[i].bank, entries, visible, &tr);
        traced_funcs++;
        if (tr.truncated) n_truncated++;
        for (int k=0;k<tr.insn_count;k++){
            const Z80Insn *in=&tr.insns[k].insn;
            hist[hkey(in)]++;
            pfx_count[in->prefix]++;
            total_insn++; total_bytes += in->length;
            if (tr.insns[k].addr > maxaddr) maxaddr = tr.insns[k].addr;
            if (in->illegal) n_illegal++;
            switch (in->cf){
                case Z80_CF_CALL: n_call++; break;
                case Z80_CF_CALL_COND: n_call_cond++; break;
                case Z80_CF_RET: case Z80_CF_RET_COND: n_ret++; break;
                case Z80_CF_JUMP: if (!in->has_target) n_computed_jump++; else n_jump++; break;
                case Z80_CF_JUMP_COND: n_jump_cond++; break;
                default: break;
            }
        }
        trace_free(&tr);
    }
    free(entries);

    int distinct=0; for (int k=0;k<7*256;k++) if (hist[k]) distinct++;

    int bankhist[64]={0}, fixed_entries=0;
    for (int i=0;i<n;i++){
        if (!fl->items[i].is_entry) continue;
        int b=fl->items[i].bank;
        if (b<0) fixed_entries++; else if (b<64) bankhist[b]++;
    }

    printf("\n===== coverage probe =====\n");
    printf("functions (entries):   %d\n", n);
    printf("entries by bank:       fixed(0/1)=%d", fixed_entries);
    for (int b=0;b<64;b++) if (bankhist[b]) printf(" b%d=%d", b, bankhist[b]);
    printf("\n");
    printf("functions traced:      %d\n", traced_funcs);
    printf("instructions decoded:  %ld  (%ld bytes)\n", total_insn, total_bytes);
    printf("distinct (pfx,opcode): %d\n", distinct);
    printf("max code address seen: 0x%04X\n", maxaddr);
    printf("prefix breakdown:      ");
    for (int p=0;p<7;p++) if (pfx_count[p]) printf("%s=%d ", PFX[p], pfx_count[p]);
    printf("\n");
    printf("control flow:          CALL=%d CALLcc=%d RET=%d JP/JR=%d JPcc/JRcc=%d computed-JP=%d\n",
           n_call, n_call_cond, n_ret, n_jump, n_jump_cond, n_computed_jump);
    printf("decode issues:         truncated-funcs=%d illegal-insns=%d\n", n_truncated, n_illegal);

    printf("\ntop opcodes:\n");
    for (int rank=0; rank<20; rank++){
        int best=-1, bestv=0;
        for (int k=0;k<7*256;k++) if (hist[k]>bestv){ bestv=hist[k]; best=k; }
        if (best<0) break;
        printf("  %-4s %02X  x%d\n", PFX[best>>8], best&0xFF, bestv);
        hist[best]=0;
    }
    printf("==========================\n\n");
}

/* ======================================================================== *
 *  cg_emit - the Z80 -> C translator                                       *
 * ======================================================================== */

static const FuncList   *G_FL;   /* function table (target name lookups) */
static const TraceResult*G_TR;   /* trace of the function being emitted  */
static const BankState  *G_BS;   /* live slot-bank state at the current insn */

/* Hard generation error - never emit a stub (PRINCIPLES #12). */
static void cg_fail(const Z80Insn *in){
    fprintf(stderr,
        "[cg_emit] FATAL: unhandled opcode (pfx=%d op=0x%02X \"%s\") - "
        "no stub allowed (PRINCIPLES #12). Fix the emitter.\n",
        in->prefix, in->opcode, in->text);
    exit(3);
}

/* Function and bank assembly use the provider's instruction services. */
static const char *cond(int cc){return z80_condition_expression((unsigned)cc);}
static void emit_op(FILE *o, FILE *scratch, const Z80Insn *in){
    if(z80_emit_operation_body_with_scratch(o,scratch,in)!=Z80_EMIT_OK)cg_fail(in);
}

/* ---- target resolution ---- */
static bool tr_has(uint16_t a){
    for (int i=0;i<G_TR->insn_count;i++) if (G_TR->insns[i].addr==a) return true;
    return false;
}
/* Distinct runtime banks for which an entry exists at address T (a -1/fixed
 * entry maps to the slot's default bank). >1 means the address is bank-
 * MULTIPLEXED: different code runs depending on the live slot bank. */
static int target_runtime_bank_count(uint16_t T){
    int rbs[64], n=0;
    for (int i=0;i<G_FL->count;i++){
        if (!G_FL->items[i].is_entry || G_FL->items[i].addr!=T) continue;
        int b = G_FL->items[i].bank;
        int rb = (b >= 0) ? b : (T >> 14);
        bool dup=false; for (int e=0;e<n;e++) if (rbs[e]==rb){ dup=true; break; }
        if (!dup && n < 64) rbs[n++]=rb;
    }
    return n;
}
static const char *name_for_target(uint16_t T){
    /* A single discovered variant does not prove the live mapping. A later
     * profile can expose an address used by several banks; even before that,
     * an unobserved bank must fall back rather than execute another bank's C.
     * Keep all pageable call/tail targets behind the generated live-bank
     * dispatch. Only the mapper's fixed first 1 KiB permits a direct call. */
    if (T >= 0x0400) return NULL;
    /* A bank-multiplexed address must dispatch on the LIVE slot bank at runtime,
     * not on the bank our static walk happened to infer at this site: the same
     * call site can be reached with different banks, so a direct call to one
     * variant would miscompile. Defer to call_by_address (which switches on
     * sms_slot_bank) — still fully static, and correct for every invocation. */
    if (target_runtime_bank_count(T) > 1) return NULL;
    /* Resolve under the call site's bank context first (so a banked call like
     * `ld ($FFFE),3 ; call $4006` binds func_4006_b3, not the bank-1 reading). */
    int idx = -1;
    if (G_BS){
        int bank;
        if (bankstate_target(G_BS, T, &bank))
            idx = funclist_find(G_FL, T, bank);
    }
    /* An entry in a different bank is never a substitute for an unresolved
     * physical target. Let live-bank dispatch handle missing/unknown mappings. */
    if (idx>=0 && G_FL->items[idx].is_entry) return G_FL->items[idx].name;
    return NULL;
}
/* A native call is legal only under the live bank. The reference build uses
 * the generic dispatcher; the native build bypasses its address switch while
 * retaining the same bank guard and exact unknown-target fallback. */
static void format_native_call(char *out,size_t cap,uint16_t T){
    if(T<0x400){
        const char *nm=name_for_target(T);
        if(nm){snprintf(out,cap,"%s();",nm);return;}
    }else if(G_BS && T<0xc000){
        int bank;
        if(bankstate_target(G_BS,T,&bank)){
            int idx=funclist_find(G_FL,T,bank);
            int physical=bank>=0?bank:T>>14;
            if(idx<0)idx=funclist_find(G_FL,T,physical);
            if(idx>=0 && G_FL->items[idx].is_entry){
                snprintf(out,cap,"SMS_CALL_BANKED(0x%04X,%d,%s);",T,physical,G_FL->items[idx].name);return;
            }
        }
    }
    snprintf(out,cap,"call_by_address(0x%04X);",T);
}

/* ---- jump target (goto for intra-function, tail call otherwise) ---- */
static void emit_jump_target(FILE *o, uint16_t T, const char *condexpr, int extra){
    if (tr_has(T)){
        if (condexpr){
            fprintf(o,"        if (%s) { ", condexpr);
            if (extra) fprintf(o,"s->cyc += %d; ", extra);
            fprintf(o,"goto L_%04X; }\n", T);
        } else {
            fprintf(o,"        goto L_%04X;\n", T);
        }
        return;
    }
    char call[512];format_native_call(call,sizeof call,T);
    if (condexpr){
        fprintf(o,"        if (%s) { ", condexpr);
        if (extra) fprintf(o,"s->cyc += %d; ", extra);
        fprintf(o,"%s return; }\n",call);
    } else {
        fprintf(o,"        %s return;\n",call);
    }
}

/* ---- call target (push return addr + C-call; pop happens at the RET) ---- */
static void emit_call_target(FILE *o, uint16_t T, uint16_t ret, const char *condexpr, int extra){
    char call[512];format_native_call(call,sizeof call,T);
    /* Propagation guard: a well-behaved callee returns with SP exactly back to
     * its pre-push value. If it returns HIGHER (it popped its own return and
     * RET'd to an ancestor — the "abort N frames" idiom), the Z80 control flow
     * went past us, so we must propagate the return up the C call stack rather
     * than resume after the call. Each ancestor repeats the check until SP
     * matches, unwinding exactly the right number of frames. */
    if (condexpr){
        fprintf(o,"        if (%s) { s->cyc += %d; uint16_t _csp = s->sp; s->sp = (uint16_t)(s->sp - 2); sms_write16(s->sp, 0x%04X); %s if ((int16_t)(s->sp - _csp) > 0) return; }\n",
                condexpr, extra, ret, call);
    } else {
        fprintf(o,"        { uint16_t _csp = s->sp; s->sp = (uint16_t)(s->sp - 2); sms_write16(s->sp, 0x%04X); %s if ((int16_t)(s->sp - _csp) > 0) return; }\n", ret, call);
    }
}

/* ---- control flow ---- */
static void emit_cf(FILE *o, const Z80Insn *in, uint16_t addr){
    uint16_t next = (uint16_t)(addr + in->length);
    switch (in->cf){
        case Z80_CF_RET:
            if (in->prefix==Z80_PFX_ED) fprintf(o,"        s->iff1 = s->iff2;\n");
            fprintf(o,"        s->sp = (uint16_t)(s->sp + 2); return;\n");
            break;
        case Z80_CF_RET_COND:{ int cc=(in->opcode>>3)&7;
            fprintf(o,"        if (%s) { s->cyc += 6; s->sp = (uint16_t)(s->sp + 2); return; }\n", cond(cc));
            break; }
        case Z80_CF_JUMP:
            if (!in->has_target){
                const char *idx = (in->prefix==Z80_PFX_DD) ? "s->ix"
                                : (in->prefix==Z80_PFX_FD) ? "s->iy" : "z80_hl(s)";
                uint16_t cont;
                if (in->opcode==0xE9 && G_TR && trace_computed_call(G_TR, addr, &cont) && tr_has(cont))
                    /* computed call: one return (cont) was pushed before jp(hl);
                     * expected post-return SP is the current SP + 2. If higher,
                     * the callee RET'd past us — propagate (see emit_call_target). */
                    fprintf(o,"        { uint16_t _csp = (uint16_t)(s->sp + 2); call_by_address(%s); if ((int16_t)(s->sp - _csp) > 0) return; goto L_%04X; }\n", idx, cont);
                else
                    fprintf(o,"        call_by_address(%s); return;\n", idx);            /* tail jump */
            } else emit_jump_target(o, in->target, NULL, 0);
            break;
        case Z80_CF_JUMP_COND:
            if (in->prefix==Z80_PFX_NONE && in->opcode==0x10){   /* DJNZ */
                fprintf(o,"        s->b = (uint8_t)(s->b - 1);\n");
                emit_jump_target(o, in->target, "s->b != 0", 5);
            } else if (in->prefix==Z80_PFX_NONE && (in->opcode&0xE7)==0x20){ /* JR cc */
                emit_jump_target(o, in->target, cond((in->opcode>>3)&3), 5);
            } else {                                             /* JP cc */
                emit_jump_target(o, in->target, cond((in->opcode>>3)&7), 0);
            }
            break;
        case Z80_CF_CALL:
            emit_call_target(o, in->target, next, NULL, 0);
            break;
        case Z80_CF_CALL_COND:{ int cc=(in->opcode>>3)&7;
            emit_call_target(o, in->target, next, cond(cc), 7);
            break; }
        default: break;
    }
}

/* Register-only instructions can run together when their entire cost fits
 * before the next host event. Exclude memory, I/O, prefixes (including R
 * reads), EI/DI, HALT and transfers. The original instruction path remains
 * authoritative at a deadline and after EI. */
static bool span_register_op(const Z80Insn *in){
    if(in->prefix!=Z80_PFX_NONE || in->cf!=Z80_CF_NONE || in->is_halt || in->illegal)return false;
    int op=in->opcode,x=op>>6,y=(op>>3)&7,z=op&7;
    if(x==0){
        if(z==0)return op==0x00 || op==0x08;
        if(z==1 || z==3 || z==7)return true;
        if(z==4 || z==5 || z==6)return y!=6;
        return false;
    }
    if(x==1)return y!=6 && z!=6;
    if(x==2)return z!=6;
    return z==6 || op==0xd9 || op==0xeb || op==0xf9;
}

static int register_span_end(const TraceResult *tr,int k,const uint16_t *lbl,int nlbl){
    int end=k;
    for(int j=k;j<tr->insn_count && j<k+8;j++){
        if(!span_register_op(&tr->insns[j].insn))break;
        if(j>k){
            if(tr->insns[j].addr!=tr->insns[j-1].addr+tr->insns[j-1].insn.length)break;
            bool entry=false;
            for(int l=0;l<nlbl;l++)if(lbl[l]==tr->insns[j].addr){entry=true;break;}
            if(entry)break;
        }
        end=j;
    }
    return end;
}

/* ---- one function ---- */
static void emit_function(FILE *o, FILE *scratch, const SmsRom *rom, const FuncEntry *fe,
                          uint16_t *ents, int nent){
    TraceResult tr;
    int visible=ff_visible_entries(G_FL,fe->addr,fe->bank,ents,nent);
    trace_function(rom, fe->addr, fe->bank, ents, visible, &tr);
    G_TR = &tr;

    /* labels: any jump target that lands inside this function body */
    uint16_t *lbl = (uint16_t*)malloc((size_t)(tr.insn_count>0?tr.insn_count:1)*sizeof(uint16_t)+sizeof(uint16_t));
    int nlbl=0;
    for (int k=0;k<tr.insn_count;k++){
        const Z80Insn *in=&tr.insns[k].insn;
        uint16_t lt; bool have=false;
        if ((in->cf==Z80_CF_JUMP || in->cf==Z80_CF_JUMP_COND) && in->has_target){ lt=in->target; have=true; }
        else if (in->cf==Z80_CF_JUMP && !in->has_target && in->opcode==0xE9 &&
                 trace_computed_call(&tr, tr.insns[k].addr, &lt)) have=true;   /* computed-call continuation */
        if (have && tr_has(lt)){
            bool seen=false; for (int j=0;j<nlbl;j++) if (lbl[j]==lt){ seen=true; break; }
            if (!seen) lbl[nlbl++]=lt;
        }
    }

    /* Execution must begin at the ENTRY address. Instructions are emitted in
     * address order, so when the trace contains code below the entry (backward
     * jumps, or the tracer following a jump into lower memory), the first
     * emitted statement is NOT the entry. Jump to the entry label in that case;
     * also force the entry label so the goto resolves. */
    bool need_entry_jump = (tr.insn_count > 0 && tr.insns[0].addr != fe->addr);
    if (need_entry_jump){
        bool seen=false; for (int j=0;j<nlbl;j++) if (lbl[j]==fe->addr){ seen=true; break; }
        if (!seen) lbl[nlbl++]=fe->addr;
    }

    fprintf(o,"void %s(void){\n    Z80State *s = &g_z80; (void)s;\n", fe->name);
    fprintf(o,"    sms_enter(0x%04X);\n", fe->addr);
    if (tr.truncated)
        fprintf(o,"    /* NOTE: trace truncated (hit unmapped/illegal); body covers the\n"
                  "       statically decodable portion. Runtime resolves the rest. */\n");
    if (need_entry_jump)
        fprintf(o,"    goto L_%04X;   /* enter at the function's entry address */\n", fe->addr);

    /* Track slot banks in address order so direct CALL/JP targets resolve under
     * the right bank (mirrors the finder's discovery). */
    BankState bs; bankstate_init(&bs, fe->addr, fe->bank); G_BS = &bs;

    int span_end=-1;
    for (int k=0;k<tr.insn_count;k++){
        bankstate_step(&bs, &tr.insns[k], k>0 ? &tr.insns[k-1] : NULL);
        uint16_t a=tr.insns[k].addr; const Z80Insn *in=&tr.insns[k].insn;
        bool islbl=false; for (int j=0;j<nlbl;j++) if (lbl[j]==a){ islbl=true; break; }
        if (islbl) fprintf(o,"L_%04X:\n", a);
        if(k>span_end){
            int end=register_span_end(&tr,k,lbl,nlbl);
            if(end>k){
                int cost=0;
                for(int j=k;j<=end;j++)cost+=z80_instruction_base_cycles(&tr.insns[j].insn);
                fprintf(o,"#ifdef SMS_NATIVE_SPANS\n"
                          "    if (!s->ei_block && s->cyc < g_sync_deadline && %d <= g_sync_deadline - s->cyc) {\n",cost);
                for(int j=k;j<=end;j++){
                    fprintf(o,"        { /* %04X %s */\n",tr.insns[j].addr,tr.insns[j].insn.text);
                    emit_op(o,scratch,&tr.insns[j].insn);
                    fprintf(o,"        }\n");
                }
                fprintf(o,"        s->r = (uint8_t)((s->r & 0x80) | ((s->r + %d) & 0x7f));\n"
                          "        s->cyc += %d;\n    } else\n#endif\n    {\n",end-k+1,cost);
                span_end=end;
            }
        }
        fprintf(o,"    /* %04X  %s */\n", a, in->text);
        bool observed=in->cf!=Z80_CF_NONE || in->is_halt || in->opcode==0xdb || in->opcode==0xd3 ||
            (in->prefix==Z80_PFX_ED && (((in->opcode&0xc7)==0x40) || ((in->opcode&0xc7)==0x41) || ((in->opcode&0xe4)==0xa0)));
        fprintf(o,"    %s(0x%04X);\n",observed?"SMS_PC_CONTROL":"SMS_PC", a);
        /* R (memory-refresh) auto-increment: +1 per M1 opcode fetch; a prefix
         * byte (CB/ED/DD/FD, incl. DDCB/FDCB) adds one more M1. Matches superzazu
         * inc_r. R only affects LD A,R reads (used by some games for PRNG). */
        fprintf(o,"    s->r = (uint8_t)((s->r & 0x80) | ((s->r + %d) & 0x7f));\n",
                in->prefix == Z80_PFX_NONE ? 1 : 2);
        int base=z80_instruction_base_cycles(in);
        if (base>0) fprintf(o,"    sms_tick(%d);\n", base);
        if (in->cf==Z80_CF_NONE){
            fprintf(o,"    {\n");
            emit_op(o,scratch,in);
            fprintf(o,"    }\n");
        } else {
            emit_cf(o,in,a);
        }
        if(k==span_end)fprintf(o,"    } /* instruction-boundary fallback */\n");
    }
    fprintf(o,"}\n\n");
    G_BS = NULL;

    free(lbl);
    trace_free(&tr);
    G_TR = NULL;
}

/* ---- file writers ---- */
static void write_funcs_header(const char *path, const char *pfx, const FuncList *fl){
    FILE *h=fopen(path,"w");
    if (!h){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(h,
        "/* %s_funcs.h - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Prototypes for every recompiled function. call_by_address() (defined\n"
        " * in %s_dispatch.c) and the timing/IO surface come from sms_runtime.h. */\n"
        "#pragma once\n"
        "#define SMS_GENERATED_NATIVE_PC 1\n"
        "#include \"sms_runtime.h\"\n"
        "#include \"z80_ops.h\"\n"
        "#ifndef SMS_PC_CONTROL\n#define SMS_PC_CONTROL(a) SMS_PC(a)\n#endif\n"
        "#ifdef SMS_NATIVE_DISPATCH\n"
        "#define SMS_CALL_BANKED(a,b,f) do { if(sms_slot_bank(a)==(b)) f(); else call_by_address(a); } while(0)\n"
        "#else\n#define SMS_CALL_BANKED(a,b,f) call_by_address(a)\n#endif\n\n", pfx, pfx);
    for (int i=0;i<fl->count;i++)
        if (fl->items[i].is_entry)
            fprintf(h,"void %s(void);\n", fl->items[i].name);
    fclose(h);
}

static void write_dispatch(const char *path, const char *pfx, const FuncList *fl){
    FILE *d=fopen(path,"w");
    if (!d){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(d,
        "/* %s_dispatch.c - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Resolves a Z80 address to its recompiled function, bank-aware: an\n"
        " * address with multiple bank variants switches on the runtime bank in\n"
        " * its slot (sms_slot_bank). On an unresolved target the runner's\n"
        " * sms_dispatch_miss() logs it (PRINCIPLES #13a). */\n"
        "#include \"%s_funcs.h\"\n\n"
        "void call_by_address(uint16_t addr){\n    switch (addr){\n", pfx, pfx);
    uint8_t *seen=(uint8_t*)calloc(0x10000,1);
    for (int i=0;i<fl->count;i++){
        if (!fl->items[i].is_entry) continue;
        uint16_t a=fl->items[i].addr;
        if (seen[a]) continue;
        seen[a]=1;
        /* gather all bank variants at this address */
        int vidx[64]; int nv=0;
        for (int j=0;j<fl->count && nv<64;j++)
            if (fl->items[j].is_entry && fl->items[j].addr==a) vidx[nv++]=j;
        if (nv == 1 && a < 0x0400){
            fprintf(d,"    case 0x%04X: %s(); return;\n", a, fl->items[vidx[0]].name);
        } else if(nv==1){
            int b=fl->items[vidx[0]].bank;
            fprintf(d,"    case 0x%04X: if(sms_slot_bank(0x%04X)==%d){ %s(); return; } break;\n",
                a,a,b>=0?b:(a>>14),fl->items[vidx[0]].name);
        } else {
            /* runtime bank for a variant: explicit bank, or the slot's default
             * (slot index) for a fixed (-1) entry. */
            fprintf(d,"    case 0x%04X: switch (sms_slot_bank(0x%04X)){\n", a, a);
            int emitted[64]; int ne=0;
            for (int v=0; v<nv; v++){
                int b = fl->items[vidx[v]].bank;
                int rb = (b >= 0) ? b : (a >> 14);
                bool dup=false; for (int e=0;e<ne;e++) if (emitted[e]==rb){ dup=true; break; }
                if (dup) continue;
                emitted[ne++]=rb;
                fprintf(d,"        case %d: %s(); return;\n", rb, fl->items[vidx[v]].name);
            }
            fprintf(d,"        default: break;\n    } break;\n");
        }
    }
    free(seen);
    fprintf(d,"    default: break;\n    }\n    sms_dispatch_miss(addr);\n}\n");
    fclose(d);
}

static void write_layout(const char *path, const char *pfx, const GameConfig *cfg, const SmsRom *rom){
    FILE *l=fopen(path,"w");
    if (!l){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    fprintf(l,
        "/* %s_layout.c - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Per-game RAM layout (g_game_layout) from [ram_layout] in game.toml.\n"
        " * Compiled into the runner once it defines the GameLayout struct\n"
        " * (runner/game_layout.h) and SMSGG_HAVE_GAME_LAYOUT. */\n"
        "#if defined(SMSGG_HAVE_GAME_LAYOUT)\n"
        "#include \"game_layout.h\"\n"
        "const GameLayout g_game_layout = {\n", pfx);
    if (cfg->game_mode_addr     != GC_ADDR_UNSET) fprintf(l,"    .game_mode     = 0x%04X,\n", cfg->game_mode_addr);
    if (cfg->vblank_count_addr  != GC_ADDR_UNSET) fprintf(l,"    .vblank_count  = 0x%04X,\n", cfg->vblank_count_addr);
    if (cfg->player_object_addr != GC_ADDR_UNSET) fprintf(l,"    .player_object = 0x%04X,\n", cfg->player_object_addr);
    fprintf(l,"};\n#include \"game_spec.h\"\n"
              "const GameSpec g_game_spec = {\n"
              " .display_name=\"%s\", .short_name=\"%s\",\n"
              " .expected_rom_crc32=0x%08Xu, .expected_rom_size=%uu,\n"
              " .platform=%s\n};\n#endif\n",cfg->display_name[0]?cfg->display_name:pfx,pfx,rom->crc32,(unsigned)rom->size,
              cfg->platform==SMS_PLATFORM_GG?"GAME_PLATFORM_GG":"GAME_PLATFORM_SMS");
    fclose(l);
}

void cg_emit(const SmsRom *rom, const FuncList *fl, const GameConfig *cfg, const char *out_dir){
    G_FL = fl;
    CG_MKDIR(out_dir);   /* best-effort; generated/ usually already exists */

    const char *pfx = cfg->output_prefix;
    char path[600];

    int n = fl->count;
    uint16_t *ents=(uint16_t*)malloc((size_t)(n>0?n:1)*sizeof(uint16_t));

    snprintf(path,sizeof path,"%s/%s_funcs.h", out_dir, pfx);
    write_funcs_header(path, pfx, fl);

    snprintf(path,sizeof path,"%s/%s_full.c", out_dir, pfx);
    FILE *o=fopen(path,"w");
    if (!o){ fprintf(stderr,"[cg_emit] cannot write %s\n", path); exit(1); }
    FILE *scratch=tmpfile();
    if(!scratch){fclose(o);fprintf(stderr,"[cg_emit] cannot open emission scratch\n");exit(1);}
    fprintf(o,
        "/* %s_full.c - GENERATED by SmsRecomp. DO NOT EDIT (PRINCIPLES #19).\n"
        " * Recompiled Z80 function bodies. */\n"
        "#include \"%s_funcs.h\"\n\n", pfx, pfx);
    int emitted=0;
    for (int i=0;i<n;i++){
        if (!fl->items[i].is_entry) continue;
        emit_function(o, scratch, rom, &fl->items[i], ents, n);
        emitted++;
    }
    fclose(scratch);
    fclose(o);

    snprintf(path,sizeof path,"%s/%s_dispatch.c", out_dir, pfx);
    write_dispatch(path, pfx, fl);

    snprintf(path,sizeof path,"%s/%s_layout.c", out_dir, pfx);
    write_layout(path, pfx, cfg, rom);

    free(ents);
    printf("[cg_emit] wrote %d functions to %s/%s_{full,dispatch,layout}.c (+%s_funcs.h)\n",
           emitted, out_dir, pfx, pfx);
}

/* ======================================================================== *
 *  Flat single-step AOT backend                                            *
 * ======================================================================== *
 *
 * Function-form recompilation is ideal when the Z80 owns the whole machine:
 * generated calls/gotos can run until sms_tick reaches a video event. A
 * coprocessor host instead needs instruction-boundary control to interleave
 * another CPU and shared devices. The shared emitter uses the same decoder,
 * timing model and semantic helpers, but makes PC and the guest stack
 * explicit and returns after exactly one instruction.
 *
 * Every byte in the flat image is a legal dispatch entry. That deliberately
 * trades generated-code size for correctness with computed jumps: no profile
 * manifest or guessed function boundary is needed, and runtime performs no
 * opcode fetch/decode. The host should validate the uploaded image CRC before
 * enabling this backend and retain an interpreter fallback for a different or
 * self-modified driver image. */

/* The provider owns explicit-PC instruction lowering; this consumer owns
 * image alternatives, guards and whole-image dispatch assembly. */
static void emit_flat_body(FILE *o, FILE *scratch, const Z80Insn *in, uint16_t addr)
{
    if (z80_emit_step_body_with_scratch(o, scratch, in, addr) != Z80_EMIT_OK)
        cg_fail(in);
}

static bool flat_same_bytes(const Z80Insn *a, const Z80Insn *b)
{
    return a->length == b->length && memcmp(a->raw, b->raw, a->length) == 0;
}

static void emit_flat_match(FILE *o, FILE *scratch, const Z80Insn *in, uint16_t addr)
{
    fprintf(o, "        if (");
    for (unsigned bi = 0; bi < in->length; ++bi) {
        if (bi) fprintf(o, " && ");
        fprintf(o, "sms_read8(0x%04X) == 0x%02X", (uint16_t)(addr + bi),
                in->raw[bi]);
    }
    fprintf(o, ") {\n");
    emit_flat_body(o, scratch, in, addr);
    fprintf(o, "            return;\n        }\n");
}

static bool emit_flat_live_operands(FILE *o, FILE *scratch, const Z80Insn *in,
                                    uint16_t addr)
{
    const Z80EmitResult result = z80_emit_live_operand_case_with_scratch(o, scratch, in, addr);
    if (result == Z80_EMIT_UNSUPPORTED) return false;
    if (result != Z80_EMIT_OK) cg_fail(in);
    return true;
}

void cg_emit_flat_step(const SmsRom *rom, const GameConfig *cfg,
                       const char *out_dir, const SmsRom *variants,
                       int variant_count)
{
    CG_MKDIR(out_dir);
    const char *pfx = cfg->output_prefix;
    size_t image_size = rom->size > 0x10000u ? 0x10000u : rom->size;
    for (int v = 0; v < variant_count; ++v) {
        size_t n = variants[v].size > 0x10000u ? 0x10000u : variants[v].size;
        if (n > image_size) image_size = n;
    }
    char path[600];

    snprintf(path, sizeof path, "%s/%s_step.h", out_dir, pfx);
    FILE *h = fopen(path, "w");
    if (!h) { fprintf(stderr, "[cg_flat] cannot write %s\n", path); exit(1); }
    fprintf(h,
        "/* %s_step.h - GENERATED by SmsRecomp --flat-step. DO NOT EDIT. */\n"
        "#pragma once\n"
        "#include <stdint.h>\n"
        "#include \"sms_runtime.h\"\n"
        "#include \"z80_ops.h\"\n\n"
        "extern const uint32_t %s_code_crc32;\n"
        "extern const uint32_t %s_code_size;\n"
        "void %s_step(void);\n",
        pfx, pfx, pfx, pfx);
    fclose(h);

    snprintf(path, sizeof path, "%s/%s_step.c", out_dir, pfx);
    FILE *o = fopen(path, "w");
    if (!o) { fprintf(stderr, "[cg_flat] cannot write %s\n", path); exit(1); }
    FILE *scratch = tmpfile();
    if (!scratch) { fclose(o); fprintf(stderr, "[cg_flat] cannot open emission scratch\n"); exit(1); }
    fprintf(o,
        "/* %s_step.c - GENERATED by SmsRecomp --flat-step. DO NOT EDIT.\n"
        " * Flat Z80 AOT dispatch: one decoded instruction per call. */\n"
        "#include \"%s_step.h\"\n\n"
        "const uint32_t %s_code_crc32 = 0x%08Xu;\n"
        "const uint32_t %s_code_size = 0x%X;\n\n"
        "void %s_step(void)\n"
        "{\n"
        "    Z80State *s = &g_z80;\n"
        "    switch (s->pc) {\n",
        pfx, pfx, pfx, rom->crc32, pfx, (unsigned)image_size, pfx);

    const SmsRom *images[9];
    images[0] = rom;
    for (int v = 0; v < variant_count; ++v) images[v + 1] = &variants[v];

    unsigned emitted = 0;
    unsigned alternatives = 0;
    for (size_t a = 0; a < image_size; ++a) {
        Z80Insn decoded[9];
        int unique_count = 0;
        for (int v = 0; v <= variant_count; ++v) {
            const SmsRom *img = images[v];
            size_t nbytes = img->size > 0x10000u ? 0x10000u : img->size;
            if (a >= nbytes) continue;
            Z80Insn in;
            int n = z80_decode(img->data + a, nbytes - a, (uint16_t)a, &in);
            if (n <= 0 || in.illegal) continue;
            bool duplicate = false;
            for (int u = 0; u < unique_count; ++u) {
                if (flat_same_bytes(&decoded[u], &in)) { duplicate = true; break; }
            }
            if (!duplicate) decoded[unique_count++] = in;
        }
        if (unique_count == 0) continue;
        uint16_t addr = (uint16_t)a;
        fprintf(o, "    case 0x%04X: { /* %s", addr, decoded[0].text);
        if (unique_count > 1) fprintf(o, " (+%d variant%s)", unique_count - 1,
                                      unique_count == 2 ? "" : "s");
        fprintf(o, " */\n");
        /* Genesis uploads its sound program into RAM while the Z80 is reset,
         * and some carts briefly run bootstrap/partial images before the final
         * driver is present. Guard every compiled alternative against the live
         * bytes. A different revision, incomplete upload, or uncaptured
         * self-modified instruction takes the host's explicit interpreter
         * fallback. Matching paths perform no runtime opcode decode. */
        bool live_shape[9] = {false};
        for (int u = 0; u < unique_count; ++u) {
            bool covered = false;
            for (int v = 0; v < u; ++v) {
                if (live_shape[v] && decoded[v].prefix == decoded[u].prefix &&
                    decoded[v].opcode == decoded[u].opcode &&
                    decoded[v].length == decoded[u].length) {
                    covered = true;
                    break;
                }
            }
            if (covered) continue;
            /* Uploaded RAM programs can share a PC with a different bootstrap
             * instruction. Preserve live operands for each native opcode shape,
             * rather than requiring all image alternatives to have one shape. */
            live_shape[u] = emit_flat_live_operands(o, scratch, &decoded[u], addr);
            if (!live_shape[u]) emit_flat_match(o, scratch, &decoded[u], addr);
        }
        fprintf(o, "        sms_dispatch_miss(0x%04X);\n        return;\n    }\n", addr);
        emitted++;
        alternatives += (unsigned)(unique_count - 1);
    }

    fprintf(o,
        "    default:\n"
        "        sms_dispatch_miss(s->pc);\n"
        "        return;\n"
        "    }\n"
        "}\n");
    fclose(scratch);
    fclose(o);

    printf("[cg_flat] wrote %u instruction entries with %u byte-sequence variants (%zu-byte image) to %s/%s_step.{c,h}\n",
           emitted, alternatives, image_size, out_dir, pfx);
}
