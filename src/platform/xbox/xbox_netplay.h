#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum XboxNetplayExitReason {
  ZNP_EXIT_NORMAL = 0,
  ZNP_EXIT_PREPARE = 1,
  ZNP_EXIT_BEGIN = 2,
  ZNP_EXIT_SNAPSHOT_INIT = 3,
  ZNP_EXIT_SNAPSHOT_LOAD = 4,
  ZNP_EXIT_REPLAY_INPUT = 5,
  ZNP_EXIT_REPLAY_RUNTIME = 6,
  ZNP_EXIT_SNAPSHOT_SAVE = 7,
  ZNP_EXIT_AFTERFRAME = 8,
  ZNP_EXIT_RUNTIME = 9,
  ZNP_EXIT_CORE = 10,
};

typedef struct {
  uint32_t tx_packets;
  uint32_t rx_packets;
  uint32_t wait_loops;
  uint32_t wait_frames;
  uint32_t wait_ms;
  uint32_t max_wait_ms;
  uint32_t input_resends;
  uint32_t proactive_copies;
  uint32_t resend_suppressed;
  uint32_t pace_ms_total;
  uint32_t pace_budget_skips;
  uint32_t preinput_wait_frames;
  uint32_t preinput_wait_ms;
  uint32_t late_sample_frames;
  uint32_t rollback_predictions;
  uint32_t rollback_corrections;
  uint32_t rollback_events;
  uint32_t rollback_replayed_frames;
  uint32_t rollback_max_depth;
  uint32_t hash_matches;
  uint32_t current_frame;
  uint32_t latest_peer_frame;
  uint32_t peer_consumed_frame;
  uint32_t fault_frame;
  uint32_t rtt_ms;
  uint32_t jitter_ms;
  uint32_t calibration_samples;
  uint8_t delay;
  uint8_t auto_delay;
  uint8_t delay_auto;
  uint8_t local_slot;
  uint8_t active;
  uint8_t fault;
  uint8_t pace_ms;
} XboxNetplayStats;

typedef void (*XboxNetplayBackground)(uint8_t* pixels, int pitch);
// begin=true resets; polling returns 0..99 progress, 100 ready, -1 failure.
typedef int (*XboxNetplayPrepare)(bool begin);
void Xbox_Netplay_SetFrontend(XboxNetplayBackground background, XboxNetplayPrepare prepare);
void Xbox_Netplay_DrawText(uint8_t* pixels,int pitch,int x,int y,const char* text,int scale,uint32_t color);
// 1=Host Direct, 2=Join Direct, 3=Public Rooms. False means cancelled/failed.
bool Xbox_Netplay_Open(int choice);
// R49.9.4 compact Direct Connect submenu. Returns 1=host, 2=join, -1=back.
int Xbox_Netplay_DirectMenu(void);
bool Xbox_Netplay_Init(uint64_t rom_hash);
void Xbox_Netplay_Shutdown(void);
void Xbox_Netplay_ReportFatal(uint8_t reason, uint32_t frame);
bool Xbox_Netplay_Active(void);
const char* Xbox_Netplay_Role(void);

// R49.8 shared rules/profile/leaderboard metadata. Rules are part of the
// session contract even though they live outside the SNES snapshot.
enum XboxZamnRuleset {
  ZAMN_RULE_NORMAL_SAVE = 0,
  ZAMN_RULE_NORMAL_NOSAVE = 1,
  ZAMN_RULE_HARDCORE_SAVE = 2,
  ZAMN_RULE_HARDCORE_NOSAVE = 3,
};
void Xbox_Netplay_SetRuleset(uint8_t ruleset);
uint8_t Xbox_Netplay_Ruleset(void);
const char* Xbox_Netplay_RulesetName(uint8_t ruleset);
bool Xbox_Netplay_RulesetSavable(uint8_t ruleset);
bool Xbox_Netplay_RulesetNormal(uint8_t ruleset);
const char* Xbox_Netplay_ProfileName(void);
const char* Xbox_Netplay_PeerProfileName(void);
void Xbox_Netplay_ProfileMenu(void);
bool Xbox_Netplay_EnsureProfile(void);
// Returns one of XboxZamnRuleset, or -1 for back. Leaderboards are handled
// inside this menu and do not return to gameplay.
int Xbox_Netplay_SoloMenu(void);
void Xbox_Netplay_LeaderboardMenu(void);
// Record one completed level in the local best-only queue. Pending records are
// authenticated and uploaded when the leaderboard browser connects to the
// project-operated service.
void Xbox_Netplay_RecordLevelTime(uint8_t ruleset, uint16_t level,
                                  uint32_t frames, uint8_t players, bool solo);

// Sample one local physical pad for frame+delay and wait until both logical
// pads for `frame` are present. R48 supports Direct/Public sessions and negotiated 0..12 frame delay
// with AUTO (1-frame LAN floor). Returns false on timeout/protocol/desync fault.
bool Xbox_Netplay_BeginFrame(uint32_t frame, uint16_t local_pad,
                             uint16_t* p1, uint16_t* p2);

// R48 phase-aligns a slightly faster LAN peer before controller polling.
// Pacing is frame-budget aware: it may consume only genuine sub-16ms slack,
// so it cannot intentionally drag a frame that is already at the 60Hz budget.
void Xbox_Netplay_PaceBeforeInput(void);

// R48: for delay>=1, wait for the current remote input before polling the
// physical controller. This moves lockstep waiting out of local input age.
bool Xbox_Netplay_PrepareFrame(uint32_t frame);

// R48 experimental rollback. Delay mode remains the default/fallback.
bool Xbox_Netplay_RollbackEnabled(void);
uint8_t Xbox_Netplay_RollbackWindow(void);
bool Xbox_Netplay_TakeRollbackRequest(uint32_t current_frame, uint32_t* from_frame);
bool Xbox_Netplay_GetReplayInputs(uint32_t frame, uint16_t* p1, uint16_t* p2);
void Xbox_Netplay_NoteReplay(uint32_t depth);
// Exact input status for sparse pre-frame snapshots; no socket polling.
bool Xbox_Netplay_RemoteInputAuthoritative(uint32_t frame);
bool Xbox_Netplay_RollbackHealthy(void);
bool Xbox_Netplay_FrameAuthoritative(uint32_t frame);
void Xbox_Netplay_SubmitConfirmedHash(uint32_t frame, uint64_t hash);

// Send/drain the normal redundant input copy immediately after simulation,
// before video/audio/present, so a lost first packet gets its second chance sooner.
void Xbox_Netplay_MidFrame(uint32_t frame);

// Commit the frame after simulation. `state_hash` is sent every 30 frames;
// other calls pass zero and still advance the logical frame.
bool Xbox_Netplay_AfterFrame(uint32_t frame, uint64_t state_hash);
void Xbox_Netplay_GetStats(XboxNetplayStats* out);


// R49.7 shared online pause/save control channel.  These packets run only
// while gameplay simulation is frozen; they never alter deterministic input
// or state hashing.
uint8_t Xbox_Netplay_LocalSlot(void);
bool Xbox_Netplay_ControlSend(uint8_t kind, const void* data, int len);
// Returns payload bytes copied, 0 on timeout/no packet, -1 on transport error.
// `kind` receives the control subtype.
int Xbox_Netplay_ControlRecv(uint8_t* kind, void* data, int cap, uint32_t timeout_ms);
// Rebase the deterministic input timeline after both peers load the same save.
// `frame` is the next gameplay frame represented by the restored snapshot.
bool Xbox_Netplay_ResyncAfterLoad(uint32_t frame);

#ifdef __cplusplus
}
#endif
