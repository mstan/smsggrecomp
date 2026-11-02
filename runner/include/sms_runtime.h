/* Compatibility include for existing generated SMS/GG source. The canonical
 * packed Z80 state and host ABI live in the shared z80-recomp-core submodule. */
#ifndef SMSGG_SMS_RUNTIME_FORWARD_H
#define SMSGG_SMS_RUNTIME_FORWARD_H
#define SMSGG_NATIVE_EXECUTION 1

#if defined(SMS_NATIVE_SPANS) && !defined(SMS_NATIVE_HOOKS)
#error Native spans require the production instruction hooks
#endif

#ifdef SMS_NATIVE_HOOKS
#if defined(SMS_TRACE_PC) || defined(SMS_CYC_WATCH)
#error Native hooks require a production build; omit SMS_NATIVE_HOOKS for instruction diagnostics
#endif
#define sms_tick sms_tick_reference
#define sms_tick_post sms_tick_post_reference
#define sms_enter sms_enter_reference
#endif
#include "../../external/z80-recomp-core/include/sms_runtime.h"
#ifdef SMS_NATIVE_HOOKS
#undef sms_tick
#undef sms_tick_post
#undef sms_enter
/* Production instruction boundary: guest time, EI and IRQ sampling are exact;
 * timing-probe counters and the controlled differential harness are separate
 * build-time diagnostics. Keep the always-on function-entry ring. */
static inline void sms_tick(uint8_t n){
    if(g_z80.cyc>=g_sync_deadline)sms_sync();
    g_z80.cyc+=n;g_z80.ei_block=0;
}
static inline void sms_tick_post(uint8_t n){
    g_z80.cyc+=n;
    if(g_z80.cyc>=g_sync_deadline)sms_sync();
    g_z80.ei_block=0;
}
static inline void sms_enter(uint16_t addr){
    g_enter_ring[g_enter_pos & (SMS_ENTER_RING_SIZE-1)]=addr;g_enter_pos++;
}
/* Publish the real interrupted PC before every possible sync. Calls, I/O,
 * block operations and other externally observable instructions publish
 * unconditionally through SMS_PC_CONTROL. Pure arithmetic needs no store. */
#undef SMS_PC
#ifdef SMS_GENERATED_NATIVE_PC
#define SMS_PC(a) ((g_z80.cyc>=g_sync_deadline)?(void)(g_dbg_pc=(uint16_t)(a)):(void)0)
#else
/* Older generated block operations can sync after their cycle increment.
 * Keep their unconditional PC publication until regenerated with CONTROL. */
#define SMS_PC(a) (g_dbg_pc=(uint16_t)(a))
#endif
#define SMS_PC_CONTROL(a) (g_dbg_pc=(uint16_t)(a))
#else
#define SMS_PC_CONTROL(a) SMS_PC(a)
#endif

#endif
