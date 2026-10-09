#include "native/runtime.h"

#include <stdlib.h>
#include <string.h>

#include "cosim/cosim.h"
#include "native/audio.h"
#include "native/video.h"
#include "port/player.h"
#include "apu.h"
#include "dsp.h"
#include "ppu.h"
#include "snes.h"

// These are release diagnostics owned by the existing native gameplay batches.
// They intentionally stay behind the R33 runtime boundary instead of leaking
// cosim internals back into the Xbox frontend.
void cosim_r27ZombieFrameStats(uint64_t* calls, uint64_t* fused,
                               uint64_t* to_yield, uint64_t* unknown,
                               uint64_t* h8600, uint64_t* h8656,
                               uint64_t* h86b3, uint64_t* random);
void cosim_r27ZombieFrameStatsReset(void);
void cosim_r29ActorStats(uint32_t* counts);
void cosim_r28BulkBurnReset(void);
void cosim_r28BulkBurnStats(uint64_t* calls, uint64_t* cycles,
                            uint64_t* legacy12);

struct ZamnNativeRuntime {
  Snes* snes;
  Cosim cosim;
  bool cosim_live;
  ZamnRuntimeMode mode;
  ZamnNativeAudio audio;
  ZamnNativeVideo video;
#if defined(XBOX_PORT) && defined(ZAMN_R39_BUFFERED_LOG) && defined(ZAMN_R62_ACTOR_CONTROL_NATIVE)
  // Unrelated to game state; these never enter rollback snapshots or hashes.
  uint32_t r62_prior_native_hits[COSIM_MAX_ROUTINES];
#endif
  int title_reset_frames; // frontend-only; never used by gameplay or replay
#if defined(XBOX_PORT) && defined(ZAMN_R71_CONTROL_MAPPING)
  uint8_t r71_lt_held[2]; // deterministic edges: stored in rollback snapshots
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R46_STRICT_NATIVE_SHARE)
  uint64_t r46_total_last;
  uint64_t r46_native_last;
  uint64_t r46_idle_last;
  uint64_t r46_wait_last;
  uint64_t r46_dma_last;
#endif
};

static void set_pad(Snes* snes, int player, uint16_t bits) {
  for (int b = 0; b < 12; ++b)
    snes_setButtonState(snes, player + 1, b, (bits & (1u << b)) != 0);
}

ZamnNativeRuntime* zamn_runtime_create(const uint8_t* rom, int rom_size,
                                       ZamnRuntimeMode mode) {
  if (!rom || rom_size <= 0) return NULL;

  ZamnNativeRuntime* rt = (ZamnNativeRuntime*)calloc(1, sizeof(*rt));
  if (!rt) return NULL;

  rt->mode = mode;
  zamn_native_audio_init(&rt->audio);
  zamn_native_video_init(&rt->video);
  rt->snes = snes_init();
  if (!rt->snes || !snes_loadRom(rt->snes, rom, rom_size)) {
    zamn_runtime_destroy(rt);
    return NULL;
  }

  if (mode == ZAMN_RUNTIME_NATIVE_CUTOVER) {
    cosim_init(&rt->cosim, rt->snes, COSIM_NATIVE);
    cosim_enable_all(&rt->cosim);
    rt->cosim_live = true;
  }

  snes_setPixelFormat(rt->snes, pixelFormatXRGB);
  snes_setWidescreen(rt->snes, 0, 0);
  snes_reset(rt->snes, true);
#if defined(XBOX_PORT) && defined(ZAMN_R41_NATIVE_AUDIO)
  dsp_setExternalSampleProvider(rt->snes->apu->dsp, zamn_native_audio_sample,
                                &rt->audio);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R41_NATIVE_RENDER_OWNER)
  zamn_native_video_install(&rt->video, rt->snes->ppu);
#endif
  zamn_runtime_perf_reset(rt);
  return rt;
}

void zamn_runtime_destroy(ZamnNativeRuntime* rt) {
  if (!rt) return;
  if (rt->cosim_live) cosim_free(&rt->cosim);
  if (rt->snes) snes_free(rt->snes);
  free(rt);
}

const char* zamn_runtime_backend_name(const ZamnNativeRuntime* rt) {
  if (!rt) return "none";
  if (rt->mode != ZAMN_RUNTIME_NATIVE_CUTOVER) return "reference LakeSnes";
#if defined(XBOX_PORT) && defined(ZAMN_R41_TRANSITION_WAIT_CUTOVER)
  return "R41 native transition waits + R40 gameplay cutover / hardware fallback";
#elif defined(XBOX_PORT) && defined(ZAMN_R36_NATIVE_LEAN)
  return "R36 native gameplay lean + residual cutover profiler / hardware fallback";
#elif defined(XBOX_PORT) && defined(ZAMN_R35_NATIVE_OBJECT_PIPELINE)
  return "R35 native scheduler + cached/MMX object pipeline / hardware fallback";
#elif defined(XBOX_PORT) && defined(ZAMN_R34_TRUE_NATIVE_CUTOVER)
  return "R34 native scheduler / immediate translated-C / hardware fallback";
#else
  return "R33-compatible native-owner / translated-C first / LakeSnes fallback";
#endif
}

int zamn_runtime_native_substitutions(const ZamnNativeRuntime* rt) {
  return rt && rt->cosim_live ? rt->cosim.stat_count : 0;
}

uint32_t zamn_runtime_r76_take_reverse_actions(void) {
#if defined(XBOX_PORT) && defined(ZAMN_R75_PAD_ROUTE_TRACE) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
  return player_cycle_r76_take_applied();
#else
  return 0;
#endif
}

void zamn_runtime_set_pad(ZamnNativeRuntime* rt, int player, uint16_t bits) {
  if (!rt || !rt->snes || player < 0 || player > 1) return;
#if defined(XBOX_PORT) && defined(ZAMN_R71_CONTROL_MAPPING)
  const uint8_t held=(uint8_t)((bits & (1u<<12)) != 0);
  if(held && !rt->r71_lt_held[player] && rt->mode==ZAMN_RUNTIME_NATIVE_CUTOVER)
    player_cycle_request((uint16_t)(player*2),PSN_CYCLE_WEAPON,-1);
  rt->r71_lt_held[player]=held;
  bits &= (uint16_t)~(1u<<12);
#endif
  set_pad(rt->snes, player, bits);
}

// USA FNV64 985A7978F6E47187: title menu yields at $80:9744.
// Saved stack: direct page (2), P (1), DB (1), JSL return $80:9747 (3).
static int title_page(ZamnNativeRuntime* rt) {
  if (!rt || !rt->snes) return -1;
  Wram* w=(Wram*)rt->snes->ram;
  for (unsigned i=0;i<48;i+=2) {
    if (!(wram_r16(w,0x1180+i)&0x8000)) continue;
    unsigned sp=wram_r16(w,0x11b0+i);
    if (sp>0x1ff8) continue;
    if (wram_r16(w,sp+5)==0x9747 && wram_r8(w,sp+7)==0x80)
      return wram_r16(w,sp+1);
  }
  return -1;
}
bool zamn_runtime_title_ready(ZamnNativeRuntime* rt) { return title_page(rt)>=0; }
void zamn_runtime_title_control(ZamnNativeRuntime* rt, bool hold, int selection) {
  int dp=title_page(rt); if(dp<0)return;
  Wram* w=(Wram*)rt->snes->ram;
  // Prevent the stock idle timeout while the native frontend owns input.
  if(hold) wram_w16(w,dp+0x5e,0);
  if(selection>=0) wram_w16(w,dp+0x6a,selection ? 4 : 0);
  // Move only the two menu actors offscreen. The logo and spiral are untouched.
  unsigned start=wram_r16(w,dp+0x50), password=wram_r16(w,dp+0x52);
  if(start<0x1ffe && password<0x1ffe) {
    wram_w16(w,start+2,hold?0xff80:0x00d0);
    wram_w16(w,password+2,hold?0xff80:0x00d5);
  }
}
void zamn_runtime_title_reset_begin(ZamnNativeRuntime* rt) {
  if(!rt)return;
  rt->title_reset_frames=0;
  if(rt->cosim_live)cosim_free(&rt->cosim);
  // Clear prior-session mastering/voice history, retaining the chosen preset/bank.
  zamn_native_audio_set_mode(&rt->audio,rt->audio.mode);
  snes_reset(rt->snes,true);
  if(rt->mode==ZAMN_RUNTIME_NATIVE_CUTOVER) {
    cosim_init(&rt->cosim,rt->snes,COSIM_NATIVE);
    cosim_enable_all(&rt->cosim);
  }
#if defined(XBOX_PORT) && defined(ZAMN_R71_CONTROL_MAPPING)
  rt->r71_lt_held[0]=rt->r71_lt_held[1]=0;
  player_cycle_clear();
#endif
  set_pad(rt->snes,0,0);set_pad(rt->snes,1,0);
}
bool zamn_runtime_title_start(ZamnNativeRuntime* rt) {
  if(!rt || !zamn_runtime_title_ready(rt))return false;
  ZamnFrameResult fr;
  // The cached state is the canonical neutral title yield. Reproduce the old
  // R49.4 final step exactly: expose START, pulse P1 START for one headless
  // frame, then release it before gameplay ownership begins.
  zamn_runtime_title_control(rt,false,0);
  zamn_runtime_set_replay_suppressed(rt,true);
  set_pad(rt->snes,0,1u<<3);
  bool ok=zamn_runtime_frame(rt,NULL,0,&fr);
  set_pad(rt->snes,0,0);
  zamn_runtime_set_replay_suppressed(rt,false);
  zamn_runtime_perf_reset(rt);
  return ok;
}
int zamn_runtime_title_reset_step(ZamnNativeRuntime* rt, bool start) {
  if(!rt || rt->title_reset_frames>=3600)return -1;
  ZamnFrameResult fr;
  // Headless work only; return to the lobby/network pump after every frame.
  zamn_runtime_set_replay_suppressed(rt,true);
  bool ok=zamn_runtime_frame(rt,NULL,0,&fr);
  ++rt->title_reset_frames;
  bool ready=ok && zamn_runtime_title_ready(rt);
  zamn_runtime_set_replay_suppressed(rt,false);
  if(ready) {
    zamn_runtime_title_control(rt,false,0);
    if(start) ok=zamn_runtime_title_start(rt);
    else zamn_runtime_perf_reset(rt);
  }
  return !ok ? -1 : ready ? 1 : 0;
}
bool zamn_runtime_reset_to_title(ZamnNativeRuntime* rt, bool start) {
  if(!rt)return false;
  zamn_runtime_title_reset_begin(rt);
  int result;
  do { result=zamn_runtime_title_reset_step(rt,start); } while(result==0);
  return result==1;
}

static uint64_t r47_hash_byte(uint64_t h, uint8_t v) {
  return (h ^ v) * UINT64_C(1099511628211);
}
static uint64_t r47_hash_u16(uint64_t h, uint16_t v) {
  h = r47_hash_byte(h, (uint8_t)v);
  return r47_hash_byte(h, (uint8_t)(v >> 8));
}
static uint64_t r47_hash_u32(uint64_t h, uint32_t v) {
  h = r47_hash_u16(h, (uint16_t)v);
  return r47_hash_u16(h, (uint16_t)(v >> 16));
}
static uint64_t r47_hash_u64(uint64_t h, uint64_t v) {
  h = r47_hash_u32(h, (uint32_t)v);
  return r47_hash_u32(h, (uint32_t)(v >> 32));
}

#define ZAMN_RB_SNAPSHOT_MAGIC 0x3253425au /* 'ZBS2' */
typedef struct {
  uint32_t magic;
  uint32_t core_size;
  uint32_t cosim_size;
} ZamnRollbackFooter;

/*
 * R49.1 rollback continuation state.
 *
 * LakeSnes' ordinary save-state loader deliberately clears the Xbox integer
 * APU phase accumulators because they are derived from the serialized machine
 * state.  That is safe for a user-initiated cold state load, but rollback is a
 * continuation: discarding the fractional SPC/master-clock phase creates a
 * state that never existed in the original timeline.  A later APU port/set
 * transfer can then run a different number of SPC cycles before an ACK and
 * eventually produce a deterministic netplay hash mismatch.
 *
 * Keep these two Xbox-only accumulators in the rollback wrapper rather than
 * changing the generic LakeSnes state-file format/version.
 */
#if defined(XBOX_PORT)
#if defined(ZAMN_R71_CONTROL_MAPPING)
#define ZAMN_R71_RB_BYTES ((int)sizeof(PlayerCycleState) + 2)
#else
#define ZAMN_R71_RB_BYTES 0
#endif
#define ZAMN_RB_XBOX_TIMING_BYTES ((int)sizeof(uint32_t) + (int)sizeof(int64_t) + ZAMN_R71_RB_BYTES)
#else
#define ZAMN_RB_XBOX_TIMING_BYTES 0
#endif

static int rollback_platform_state_size(void) {
  return ZAMN_RB_XBOX_TIMING_BYTES;
}

int zamn_runtime_snapshot_size(ZamnNativeRuntime* rt) {
  if (!rt || !rt->snes) return 0;
  const int core = snes_saveState(rt->snes, NULL);
  if (core <= 0) return 0;
  const int cosim = rt->cosim_live ? cosim_rollback_snapshot_max_size(&rt->cosim) : 0;
  if (rt->cosim_live && cosim <= 0) return 0;
  const uint64_t total = (uint64_t)core + (uint64_t)cosim +
                         (uint64_t)sizeof(rt->audio) +
                         (uint64_t)rollback_platform_state_size() +
                         (uint64_t)sizeof(ZamnRollbackFooter);
  return total <= 0x7fffffffu ? (int)total : 0;
}

int zamn_runtime_snapshot_needed_size(ZamnNativeRuntime* rt) {
  if (!rt || !rt->snes) return 0;
  const int core = snes_saveState(rt->snes, NULL);
  if (core <= 0) return 0;
  const int cosim = rt->cosim_live ?
      cosim_rollback_snapshot_needed_size(&rt->cosim) : 0;
  if (rt->cosim_live && cosim <= 0) return 0;
  const uint64_t total = (uint64_t)core + (uint64_t)cosim +
                         (uint64_t)sizeof(rt->audio) +
                         (uint64_t)rollback_platform_state_size() +
                         (uint64_t)sizeof(ZamnRollbackFooter);
  return total <= 0x7fffffffu ? (int)total : 0;
}

bool zamn_runtime_save_snapshot(ZamnNativeRuntime* rt, uint8_t* dst,
                                int capacity, int* out_size) {
  if (!rt || !rt->snes || !dst || capacity <= 0) return false;
  const int platform = rollback_platform_state_size();
  const int reserve = (int)sizeof(rt->audio) + platform +
                      (int)sizeof(ZamnRollbackFooter);
  if (capacity <= reserve) return false;

  // Give the fixed writer the whole caller-owned buffer. The ordinary core
  // state is ~270 KiB; the extra rollback capacity is deliberate headroom, so
  // StateHandler never reaches its realloc path here.
  const int core = snes_saveStateFast(rt->snes, dst, capacity - reserve);
  if (core <= 0 || core > capacity - reserve) return false;

  int cosim = 0;
  if (rt->cosim_live) {
    if (!cosim_rollback_save(&rt->cosim, dst + core,
                             capacity - core - reserve, &cosim))
      return false;
  }
  const int used = core + cosim + reserve;
  if (used > capacity) return false;
  uint8_t* tail = dst + core + cosim;
  memcpy(tail, &rt->audio, sizeof(rt->audio));
  tail += sizeof(rt->audio);
#if defined(XBOX_PORT)
  memcpy(tail, &rt->snes->apuMasterPending, sizeof(rt->snes->apuMasterPending));
  tail += sizeof(rt->snes->apuMasterPending);
  memcpy(tail, &rt->snes->apuCycleDebtNumerator,
         sizeof(rt->snes->apuCycleDebtNumerator));
  tail += sizeof(rt->snes->apuCycleDebtNumerator);
#if defined(ZAMN_R71_CONTROL_MAPPING)
  PlayerCycleState cycle;
  player_cycle_get_state(&cycle);
  memcpy(tail,&cycle,sizeof(cycle));tail+=sizeof(cycle);
  memcpy(tail,rt->r71_lt_held,2);tail+=2;
#endif
#endif
  ZamnRollbackFooter f = {ZAMN_RB_SNAPSHOT_MAGIC, (uint32_t)core,
                          (uint32_t)cosim};
  memcpy(dst + used - (int)sizeof(f), &f, sizeof(f));
  if (out_size) *out_size = used;
  return true;
}

bool zamn_runtime_load_snapshot(ZamnNativeRuntime* rt, const uint8_t* src,
                                int size) {
  if (!rt || !rt->snes || !src) return false;
  const int platform = rollback_platform_state_size();
  const int reserve = (int)sizeof(rt->audio) + platform +
                      (int)sizeof(ZamnRollbackFooter);
  // Pre-R71 named saves and title caches contain the same gameplay state,
  // with no LT edge/weapon queue fields. Read them by resetting the new fields.
#if defined(XBOX_PORT) && defined(ZAMN_R71_CONTROL_MAPPING)
  const int old_reserve = reserve - ZAMN_R71_RB_BYTES;
#else
  const int old_reserve = reserve;
#endif
  if (size <= old_reserve) return false;
#if defined(XBOX_PORT)
  ppu_xboxCancelNativeTarget();
#endif
  ZamnRollbackFooter f;
  memcpy(&f, src + size - (int)sizeof(f), sizeof(f));
  if (f.magic != ZAMN_RB_SNAPSHOT_MAGIC) return false;
  const uint64_t expected = (uint64_t)f.core_size + (uint64_t)f.cosim_size +
                            (uint64_t)reserve;
  const uint64_t legacy_expected = (uint64_t)f.core_size + (uint64_t)f.cosim_size +
                                   (uint64_t)old_reserve;
  const bool legacy = expected != (uint64_t)size && legacy_expected == (uint64_t)size;
  if ((expected != (uint64_t)size && !legacy) || f.core_size == 0) return false;
  if (!snes_loadStateFast(rt->snes, src, (int)f.core_size)) return false;
  if (rt->cosim_live) {
    if (f.cosim_size == 0 ||
        !cosim_rollback_load(&rt->cosim, src + f.core_size,
                             (int)f.cosim_size))
      return false;
  } else if (f.cosim_size != 0) {
    return false;
  }
  const uint8_t* tail = src + f.core_size + f.cosim_size;
  // The remastered bank allocation is process-local. Rollback snapshots are
  // normally restored in-process, but R49.5 also persists a canonical title
  // snapshot across launches. Never resurrect a pointer value from an older
  // process; keep the currently installed bank allocation while restoring the
  // deterministic mastering/voice continuation around it.
  const ZamnNativeAudioMode current_mode = rt->audio.mode;
  const ZamnNativeAudioProfile current_profile = rt->audio.profile;
  const uint8_t* current_bank = rt->audio.bank;
  const uint32_t current_bank_size = rt->audio.bank_size;
  ZamnRemasteredSample current_samples[256];
  memcpy(current_samples, rt->audio.sample, sizeof(current_samples));
  memcpy(&rt->audio, tail, sizeof(rt->audio));
  rt->audio.mode = current_mode;
  rt->audio.profile = current_profile;
  rt->audio.bank = current_bank;
  rt->audio.bank_size = current_bank_size;
  memcpy(rt->audio.sample, current_samples, sizeof(current_samples));
  tail += sizeof(rt->audio);
#if defined(XBOX_PORT)
  /* snes_loadStateFast() clears these; rollback must restore their exact phase. */
  memcpy(&rt->snes->apuMasterPending, tail, sizeof(rt->snes->apuMasterPending));
  tail += sizeof(rt->snes->apuMasterPending);
  memcpy(&rt->snes->apuCycleDebtNumerator, tail,
         sizeof(rt->snes->apuCycleDebtNumerator));
  tail += sizeof(rt->snes->apuCycleDebtNumerator);
#if defined(ZAMN_R71_CONTROL_MAPPING)
  PlayerCycleState cycle;
  if (!legacy) {
    memcpy(&cycle,tail,sizeof(cycle));tail+=sizeof(cycle);
    player_cycle_set_state(&cycle);
    memcpy(rt->r71_lt_held,tail,2);tail+=2;
  } else {
    player_cycle_clear();
    rt->r71_lt_held[0]=rt->r71_lt_held[1]=0;
  }
#endif
#endif
  return true;
}

void zamn_runtime_set_replay_suppressed(ZamnNativeRuntime* rt, bool suppressed) {
  if (!rt || !rt->snes || !rt->snes->ppu) return;
  rt->snes->ppu->noPixels = suppressed;
}

uint16_t zamn_runtime_current_level(const ZamnNativeRuntime* rt) {
  if (!rt || !rt->snes || !rt->snes->ram) return 0;
  return wram_r16((Wram*)rt->snes->ram, 0x1e7cu);
}
bool zamn_runtime_player_stage_hud(const ZamnNativeRuntime* rt) {
  if (!rt || !rt->snes || !rt->snes->ram) return false;
  const Wram* w=(const Wram*)rt->snes->ram;
  return wram_r16(w,0x1e88u)!=0 || wram_r16(w,0x1e8au)!=0;
}

uint16_t zamn_runtime_exit_door_last(const ZamnNativeRuntime* rt) {
  if (!rt || !rt->snes || !rt->snes->ram) return 0;
  return wram_r16((Wram*)rt->snes->ram, 0x1fbcu);
}

uint16_t zamn_runtime_player_lives(const ZamnNativeRuntime* rt, int player) {
  if (!rt || !rt->snes || !rt->snes->ram || player < 0 || player > 1) return 0xffffu;
  return wram_r16((Wram*)rt->snes->ram, 0x1d4cu + (uint32_t)(player * 2));
}

uint16_t zamn_runtime_player_health(const ZamnNativeRuntime* rt, int player) {
  if (!rt || !rt->snes || !rt->snes->ram || player < 0 || player > 1) return 0;
  return wram_r16((Wram*)rt->snes->ram, 0x1cb8u + (uint32_t)(player * 2));
}

bool zamn_runtime_normal_checkpoint_reward(ZamnNativeRuntime* rt, int players) {
  if (!rt || !rt->snes || !rt->snes->ram) return false;
  if (players < 1) players = 1;
  if (players > 2) players = 2;
  Wram* w = (Wram*)rt->snes->ram;
  for (int player = 0; player < players; ++player) {
    const uint32_t off = (uint32_t)(player * 2);
    uint16_t lives = wram_r16(w, 0x1d4cu + off);
    // A negative life count is the ROM's game-over state. Re-enter at zero,
    // then apply the checkpoint +1 so the character is eligible for the next
    // level spawn. Normal live counts preserve the original five-life cap.
    if (lives & 0x8000u) lives = 0;
    if (lives < 5u) ++lives;
    wram_w16(w, 0x1d4cu + off, lives);
    if (wram_r16(w, 0x1cb8u + off) == 0)
      wram_w16(w, 0x1cb8u + off, 10u);
  }
  return true;
}

static uint64_t r71_state_hash_ex(const ZamnNativeRuntime* rt, bool include_r71) {
  if (!rt || !rt->snes || !rt->snes->cpu) return 0;
  const Snes* s = rt->snes;
  const Cpu* c = s->cpu;
  uint64_t h = UINT64_C(1469598103934665603);
  for (uint32_t i = 0; i < 0x20000u; ++i) h = r47_hash_byte(h, s->ram[i]);

  // Explicit compatibility CPU state catches divergence before it necessarily
  // becomes visible in WRAM, but never hashes pointers or compiler padding.
  h = r47_hash_u16(h, c->a); h = r47_hash_u16(h, c->x);
  h = r47_hash_u16(h, c->y); h = r47_hash_u16(h, c->sp);
  h = r47_hash_u16(h, c->pc); h = r47_hash_u16(h, c->dp);
  h = r47_hash_byte(h, c->k); h = r47_hash_byte(h, c->db);
  uint16_t flags = (uint16_t)c->c | ((uint16_t)c->z << 1) |
                   ((uint16_t)c->v << 2) | ((uint16_t)c->n << 3) |
                   ((uint16_t)c->i << 4) | ((uint16_t)c->d << 5) |
                   ((uint16_t)c->xf << 6) | ((uint16_t)c->mf << 7) |
                   ((uint16_t)c->e << 8) | ((uint16_t)c->waiting << 9) |
                   ((uint16_t)c->stopped << 10) | ((uint16_t)c->irqWanted << 11) |
                   ((uint16_t)c->nmiWanted << 12) | ((uint16_t)c->intWanted << 13) |
                   ((uint16_t)c->resetWanted << 14);
  h = r47_hash_u16(h, flags);

  h = r47_hash_u32(h, s->frames);
  h = r47_hash_u64(h, s->cycles);
#if defined(XBOX_PORT)
  /* R49.1: these affect when the SPC700 reaches future port acknowledgements. */
  h = r47_hash_u32(h, s->apuMasterPending);
  h = r47_hash_u64(h, (uint64_t)s->apuCycleDebtNumerator);
#if defined(ZAMN_R71_CONTROL_MAPPING)
  if (include_r71) {
    PlayerCycleState cycle;
    player_cycle_get_state(&cycle);
    for(int i=0;i<2;i++) {
      h=r47_hash_byte(h,rt->r71_lt_held[i]);
      h=r47_hash_byte(h,cycle.ttl[i]);
      for(int k=0;k<2;k++)h=r47_hash_byte(h,(uint8_t)cycle.steps[i][k]);
    }
  }
#else
  (void)include_r71;
#endif
#endif
  h = r47_hash_u16(h, s->hPos); h = r47_hash_u16(h, s->vPos);
  h = r47_hash_u16(h, s->hTimer); h = r47_hash_u16(h, s->vTimer);
  h = r47_hash_u16(h, s->autoJoyTimer);
  for (int i = 0; i < 4; ++i) h = r47_hash_u16(h, s->portAutoRead[i]);
  uint16_t machine = (uint16_t)s->hIrqEnabled |
                     ((uint16_t)s->vIrqEnabled << 1) |
                     ((uint16_t)s->nmiEnabled << 2) |
                     ((uint16_t)s->inNmi << 3) |
                     ((uint16_t)s->irqCondition << 4) |
                     ((uint16_t)s->inIrq << 5) |
                     ((uint16_t)s->inVblank << 6) |
                     ((uint16_t)s->autoJoyRead << 7) |
                     ((uint16_t)s->fastMem << 8);
  return r47_hash_u16(h, machine);
}

uint64_t zamn_runtime_state_hash(const ZamnNativeRuntime* rt) {
  return r71_state_hash_ex(rt,true);
}
uint64_t zamn_runtime_legacy_state_hash(const ZamnNativeRuntime* rt) {
  return r71_state_hash_ex(rt,false);
}

bool zamn_runtime_frame(ZamnNativeRuntime* rt, uint8_t* native_pixels,
                        int native_pitch, ZamnFrameResult* out) {
  if (!rt || !rt->snes || !out) return false;
  memset(out, 0, sizeof(*out));
#if defined(XBOX_PORT) && defined(ZAMN_R71_CONTROL_MAPPING)
  player_cycle_age();
#endif

  const bool target =
      native_pixels && native_pitch > 0 &&
      ppu_xboxBeginNativeTarget(rt->snes->ppu, native_pixels, native_pitch);

  const uint64_t machine0 = rt->snes->cycles;
  const uint64_t native0 = rt->cosim_live ? rt->cosim.work.cycles_native : 0;
  const uint64_t calls0 = rt->cosim_live ? rt->cosim.work.calls_native : 0;

  if (rt->cosim_live)
    cosim_frame(&rt->cosim);
  else
    snes_runFrame(rt->snes);

  out->machine_cycles = rt->snes->cycles - machine0;
  if (rt->cosim_live) {
    out->native_cycles = rt->cosim.work.cycles_native - native0;
    out->native_calls = rt->cosim.work.calls_native - calls0;
  }

  out->width = 512;
  out->height = 448;
  if (target) {
    out->native_video =
        ppu_xboxFinishNativeTarget(rt->snes->ppu, &out->width, &out->height);
  } else {
    ppu_xboxCancelNativeTarget();
  }
  return true;
}

void zamn_runtime_copy_legacy_frame(ZamnNativeRuntime* rt, uint8_t* dst,
                                    int dst_pitch, uint8_t* scratch,
                                    int scratch_pitch, int rows) {
  if (!rt || !rt->snes || !dst || dst_pitch <= 0 || rows <= 0) return;

  const int packed_pitch = 512 * 4;
  if (dst_pitch == packed_pitch) {
    ppu_putPixels(rt->snes->ppu, dst);
    return;
  }

  if (!scratch || scratch_pitch < packed_pitch || dst_pitch < packed_pitch)
    return;

  ppu_putPixels(rt->snes->ppu, scratch);
  for (int y = 0; y < rows; ++y)
    memcpy(dst + y * dst_pitch, scratch + y * scratch_pitch,
           (size_t)packed_pitch);
}

void zamn_runtime_audio(ZamnNativeRuntime* rt, int16_t* stereo, int frames) {
  if (!rt || !rt->snes || !stereo || frames <= 0) return;
  snes_setSamples(rt->snes, stereo, frames);
#if defined(XBOX_PORT) && defined(ZAMN_R41_NATIVE_AUDIO)
  zamn_native_audio_process(&rt->audio, stereo, frames);
#endif
}

void zamn_runtime_set_audio_mode(ZamnNativeRuntime* rt, ZamnNativeAudioMode mode) {
  if (!rt) return;
  zamn_native_audio_set_mode(&rt->audio, mode);
}

const char* zamn_runtime_audio_mode_name(const ZamnNativeRuntime* rt) {
  return rt ? zamn_native_audio_mode_name(&rt->audio) : "Original";
}

void zamn_runtime_set_audio_profile(ZamnNativeRuntime* rt,
                                    ZamnNativeAudioProfile profile) {
  if (!rt) return;
  zamn_native_audio_set_profile(&rt->audio, profile);
}

const char* zamn_runtime_audio_profile_name(const ZamnNativeRuntime* rt) {
  return rt ? zamn_native_audio_profile_name(&rt->audio) : "Balanced";
}

bool zamn_runtime_set_remastered_bank(ZamnNativeRuntime* rt, const uint8_t* data,
                                       uint32_t size) {
  return rt && zamn_native_audio_set_bank(&rt->audio, data, size);
}

bool zamn_runtime_has_remastered_bank(const ZamnNativeRuntime* rt) {
  return rt && zamn_native_audio_has_bank(&rt->audio);
}

void zamn_runtime_perf_reset(ZamnNativeRuntime* rt) {
#if defined(XBOX_PORT) && defined(ZAMN_R46_STRICT_NATIVE_SHARE)
  if (rt && rt->cosim_live) {
    rt->r46_total_last = rt->cosim.work.cycles_total;
    rt->r46_native_last = rt->cosim.work.cycles_native;
    rt->r46_idle_last = rt->cosim.work.cycles_idle;
    rt->r46_wait_last = rt->cosim.work.cycles_wait;
    rt->r46_dma_last = rt->cosim.work.cycles_native_dma;
  }
#else
  (void)rt;
#endif
  snes_perfReset();
  ppu_xboxPerfReset();
  cosim_r27ZombieFrameStatsReset();
  cosim_r28BulkBurnReset();
#if defined(XBOX_PORT) && defined(ZAMN_R291_HOT_RESIDUE)
  cosim_r291HotReset();
#endif
}

void zamn_runtime_take_window_stats(ZamnNativeRuntime* rt,
                                    ZamnRuntimeWindowStats* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!rt) return;

  out->mode1_fast_lines = ppu_xboxPerfFastLines();
  out->submath_lines = ppu_xboxPerfSubMathLines();
  out->sprite_cache_builds = ppu_xboxPerfSpriteCacheBuilds();
  out->sprite_cached_lines = ppu_xboxPerfSpriteCachedLines();
  out->sprite_mmx_slivers = ppu_xboxPerfSpriteMmxSlivers();
  out->sprite_scalar_slivers = ppu_xboxPerfSpriteScalarSlivers();

  if (rt->cosim_live) {
    cosim_r27ZombieFrameStats(
        &out->zombie_calls, &out->zombie_fused, &out->zombie_yield,
        &out->zombie_unknown, &out->zombie_h8600, &out->zombie_h8656,
        &out->zombie_h86b3, &out->zombie_random);
    cosim_r27ZombieFrameStatsReset();

    cosim_r29ActorStats(out->actor_counts);

    cosim_r28BulkBurnStats(&out->burn_calls, &out->burn_cycles,
                           &out->burn_legacy12);
    cosim_r28BulkBurnReset();

    cosim_r33_dispatch_take(&rt->cosim, &out->dispatch_lookups,
                            &out->dispatch_hits, &out->dispatch_probes);
#if defined(XBOX_PORT) && defined(ZAMN_R65_HOT_NATIVE_DISPATCH) && defined(ZAMN_R39_BUFFERED_LOG)
    cosim_r65_hot_take(&rt->cosim, &out->r65_hot_hits, &out->r65_hot_misses);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R39_BUFFERED_LOG) && defined(ZAMN_R62_ACTOR_CONTROL_NATIVE)
    // Sample existing CosimStat counters only at the perf-window boundary.
    // No new per-translation increments, no game-state writes, and no cost
    // added to the logging-OFF Release executable.
    for (int i=0; i<rt->cosim.stat_count && i<COSIM_MAX_ROUTINES; ++i) {
      const CosimStat* st=&rt->cosim.stats[i];
      if(!st->routine || !st->routine->name)continue;
      const char* name=st->routine->name;
      if(name[0]!='r'||name[1]!='6'&&name[1]!='5'||name[2]<'0'||name[2]>'9'||name[3]!='_')continue;
      int family=-1;
      if(name[1]=='5'&&name[2]=='9')family=0;
      if(name[1]=='6'&&name[2]=='0')family=1;
      if(name[1]=='6'&&name[2]=='1')family=2;
      if(name[1]=='6'&&name[2]=='2')family=3;
      if(name[1]=='6'&&name[2]=='3')family=4;
      if(name[1]=='6'&&name[2]=='4')family=5;
      if(family<0)continue;
      // A guard refusal is a ROM fallback, not a native call.
      uint32_t now=(uint32_t)(st->calls-st->declined);
      uint32_t before=rt->r62_prior_native_hits[i];
      uint64_t delta=now>=before ? (uint64_t)(now-before) : (uint64_t)now;
      rt->r62_prior_native_hits[i]=now;
      out->r62_family_hits[family]+=delta;
      if(!delta)continue;
      for(int j=0;j<5;j++)if(delta>out->r62_top[j].hits){
         for(int k=4;k>j;k--)out->r62_top[k]=out->r62_top[k-1];
         out->r62_top[j].name=name;out->r62_top[j].pc=st->routine->entry;
         out->r62_top[j].hits=delta;break;
      }
    }
    for(int j=0;j<5;j++)if(out->r62_top[j].hits)out->r62_top_count++;
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R46_STRICT_NATIVE_SHARE)
    const uint64_t r46_total_now = rt->cosim.work.cycles_total;
    const uint64_t r46_native_now = rt->cosim.work.cycles_native;
    const uint64_t r46_idle_now = rt->cosim.work.cycles_idle;
    const uint64_t r46_wait_now = rt->cosim.work.cycles_wait;
    const uint64_t r46_dma_now = rt->cosim.work.cycles_native_dma;
    const uint64_t r46_total = r46_total_now - rt->r46_total_last;
    const uint64_t r46_native = r46_native_now - rt->r46_native_last;
    const uint64_t r46_idle = r46_idle_now - rt->r46_idle_last;
    const uint64_t r46_wait = r46_wait_now - rt->r46_wait_last;
    const uint64_t r46_dma = r46_dma_now - rt->r46_dma_last;
    rt->r46_total_last = r46_total_now;
    rt->r46_native_last = r46_native_now;
    rt->r46_idle_last = r46_idle_now;
    rt->r46_wait_last = r46_wait_now;
    rt->r46_dma_last = r46_dma_now;
#endif
    cosim_r34_cutover_take(&rt->cosim, &out->immediate_calls,
                           &out->chain_hops, &out->cycles_elided,
                           &out->page_rejects, &out->idle_slices,
                           &out->idle_cycles);
#if defined(XBOX_PORT) && defined(ZAMN_R36_NATIVE_LEAN)
    cosim_r36_guard_take(&rt->cosim, &out->guard_dry_runs,
                         &out->guard_bytes, &out->guard_declines);
#if defined(ZAMN_R52_FAST_COLLISION_GUARDS)
    cosim_r52_fast_take(&rt->cosim, &out->r52_fast_approved,
                        &out->r52_fast_rejected,
                        &out->r52_fast_bytes_avoided);
    cosim_r56_oam_take(&rt->cosim, &out->r56_oam_approved);
#if defined(ZAMN_R57_OAM_COLLISION_PROOF)
    cosim_r57_oam_take(&rt->cosim, &out->r57_oam_rejected,
                       &out->r57_oam_deferred);
#if defined(ZAMN_R58_COLLISION_THREAD_FASTPATH)
    cosim_r58_oam_take(&rt->cosim, &out->r58_oam_new_approved);
    cosim_r58_notify_take(&rt->cosim, &out->r58_notify_new_approved);
#if defined(ZAMN_R66_TOTAL_HANDLER_FAST_GUARDS)
    cosim_r66_total_take(&rt->cosim, &out->r66_total_approved);
#if defined(ZAMN_R68_INPUT_READONLY_GUARDS)
    cosim_r68_input_take(&rt->cosim, &out->r68_oam_approvals,
                         &out->r68_thread_approvals);
#if defined(ZAMN_R70_HOT_THREAD_9A6D)
    cosim_r70_thread_take(&rt->cosim, &out->r70_thread_9a6d_approved);
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF)
    cosim_r74_pair_take(&rt->cosim, &out->r74_notify_approvals,
                        &out->r74_oam_approvals);
#endif
#endif
#endif
#if defined(ZAMN_R67_GUARD_REASON_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
    cosim_r67_reasons_take(&rt->cosim, out->r67_fast_reasons,
                           out->r67_sandbox_results, out->r67_defer_bad_db);
#endif
#endif
#endif
#endif
#endif
#if defined(ZAMN_R39_BUFFERED_LOG)
    cosim_guard_profile_take(&rt->cosim, out->guard_hot);
#if defined(ZAMN_R69_PORTING_PROFILE) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
    cosim_r69_guard_take(out->r69_guard_hot, &out->r69_guard_untracked);
    apu_r69_profile_take(&out->r69_apu);
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF)
    cosim_r74_pair_hist_take(out->r74_pair_hot,&out->r74_pair_untracked);
#endif
#endif
#if defined(ZAMN_R70_PAUSE_TRACE) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
    cosim_r70_pause_take(&rt->cosim, out->r70_pause_polls,
                         out->r70_pause_start_down);
#endif
#endif
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R37_READONLY_GUARDS)
    cosim_r37_guard_take(&rt->cosim, &out->guard_readonly_checks,
                         &out->guard_bytes_avoided,
                         &out->guard_readonly_declines);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R41_TRANSITION_WAIT_CUTOVER)
    cosim_r41_wait_take(&rt->cosim, &out->transition_wait_slices,
                         &out->transition_wait_cycles,
                         out->transition_wait_sites);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER)
    cosim_r42_intro_wait_take(&rt->cosim, &out->intro_wait_slices,
                              &out->intro_wait_cycles,
                              out->intro_wait_sites,
                              &out->intro_wait_pattern_rejects);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R43_NATIVE_WAIT_CUTOVER)
    cosim_r43_wait_take(&rt->cosim, &out->r43_wait_slices,
                        &out->r43_wait_cycles, out->r43_wait_sites,
                        out->r43_wait_rejects);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R45_APU_SET_NATIVE_TRANSFER)
    cosim_r45_apu_set_take(&rt->cosim, &out->r45_apu_set_calls,
                            &out->r45_apu_set_run_events,
                            &out->r45_apu_set_cycles_elided);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R46_APU_COMPACT_TRACE)
    cosim_r46ApuTraceTake(&out->r46_apu_trace_calls,
                           &out->r46_apu_run_steps_elided,
                           &out->r46_apu_run_reps_elided,
                           &out->r46_apu_cycles_elided,
                           &out->r46_apu_stack_coalesced,
                           &out->r46_apu_trace_steps_emitted);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R46_STRICT_NATIVE_SHARE)
    {
      const uint64_t not_work = r46_idle + r46_wait;
      const uint64_t measured_work = r46_total > not_work ?
                                     r46_total - not_work : 0;
      // R34 immediate native C and R46 compact APU runs no longer consume
      // compatibility machine cycles, so restore their exact SNES-equivalent
      // cost to both sides of the share before comparing native vs residual.
      const uint64_t elided = out->cycles_elided + out->r46_apu_cycles_elided;
      const uint64_t native_cpu = r46_native > r46_dma ?
                                  r46_native - r46_dma : 0;
      out->r46_work_equiv = measured_work + elided;
      out->r46_native_equiv = r46_native + elided;
      out->r46_native_cpu_equiv = native_cpu + elided;
      if (out->r46_native_equiv > out->r46_work_equiv)
        out->r46_native_equiv = out->r46_work_equiv;
      if (out->r46_native_cpu_equiv > out->r46_work_equiv)
        out->r46_native_cpu_equiv = out->r46_work_equiv;
    }
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R44_NATIVE_BURN_ATTRIB)
    CosimR44BurnTop top[3];
    memset(top, 0, sizeof(top));
    out->r44_burn_top_count = cosim_r44BurnTake(&rt->cosim, top, 3);
    for (int i = 0; i < out->r44_burn_top_count; ++i) {
      out->r44_burn_top[i].name = top[i].name;
      out->r44_burn_top[i].symbol = top[i].symbol;
      out->r44_burn_top[i].entry = top[i].entry;
      out->r44_burn_top[i].cycles = top[i].cycles;
      out->r44_burn_top[i].calls = top[i].calls;
    }
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R41_NATIVE_RENDER_OWNER)
    zamn_native_video_take(&rt->video, &out->native_renderer_lines,
                           &out->native_renderer_fallbacks);
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R291_HOT_RESIDUE)
    out->residual_count = cosim_r291HotTake(
        out->residual_pc, out->residual_hits, 8,
        &out->residual_samples, &out->residual_dropped);
#endif
  }

  snes_perfReset();
  ppu_xboxPerfReset();
}
