#include "netplay/netplay_core.h"

#include <string.h>

static void check_hash(ZnpCore* c, uint32_t frame) {
  ZnpHashSlot* a = &c->local_hash[frame & (ZNP_HISTORY - 1)];
  ZnpHashSlot* b = &c->peer_hash[frame & (ZNP_HISTORY - 1)];
  if (!a->valid || !b->valid || a->frame != frame || b->frame != frame) return;
  if (a->hash != b->hash && !c->fault) {
    c->fault = 1;
    c->fault_frame = frame;
    c->fault_local_hash = a->hash;
    c->fault_peer_hash = b->hash;
  }
}

void znp_core_init(ZnpCore* c, uint8_t local_slot, uint8_t delay) {
  if (!c) return;
  memset(c, 0, sizeof(*c));
  c->local_slot = local_slot < ZNP_MAX_PLAYERS ? local_slot : 0;
  c->delay = delay;
  for (uint32_t f = 0; f < delay; ++f) {
    for (uint8_t p = 0; p < ZNP_MAX_PLAYERS; ++p)
      znp_core_put_input(c, p, f, 0);
  }
}

bool znp_core_put_input(ZnpCore* c, uint8_t player, uint32_t frame,
                        uint16_t pad) {
  if (!c || player >= ZNP_MAX_PLAYERS || c->fault) return false;
  ZnpInputSlot* s = &c->inputs[player][frame & (ZNP_HISTORY - 1)];
  if (s->valid && s->frame == frame) {
    if (s->pad != pad) {
      c->fault = 1;
      c->fault_frame = frame;
      return false;
    }
    return true;
  }
  s->frame = frame;
  s->pad = pad;
  s->valid = 1;
  if (player == c->local_slot) c->latest_local = frame;
  else c->latest_peer = frame;
  return true;
}

bool znp_core_correct_input(ZnpCore* c, uint8_t player, uint32_t frame,
                            uint16_t pad, bool* changed) {
  if (changed) *changed = false;
  if (!c || player >= ZNP_MAX_PLAYERS || c->fault) return false;
  ZnpInputSlot* s = &c->inputs[player][frame & (ZNP_HISTORY - 1)];
  if (!s->valid || s->frame != frame) {
    s->frame = frame;
    s->pad = pad;
    s->valid = 1;
    if (player == c->local_slot) c->latest_local = frame;
    else if (frame > c->latest_peer) c->latest_peer = frame;
    if (changed) *changed = true;
    return true;
  }
  if (s->pad != pad) {
    s->pad = pad;
    if (changed) *changed = true;
  }
  if (player != c->local_slot && frame > c->latest_peer) c->latest_peer = frame;
  return true;
}

bool znp_core_get_input(const ZnpCore* c, uint8_t player, uint32_t frame,
                        uint16_t* pad) {
  if (!c || player >= ZNP_MAX_PLAYERS) return false;
  const ZnpInputSlot* s = &c->inputs[player][frame & (ZNP_HISTORY - 1)];
  if (!s->valid || s->frame != frame) return false;
  if (pad) *pad = s->pad;
  return true;
}

bool znp_core_frame_ready(const ZnpCore* c, uint32_t frame,
                          uint16_t pads[ZNP_MAX_PLAYERS]) {
  if (!c || c->fault) return false;
  for (uint8_t p = 0; p < ZNP_MAX_PLAYERS; ++p)
    if (!znp_core_get_input(c, p, frame, pads ? &pads[p] : 0)) return false;
  return true;
}

void znp_core_commit_frame(ZnpCore* c, uint32_t frame) {
  if (!c || c->fault) return;
  if (frame == c->frame) c->frame = frame + 1;
}

void znp_core_put_local_hash(ZnpCore* c, uint32_t frame, uint64_t hash) {
  if (!c || c->fault) return;
  ZnpHashSlot* s = &c->local_hash[frame & (ZNP_HISTORY - 1)];
  s->frame = frame;
  s->hash = hash;
  s->valid = 1;
  check_hash(c, frame);
}

void znp_core_put_peer_hash(ZnpCore* c, uint32_t frame, uint64_t hash) {
  if (!c || c->fault) return;
  ZnpHashSlot* s = &c->peer_hash[frame & (ZNP_HISTORY - 1)];
  if (s->valid && s->frame == frame && s->hash != hash) {
    c->fault = 1;
    c->fault_frame = frame;
    c->fault_peer_hash = hash;
    return;
  }
  s->frame = frame;
  s->hash = hash;
  s->valid = 1;
  check_hash(c, frame);
}

bool znp_core_latest_local_hash(const ZnpCore* c, uint32_t* frame,
                                uint64_t* hash) {
  if (!c || c->frame == 0) return false;
  uint32_t f = c->frame - 1;
  for (uint32_t n = 0; n < ZNP_HISTORY && n <= f; ++n) {
    uint32_t q = f - n;
    const ZnpHashSlot* s = &c->local_hash[q & (ZNP_HISTORY - 1)];
    if (s->valid && s->frame == q) {
      if (frame) *frame = q;
      if (hash) *hash = s->hash;
      return true;
    }
  }
  return false;
}
