// R34 native runtime boundary (R33 ownership + true-native scheduler cutover).
//
// The Xbox frontend talks only to this API.  LakeSnes/cosim live behind it as
// a temporary compatibility backend for code that has not been translated yet.
// That inversion is intentional: future batches can replace fallback pieces
// without changing xbox_main.cpp again, and the final backend can drop the
// reference core completely.

#ifndef ZAMN_NATIVE_RUNTIME_H
#define ZAMN_NATIVE_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include "cosim/guard_profile.h"
#include "cosim/r69_profile.h"
#include "cosim/r74_pair_profile.h"
#if defined(ZAMN_R69_PORTING_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
#include "apu.h"
#endif

#include "native/audio.h"
#include "native/video.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZamnNativeRuntime ZamnNativeRuntime;

typedef enum {
  // R33 shipping path: native C substitutions own the port, with the 65816/PPU
  // core retained only as a compatibility fallback for untranslated regions.
  ZAMN_RUNTIME_NATIVE_CUTOVER = 0,
  // Development/reference path: no substitutions; run the original ROM/core.
  ZAMN_RUNTIME_REFERENCE = 1,
} ZamnRuntimeMode;

typedef struct {
  bool native_video;
  int width;
  int height;
  uint64_t machine_cycles;
  uint64_t native_cycles;
  uint64_t native_calls;
} ZamnFrameResult;

typedef struct {
  uint64_t mode1_fast_lines;
  uint64_t submath_lines;
  uint64_t sprite_cache_builds;
  uint64_t sprite_cached_lines;
  uint64_t sprite_mmx_slivers;
  uint64_t sprite_scalar_slivers;

  uint64_t zombie_calls;
  uint64_t zombie_fused;
  uint64_t zombie_yield;
  uint64_t zombie_unknown;
  uint64_t zombie_h8600;
  uint64_t zombie_h8656;
  uint64_t zombie_h86b3;
  uint64_t zombie_random;

  uint32_t actor_counts[6];

  uint64_t burn_calls;
  uint64_t burn_cycles;
  uint64_t burn_legacy12;

  uint64_t dispatch_lookups;
  uint64_t dispatch_hits;
  uint64_t dispatch_probes;
  uint64_t r65_hot_hits;
  uint64_t r65_hot_misses;
  // R62: diagnostic-only native-substitution activations, per 30-frame window.
  uint64_t r62_family_hits[6];  // R59, R60, R61, R62, R63, R64
  struct { const char* name; uint32_t pc; uint64_t hits; } r62_top[5];
  int r62_top_count;


  uint64_t immediate_calls;
  uint64_t chain_hops;
  uint64_t cycles_elided;
  uint64_t page_rejects;
  uint64_t idle_slices;
  uint64_t idle_cycles;

  uint64_t guard_dry_runs;
  uint64_t guard_bytes;
  uint64_t guard_declines;
  uint64_t r52_fast_approved;
  uint64_t r52_fast_rejected;
  uint64_t r52_fast_bytes_avoided;
  uint64_t r56_oam_approved;
  uint64_t r57_oam_rejected;
  uint64_t r57_oam_deferred;
  uint64_t r58_oam_new_approved;
  uint64_t r58_notify_new_approved;
  uint64_t r66_total_approved;
#if defined(ZAMN_R68_INPUT_READONLY_GUARDS)
  uint64_t r68_oam_approvals;
  uint64_t r68_thread_approvals;
  uint64_t r70_thread_9a6d_approved;
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF)
  uint64_t r74_notify_approvals;
  uint64_t r74_oam_approvals;
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R70_PAUSE_TRACE) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
  uint64_t r70_pause_polls[3];
  uint64_t r70_pause_start_down[3];
#endif
#endif
#if defined(ZAMN_R67_GUARD_REASON_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
  uint64_t r67_fast_reasons[3][6];
  uint64_t r67_sandbox_results[3][2];
  uint64_t r67_defer_bad_db[3];
#endif
  ZamnGuardProfileHot guard_hot[ZAMN_GUARD_PROFILE_TOP];
#if defined(ZAMN_R69_PORTING_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
  ZamnR69GuardHot r69_guard_hot[ZAMN_R69_GUARD_TOP];
  uint64_t r69_guard_untracked;
  ZamnR69ApuWindow r69_apu;
#if defined(XBOX_PORT) && defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF)
  ZamnR74PairStat r74_pair_hot[ZAMN_R74_PAIR_TOP];
  uint64_t r74_pair_untracked;
#endif
#endif
  uint64_t guard_readonly_checks;
  uint64_t guard_bytes_avoided;
  uint64_t guard_readonly_declines;

  uint64_t transition_wait_slices;
  uint64_t transition_wait_cycles;
  uint64_t transition_wait_sites[4];

  uint64_t intro_wait_slices;
  uint64_t intro_wait_cycles;
  uint64_t intro_wait_sites[2];
  uint64_t intro_wait_pattern_rejects;

  uint64_t r43_wait_slices;
  uint64_t r43_wait_cycles;
  uint64_t r43_wait_sites[2];
  uint64_t r43_wait_rejects[2];

  uint64_t r45_apu_set_calls;
  uint64_t r45_apu_set_run_events;
  uint64_t r45_apu_set_cycles_elided;

  // R46: APU set traces are compact at the source. These prove how much trace
  // construction was eliminated before cosim_hw and preserve the exact native
  // equivalent cycle value for strict native-share accounting.
  uint64_t r46_apu_trace_calls;
  uint64_t r46_apu_run_steps_elided;
  uint64_t r46_apu_run_reps_elided;
  uint64_t r46_apu_cycles_elided;
  uint64_t r46_apu_stack_coalesced;
  uint64_t r46_apu_trace_steps_emitted;

  // Honest per-window native-equivalent work. `native_cpu_equiv` excludes DMA
  // begun by native vblank jobs, matching the strict project progress metric.
  uint64_t r46_work_equiv;
  uint64_t r46_native_equiv;
  uint64_t r46_native_cpu_equiv;

  // R44: top compatibility-timing owners among native substitutions.
  struct {
    const char* name;
    const char* symbol;
    uint32_t entry;
    uint64_t cycles;
    uint64_t calls;
  } r44_burn_top[3];
  int r44_burn_top_count;

  uint64_t native_renderer_lines;
  uint64_t native_renderer_fallbacks;

  uint32_t residual_samples;
  uint32_t residual_dropped;
  uint32_t residual_pc[8];
  uint32_t residual_hits[8];
  int residual_count;
} ZamnRuntimeWindowStats;

// The ROM bytes are needed only during this call; the compatibility backend
// takes ownership of its own cartridge copy just as it did before R33.
ZamnNativeRuntime* zamn_runtime_create(const uint8_t* rom, int rom_size,
                                       ZamnRuntimeMode mode);
void zamn_runtime_destroy(ZamnNativeRuntime* rt);

const char* zamn_runtime_backend_name(const ZamnNativeRuntime* rt);
int zamn_runtime_native_substitutions(const ZamnNativeRuntime* rt);
uint32_t zamn_runtime_r76_take_reverse_actions(void);

bool zamn_runtime_title_ready(ZamnNativeRuntime* rt);
void zamn_runtime_title_control(ZamnNativeRuntime* rt, bool hold, int selection);
void zamn_runtime_title_reset_begin(ZamnNativeRuntime* rt);
// One headless neutral-boot frame: -1 failure, 0 pending, 1 canonical title ready.
int zamn_runtime_title_reset_step(ZamnNativeRuntime* rt, bool start);
// Enter START from an already-canonical title snapshot with exactly one
// deterministic input frame. Used by R49.5 cached online starts.
bool zamn_runtime_title_start(ZamnNativeRuntime* rt);
bool zamn_runtime_reset_to_title(ZamnNativeRuntime* rt, bool start);
void zamn_runtime_set_pad(ZamnNativeRuntime* rt, int player, uint16_t bits);

// R47 netplay: canonical simulation hash. This deliberately hashes explicit
// deterministic fields rather than native structs/pointers.
uint64_t zamn_runtime_state_hash(const ZamnNativeRuntime* rt);
// Only for validation/loading of saves created before R71.
uint64_t zamn_runtime_legacy_state_hash(const ZamnNativeRuntime* rt);

// R48 rollback: complete deterministic machine snapshots at frame boundaries.
// The underlying LakeSnes save-state already covers CPU/PPU/APU/DMA/input/cart
// state; R48.1+ also serializes any native continuation parked across it.
int zamn_runtime_snapshot_size(ZamnNativeRuntime* rt);
int zamn_runtime_snapshot_needed_size(ZamnNativeRuntime* rt);
bool zamn_runtime_save_snapshot(ZamnNativeRuntime* rt, uint8_t* dst, int capacity, int* out_size);
bool zamn_runtime_load_snapshot(ZamnNativeRuntime* rt, const uint8_t* src, int size);
// Suppress pixel composition during rollback catch-up. SNES timing/OAM still run.
void zamn_runtime_set_replay_suppressed(ZamnNativeRuntime* rt, bool suppressed);

// R49.8 gameplay metadata/rule hooks. These are deterministic WRAM-backed
// values exposed through the runtime boundary so the Xbox frontend never
// reaches into LakeSnes directly.
uint16_t zamn_runtime_current_level(const ZamnNativeRuntime* rt);
// True while the original ROM has an active in-stage player HUD ($1E88/$1E8A).
bool zamn_runtime_player_stage_hud(const ZamnNativeRuntime* rt);
uint16_t zamn_runtime_exit_door_last(const ZamnNativeRuntime* rt);
uint16_t zamn_runtime_player_lives(const ZamnNativeRuntime* rt, int player);
uint16_t zamn_runtime_player_health(const ZamnNativeRuntime* rt, int player);
bool zamn_runtime_normal_checkpoint_reward(ZamnNativeRuntime* rt, int players);

// Run one game frame.  `native_pixels` is the R21/R32 fast 256-wide staging
// target.  If the compatibility PPU cannot render this frame into it,
// native_video is false and the caller should request the legacy picture below.
bool zamn_runtime_frame(ZamnNativeRuntime* rt, uint8_t* native_pixels,
                        int native_pitch, ZamnFrameResult* out);

// Copy the compatibility picture for a frame that could not use the native
// staging target. `scratch` must hold scratch_pitch * rows bytes.
void zamn_runtime_copy_legacy_frame(ZamnNativeRuntime* rt, uint8_t* dst,
                                    int dst_pitch, uint8_t* scratch,
                                    int scratch_pitch, int rows);

void zamn_runtime_audio(ZamnNativeRuntime* rt, int16_t* stereo, int frames);
void zamn_runtime_set_audio_mode(ZamnNativeRuntime* rt, ZamnNativeAudioMode mode);
const char* zamn_runtime_audio_mode_name(const ZamnNativeRuntime* rt);
void zamn_runtime_set_audio_profile(ZamnNativeRuntime* rt,
                                    ZamnNativeAudioProfile profile);
const char* zamn_runtime_audio_profile_name(const ZamnNativeRuntime* rt);
bool zamn_runtime_set_remastered_bank(ZamnNativeRuntime* rt, const uint8_t* data,
                                       uint32_t size);
bool zamn_runtime_has_remastered_bank(const ZamnNativeRuntime* rt);

void zamn_runtime_perf_reset(ZamnNativeRuntime* rt);
void zamn_runtime_take_window_stats(ZamnNativeRuntime* rt,
                                    ZamnRuntimeWindowStats* out);

#ifdef __cplusplus
}
#endif

#endif
