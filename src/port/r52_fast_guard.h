// R52 — read-only, conservative, native guard fast decisions. No emulation
// and no game-state writes. A DEFER must execute the existing sandbox guard.
#ifndef PORT_R52_FAST_GUARD_H
#define PORT_R52_FAST_GUARD_H
#include "port/wram.h"
#include "port/oam.h"
#include "port/collide.h"

typedef enum {
  R52_DEFER_TO_SANDBOX = 0,
  R52_APPROVE_NO_HANDLER = 1,
  R52_REJECT_UNPORTED = 2,
  R52_APPROVE_TOTAL_HANDLER = 3,
  R58_APPROVE_READONLY_FIRST = 4,
  R66_APPROVE_NEW_TOTAL = 5, // Guard-free support; real handler still executes
  R68_APPROVE_READONLY_INPUT = 6, // Exact input guarantees no WRAM writes
  R70_APPROVE_9A6D_IGNORE = 7, // Proven $82:9A6D ignore branch, arg < $005C
  R74_APPROVE_PAIR_READONLY = 8 // First handler cannot alter second dispatch
} R52GuardDecision;

static inline bool r52_handler_recognized(uint32_t entry) {
  switch(entry) {
    case PLAYER_COLLIDE_ENTRY:
    case ENEMY_COLLIDE_ENTRY:
    case MONSTER_COLLIDE_ENTRY:
    case MONSTER_C440_COLLIDE_ENTRY:
    case ENEMY_B41C_COLLIDE_ENTRY:
    case ENEMY_CDDE_COLLIDE_ENTRY:
    case ENEMY_B592_COLLIDE_ENTRY:
    case ENEMY_D7F6_COLLIDE_ENTRY:
    case ENEMY_9A6D_COLLIDE_ENTRY:
    case ENEMY_9B6B_COLLIDE_ENTRY:
    case ENEMY_9063_COLLIDE_ENTRY:
    case ENEMY_D301_COLLIDE_ENTRY:
    case ENEMY_AC92_COLLIDE_ENTRY:
    case ENEMY_E6E4_COLLIDE_ENTRY:
    case ENEMY_990B_COLLIDE_ENTRY:
    case ENEMY_990B_SPIN_ENTRY:
    case ACTOR_845E_COLLIDE_ENTRY:
    case ACTOR_DEEB_COLLIDE_ENTRY:
    case ACTOR_F1C2_COLLIDE_ENTRY:
    case ACTOR_F534_COLLIDE_ENTRY:
    case VICTIM_A264_COLLIDE_ENTRY:
    case BOSS_9660_COLLIDE_ENTRY:
    case BOSS_AA2E_COLLIDE_ENTRY:
    case ACTOR_F330_COLLIDE_ENTRY:
    case ACTOR_A638_COLLIDE_ENTRY:
    case ACTOR_84AC_COLLIDE_ENTRY:
    case ENEMY_B95F_COLLIDE_ENTRY:
    case ENEMY_EFF0_COLLIDE_ENTRY:
    case ACTOR_C8C3_COLLIDE_ENTRY:
    case SHOT_COLLIDE_ENTRY:
    case SHOT_EDAA_COLLIDE_ENTRY:
    case SHOT_F6A3_COLLIDE_ENTRY:
    case ACTOR_F4EF_COLLIDE_ENTRY:
    case VICTIM_COLLIDE_ENTRY:
    case OBJECT_COLLIDE_ENTRY:
      return true;
    default: return false;
  }
}

// R68: exact inputs for six existing native handlers whose selected branch
// is total, returns carry CLEAR, and performs ZERO WRAM writes. In particular,
// none can alter another collision's actor record / thread registration.
// These are conservative sufficient conditions, not an assertion that other
// inputs are unsafe. The dispatcher's caller can still write its own scratch
// globals, just as in the existing R57/R58 OAM proof.
static inline bool r68_handler_readonly_for_arg(uint32_t entry, uint16_t arg) {
  switch (entry) {
    case ACTOR_DEEB_COLLIDE_ENTRY: return arg != DEEB_ID_STOP;
    case ACTOR_A638_COLLIDE_ENTRY: return arg != A638_ID_PARK;
    case ACTOR_F4EF_COLLIDE_ENTRY:
      return arg != F4EF_LATCH_P1 && arg != F4EF_LATCH_P2;
    case ACTOR_F534_COLLIDE_ENTRY:
      return arg != F534_LATCH_A && arg != F534_LATCH_B &&
             arg != F534_LATCH_C && arg != F534_LATCH_GUARDED_A &&
             arg != F534_LATCH_GUARDED_B;
    case ACTOR_F1C2_COLLIDE_ENTRY:
      return arg != F1C2_ID_A && arg != F1C2_ID_B &&
             arg != F1C2_ID_C && (arg & ENEMY_COLLIDE_ID_MASK) < COLLIDE_ID_PLAYER;
    case ACTOR_84AC_COLLIDE_ENTRY: return arg < COLLIDE_ID_PLAYER;
    default: return false;
  }
}

// Thread preflight uses Y as the handler argument; the handler pointer itself
// must still be loaded from live WRAM, and the existing bank-mirror test runs
// before this helper. Codes 0/2/1/3/4/5 retain their prior behavior.
static inline R52GuardDecision r68_thread_input_decision(const Wram* w,
                                                         uint16_t slot, uint16_t arg) {
  const uint16_t lo = wram_r16(w, W_THREAD_HANDLER + slot);
  const uint16_t bank = wram_r16(w, W_THREAD_HANDLER_BANK + slot);
  if (!(lo | bank)) return R52_APPROVE_NO_HANDLER;
  const uint32_t entry = ((uint32_t)(bank & 255u) << 16) | lo;
  return r68_handler_readonly_for_arg(entry, arg)
      ? R68_APPROVE_READONLY_INPUT : R52_DEFER_TO_SANDBOX;
}

// R70: the hot callback $82:9A6D is byte-for-byte the $81:D7F6 handler.
// For arg < $005C, it executes CMP #$005C : BCS(not taken) : CLC : RTL.
// The port's corresponding branch changes only ActorHandlerRegs, returns true,
// and touches zero bytes of WRAM. No nested calls, no other callbacks. The
// thread dispatcher still executes the real callback once after approval.
// Call this ONLY after validating live thread DB is a low-WRAM mirror and
// the prior handler lookup deferred (non-null known handler).
static inline R52GuardDecision r70_thread_hot_ignore_decision(
    const Wram* w, uint16_t slot, uint16_t arg) {
  const uint16_t lo = wram_r16(w, W_THREAD_HANDLER + slot);
  const uint16_t bank = wram_r16(w, W_THREAD_HANDLER_BANK + slot);
  const uint32_t entry = ((uint32_t)(bank & 0xffu) << 16) | lo;
  return entry == ENEMY_9A6D_COLLIDE_ENTRY && arg < COLLIDE_ID_PLAYER
         ? R70_APPROVE_9A6D_IGNORE : R52_DEFER_TO_SANDBOX;
}

// The exact ROM/port no-handler test is lo | bank == 0. Five proved-total
// handlers cannot decline; all other recognized handlers may decline in their
// bodies and still require the original 128 KiB guard.
static inline R52GuardDecision r52_handler_slot_decision(
    const Wram* w, uint16_t slot, uint32_t* missing_entry) {
  uint16_t lo = wram_r16(w, W_THREAD_HANDLER + slot);
  uint16_t bank = wram_r16(w, W_THREAD_HANDLER_BANK + slot);
  if ((lo | bank) == 0) return R52_APPROVE_NO_HANDLER;
  uint32_t entry = ((uint32_t)(bank & 0xff) << 16) | lo;
  // Proven total cases: every branch of these five existing native routines
  // returns true. The actual game-state writes still happen exactly once in
  // the native shim; only the scratch *preflight* is omitted.
  if (entry == SHOT_EDAA_COLLIDE_ENTRY || entry == SHOT_F6A3_COLLIDE_ENTRY ||
      entry == ENEMY_CDDE_COLLIDE_ENTRY || entry == ENEMY_B592_COLLIDE_ENTRY ||
      entry == ACTOR_845E_COLLIDE_ENTRY)
    return R52_APPROVE_TOTAL_HANDLER;
#if defined(ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS)
  // R66: Each listed port function returns true on every local control path.
  // No nested call can fail. The real handler STILL runs on live WRAM once.
  // A total first callback may mutate the second registration, so the notify
  // decision MUST defer when one of these is first (see both guards below).
  switch (entry) {
    case ACTOR_DEEB_COLLIDE_ENTRY:
    case ACTOR_F1C2_COLLIDE_ENTRY:
    case ACTOR_F534_COLLIDE_ENTRY:
    case VICTIM_A264_COLLIDE_ENTRY:
    case ACTOR_F330_COLLIDE_ENTRY:
    case ACTOR_A638_COLLIDE_ENTRY:
    case ACTOR_84AC_COLLIDE_ENTRY:
    case ACTOR_F4EF_COLLIDE_ENTRY:
    case OBJECT_COLLIDE_ENTRY:
      return R66_APPROVE_NEW_TOTAL;
    default: break;
  }
#endif
  if (r52_handler_recognized(entry)) return R52_DEFER_TO_SANDBOX;
  if (missing_entry) *missing_entry = entry;
  return R52_REJECT_UNPORTED;
}

// actor_collide_notify calls thread_b *first*. Only when it is empty can
// we inspect thread_a in advance: a real first handler may rewrite thread_a.
// The first MUST be empty; even a proved-total first handler may mutate the
// second handler's registration. The second can be empty or proved-total.
static inline R52GuardDecision r52_notify_decision(
    const Wram* w, uint16_t outer, uint16_t inner) {
  uint16_t thread_b = wram_r16(w, (uint32_t)inner + ACTOR_THREAD);
  R52GuardDecision b = r52_handler_slot_decision(w, thread_b, 0);
  if (b == R52_APPROVE_TOTAL_HANDLER
#if defined(ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS)
      || b == R66_APPROVE_NEW_TOTAL
#endif
      ) return R52_DEFER_TO_SANDBOX;
  if (b != R52_APPROVE_NO_HANDLER) return b;
  uint16_t thread_a = wram_r16(w, (uint32_t)outer + ACTOR_THREAD);
  return r52_handler_slot_decision(w, thread_a, 0);
}
// R58: Two total handlers leave actor records and every thread-registration
// word unchanged. $81:EDAA is RTL; $81:845E writes only CPU state and can
// park its own thread's WAIT flag through the caller. Therefore, for either of
// these as the first handler, the second handler registration still matches
// the live image. Unknown or other mutable first handlers still defer/reject
// exactly as R52. Code 4 denotes a NEW safe approval for logging.
static inline R52GuardDecision r58_notify_decision(
    const Wram* w, uint16_t outer, uint16_t inner) {
  const uint16_t thread_b = wram_r16(w, (uint32_t)inner + ACTOR_THREAD);
  R52GuardDecision b = r52_handler_slot_decision(w, thread_b, 0);
  if (b == R52_APPROVE_NO_HANDLER)
    return r52_handler_slot_decision(w,
         wram_r16(w, (uint32_t)outer + ACTOR_THREAD), 0);
#if defined(ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS)
  // Unlike the R58 whitelist, these total callbacks can write WRAM and even
  // change the second registration. Never approve the pair from stale reads.
  if (b == R66_APPROVE_NEW_TOTAL) return R52_DEFER_TO_SANDBOX;
#endif
  if (b != R52_APPROVE_TOTAL_HANDLER) return b;
  const uint16_t lo = wram_r16(w, W_THREAD_HANDLER + thread_b);
  const uint16_t bank = wram_r16(w, W_THREAD_HANDLER_BANK + thread_b);
  const uint32_t entry = ((uint32_t)(bank & 255u) << 16) | lo;
  if (entry != SHOT_EDAA_COLLIDE_ENTRY && entry != ACTOR_845E_COLLIDE_ENTRY)
    return R52_DEFER_TO_SANDBOX;
  const uint16_t thread_a = wram_r16(w, (uint32_t)outer + ACTOR_THREAD);
  R52GuardDecision a = r52_handler_slot_decision(w, thread_a, 0);
  if (a == R52_REJECT_UNPORTED) return R52_REJECT_UNPORTED;
  if (a == R52_DEFER_TO_SANDBOX) return R52_DEFER_TO_SANDBOX;
#if defined(ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS)
  // The R58 nonmutating FIRST handler also permits R66's total SECOND.
  // Classify this as an R66 save; prior R58 approvals remain unchanged.
  if (a == R66_APPROVE_NEW_TOTAL) return R66_APPROVE_NEW_TOTAL;
#endif
  return R58_APPROVE_READONLY_FIRST;
}

// R74: exact read-only first-callback proof, reusable by the native
// collision notifier. The first handler receives OUTER's collision ID.
// R70's $82:9A6D below $005C and R68's six input-guarded branches execute
// successfully without touching WRAM and return carry clear. In particular,
// they cannot rewrite the second callback registration. A second callback may
// mutate WRAM IF it is separately proven total (R52/R66). A second read-only
// callback may also be accepted. All other combinations defer to the existing
// copy-based sandbox verification. Never approve based on the second handler
// alone if the first handler can mutate the second registration.
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF)
static inline bool r74_callback_readonly_for_arg(const Wram* w,
                                                  uint16_t slot, uint16_t arg) {
  const uint16_t lo = wram_r16(w, W_THREAD_HANDLER + slot);
  const uint16_t bank = wram_r16(w, W_THREAD_HANDLER_BANK + slot);
  const uint32_t entry = ((uint32_t)(bank & 0xffu) << 16) | lo;
  return (entry == ENEMY_9A6D_COLLIDE_ENTRY && arg < COLLIDE_ID_PLAYER) ||
         r68_handler_readonly_for_arg(entry, arg);
}

static inline R52GuardDecision r74_notify_decision(
    const Wram* w, uint16_t outer, uint16_t inner) {
  // Preserve every earlier approval/rejection; R74 only handles deferrals.
  const R52GuardDecision old = r58_notify_decision(w, outer, inner);
  if (old != R52_DEFER_TO_SANDBOX) return old;
  const uint16_t bs = wram_r16(w, (uint32_t)inner + ACTOR_THREAD);
  const uint16_t first_arg = wram_r16(w, (uint32_t)outer + ACTOR_COLLIDE_ID);
  if (!r74_callback_readonly_for_arg(w, bs, first_arg)) return old;
  const uint16_t as = wram_r16(w, (uint32_t)outer + ACTOR_THREAD);
  const uint16_t second_arg = wram_r16(w, (uint32_t)inner + ACTOR_COLLIDE_ID);
  const R52GuardDecision second = r52_handler_slot_decision(w, as, NULL);
  if (second == R52_APPROVE_NO_HANDLER ||
      second == R52_APPROVE_TOTAL_HANDLER ||
      second == R66_APPROVE_NEW_TOTAL ||
      r74_callback_readonly_for_arg(w, as, second_arg))
    return R74_APPROVE_PAIR_READONLY;
  // The FIRST callback does not change registration, so an unsupported
  // second cannot become supported through that first call.
  if (second == R52_REJECT_UNPORTED) return R52_REJECT_UNPORTED;
  return R52_DEFER_TO_SANDBOX;
}
#endif



#endif // PORT_R52_FAST_GUARD_H
