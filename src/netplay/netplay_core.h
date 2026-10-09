#ifndef ZAMN_NETPLAY_CORE_H
#define ZAMN_NETPLAY_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZNP_MAX_PLAYERS 2
#define ZNP_HISTORY 256
#define ZNP_HASH_PERIOD 30

typedef struct {
  uint32_t frame;
  uint16_t pad;
  uint8_t valid;
} ZnpInputSlot;

typedef struct {
  uint32_t frame;
  uint64_t hash;
  uint8_t valid;
} ZnpHashSlot;

typedef struct {
  ZnpInputSlot inputs[ZNP_MAX_PLAYERS][ZNP_HISTORY];
  ZnpHashSlot local_hash[ZNP_HISTORY];
  ZnpHashSlot peer_hash[ZNP_HISTORY];
  uint32_t frame;
  uint32_t latest_local;
  uint32_t latest_peer;
  uint32_t fault_frame;
  uint64_t fault_local_hash;
  uint64_t fault_peer_hash;
  uint8_t local_slot;
  uint8_t delay;
  uint8_t fault;
} ZnpCore;

void znp_core_init(ZnpCore* c, uint8_t local_slot, uint8_t delay);
bool znp_core_put_input(ZnpCore* c, uint8_t player, uint32_t frame,
                        uint16_t pad);
// R48 rollback only: replace a locally predicted remote value with the later
// authoritative packet without treating the correction itself as a protocol fault.
bool znp_core_correct_input(ZnpCore* c, uint8_t player, uint32_t frame,
                            uint16_t pad, bool* changed);
bool znp_core_get_input(const ZnpCore* c, uint8_t player, uint32_t frame,
                        uint16_t* pad);
bool znp_core_frame_ready(const ZnpCore* c, uint32_t frame,
                          uint16_t pads[ZNP_MAX_PLAYERS]);
void znp_core_commit_frame(ZnpCore* c, uint32_t frame);
void znp_core_put_local_hash(ZnpCore* c, uint32_t frame, uint64_t hash);
void znp_core_put_peer_hash(ZnpCore* c, uint32_t frame, uint64_t hash);
bool znp_core_latest_local_hash(const ZnpCore* c, uint32_t* frame,
                                uint64_t* hash);

#ifdef __cplusplus
}
#endif

#endif
