// $80:E4BA  player_walk -- see port/walk.h.

#include <stddef.h>
#include "port/walk.h"

#include <stddef.h>
#include "port/coverage.h"
#include <stddef.h>
#include "port/step.h"
#include <stddef.h>
#include "port/terrain.h"

typedef struct {
  uint16_t x, y;
} Point;

// One player's walk this frame.
typedef struct {
  Wram* w;
  const Rom* rom;
  uint16_t page;       // the player's direct page
  bool met_reaction;   // stopped at a tile with a reaction of its own
  WalkLog* log;        // may be NULL
} Walk;

static uint16_t field(const Walk* k, uint16_t at) {
  return wram_r16(k->w, (uint16_t)(k->page + at));
}

static void set_field(Walk* k, uint16_t at, uint16_t v) {
  wram_w16(k->w, (uint16_t)(k->page + at), v);
}

static Point position(const Walk* k) {
  return (Point){field(k, WALK_DP_X), field(k, WALK_DP_Y)};
}

// ---------------------------------------------------------------------------
// The four questions
// ---------------------------------------------------------------------------

// The tests that add or subtract leave overflow behind, and the log keeps the
// last one's.
static void overflow_left(Walk* k, bool v) {
  if (k->log) k->log->overflow = v;
}

// Every answer goes through here, so the log sees each one.
static bool answer(Walk* k, WalkQuestion q, bool yes) {
  if (k->log) {
    k->log->asked[q][yes]++;
    k->log->last_yes = yes;
  }
  return yes;
}

static const uint16_t REACTION_VALUE[WALK_REACTIONS] = {
    0x0100, 0x0200, 0x0010, 0x0020, 0x0800, 0x8008};

int walk_tile_reaction(uint16_t attrs) {
  const uint16_t bits = (uint16_t)((attrs << 1) & WALK_REACTION_BITS);
  for (int i = 0; i < WALK_REACTIONS; i++)
    if (bits == REACTION_VALUE[i]) return i + 1;
  return 0;
}

static bool ground_is_solid(Walk* k, Point p) {
  TerrainRegs r;
  terrain_blocked(k->w, p.x, p.y, &r);
  overflow_left(k, r.v);
  if (!answer(k, WALK_ASK_GROUND, r.blocked)) return false;
  if (walk_tile_reaction(r.a) != 0) {
    // Not ours to handle. `walk_supported` turns the frame down, so a walk
    // that gets here is only finding that out.
    PORT_COVER(walk_reaction);
    k->met_reaction = true;
    return true;
  }
  return answer(k, WALK_ASK_REACTION, true);
}

static bool too_far_from_partner(Walk* k, Point p) {
  TetherRegs r;
  step_tether_blocked(k->w, p.x, p.y, &r);
  if (r.v_set) overflow_left(k, r.v);
  return answer(k, WALK_ASK_TETHER, r.blocked);
}

// Is anyone but the player standing at `p`?
static bool someone_at(Walk* k, Point p, WalkQuestion q) {
  ObstacleRegs r;
  ObstacleWork work = {0};
  actor_obstacle_at_point_counted(k->w, field(k, WALK_DP_RECORD), p.x, p.y, &r,
                                  &work);
  if (r.v_set) overflow_left(k, r.v);
  if (k->log)
    for (int i = 0; i < OBSTACLE_BLOCK_COUNT; i++)
      k->log->obstacle.blocks[i] += work.blocks[i];
  return answer(k, q, r.blocked);
}

static bool off_the_map(Walk* k, Point p) {
  BoundsRegs r;
  terrain_out_of_bounds(k->w, p.x, p.y, &r);
  return answer(k, WALK_ASK_MAP, r.c);
}

// ---------------------------------------------------------------------------
// The walk
// ---------------------------------------------------------------------------

// May the player step from `from` to `to`, which differ along one axis?
static bool can_step(Walk* k, Point from, Point to) {
  if (ground_is_solid(k, to)) {
    PORT_COVER(walk_solid);
    return false;
  }
  if (too_far_from_partner(k, to)) {
    PORT_COVER(walk_tethered);
    return false;
  }
  if (someone_at(k, to, WALK_ASK_THERE)) {
    // Standing clear, the player cannot walk into someone. Already standing
    // in someone, the player can walk out.
    if (!someone_at(k, from, WALK_ASK_HERE)) {
      PORT_COVER(walk_obstructed);
      return false;
    }
    PORT_COVER(walk_overlapping);
  }
  if (off_the_map(k, to)) {
    PORT_COVER(walk_off_map);
    return false;
  }
  return true;
}

static void walk(Walk* k) {
  StepProposeRegs proposed;
  step_propose(k->w, k->rom, k->page, &proposed);
  overflow_left(k, proposed.v);

  // Across first, at the height the player stands at now.
  Point here = position(k);
  Point across = {field(k, WALK_DP_WANT_X), here.y};
  if (can_step(k, here, across)) {
    set_field(k, WALK_DP_X, across.x);
    if (k->log) k->log->taken++;
  }

  // Then up or down, from wherever that left the player.
  here = position(k);
  Point up_down = {here.x, field(k, WALK_DP_WANT_Y)};
  if (can_step(k, here, up_down)) {
    set_field(k, WALK_DP_Y, up_down.y);
    if (k->log) k->log->taken++;
  }
}

void player_walk(Wram* w, const Rom* rom, uint16_t page, WalkLog* log) {
  Walk k = {w, rom, page, false, log};
  walk(&k);
}



// ---------------------------------------------------------------------------
// R38 read-only support probe
// ---------------------------------------------------------------------------
//
// `walk_supported()` predates the native Release cutover.  It answers the
// support question by *running the whole walk* on a 128 KB scratch clone and
// seeing whether the walk encountered one of the two ROM-owned cases.  That
// was ideal for verification, but after R37 it is one of the two remaining
// once-per-frame sandbox passes.
//
// This is the same decision written as a pure predicate.  It mirrors only the
// four yes/no tests that decide whether an axis advances; it does not reproduce
// register outputs, scratch writes, coverage, or cycle pricing.  The real
// `player_walk()` still does all of those once the predicate says yes.

static uint16_t ro_step_axis(uint16_t base, uint16_t delta, bool twice) {
  uint16_t v = (uint16_t)(base + delta);
  if (twice) v = (uint16_t)(v + delta);
  return v;
}

static void ro_step_propose(const Wram* w, const Rom* rom, uint16_t page,
                            uint16_t* want_x, uint16_t* want_y) {
  const uint16_t dir = wram_r16(w, (uint32_t)page + STEP_DP_DIR);
  const uint16_t index = (uint16_t)(((dir & STEP_DIR_CARDINAL) << 2) +
                                    wram_r16(w, (uint32_t)page + STEP_DP_SPEED_CLASS));
  const uint16_t mask = rom_word(rom, STEP_SPEED_MASKS + index);
  const bool twice = (mask & wram_r16(w, W_SCHED_TICK)) != 0;
  const uint16_t dx = rom_word(rom, STEP_DELTA_X + dir);
  const uint16_t dy = rom_word(rom, STEP_DELTA_Y + dir);
  *want_x = ro_step_axis(wram_r16(w, (uint32_t)page + WALK_DP_X), dx, twice);
  *want_y = ro_step_axis(wram_r16(w, (uint32_t)page + WALK_DP_Y), dy, twice);
}

static uint16_t ro_probe_offset(const Wram* w, int i) {
  const uint16_t col = (uint16_t)((i % TERRAIN_PROBE_COLS) * 2);
  if (i < TERRAIN_PROBE_COLS) return col;
  return (uint16_t)(wram_r16(w, W_TILEMAP_ROW_BYTES) + col);
}

static uint16_t ro_probe_attrs(const Wram* w, uint16_t map, uint16_t yoff) {
  const uint32_t entry_off = ((uint32_t)(TERRAIN_MAP_BANK & 1) << 16) + map + yoff;
  const uint16_t entry = wram_r16(w, entry_off);
  const uint16_t tile = (uint16_t)((entry & TILEMAP_INDEX_MASK) << 1);
  const uint16_t attrs = wram_r16(w, W_TILE_ATTRS);
  const uint8_t bank = wram_r8(w, W_TILE_ATTRS + 2);
  return wram_r16(w, ((uint32_t)(bank & 1) << 16) + attrs + tile);
}

// Return true when the footprint is solid. `reaction` says that the exact
// solid attribute word belongs to the ROM reaction path and therefore makes
// this walk unsupported by the native routine today.
static bool ro_ground_blocked(const Wram* w, uint16_t x, uint16_t y,
                              bool* reaction) {
  const uint16_t row = (uint16_t)(((uint16_t)(y - TERRAIN_ORIGIN_Y) >>
                                   TERRAIN_TILE_SHIFT) & TERRAIN_TILE_MASK);
  const uint16_t col = (uint16_t)(((uint16_t)(x - TERRAIN_ORIGIN_X) >>
                                   TERRAIN_TILE_SHIFT) & TERRAIN_TILE_MASK);
  const uint16_t map = (uint16_t)(col + wram_r16(w, W_TILE_ROW_BASE + row));
  *reaction = false;
  for (int i = 0; i < TERRAIN_PROBE_COUNT; i++) {
    const uint16_t attrs = ro_probe_attrs(w, map, ro_probe_offset(w, i));
    if (attrs & TERRAIN_MASK_SOLID) {
      // terrain_blocked() leaves A shifted right one before walk_tile_reaction.
      *reaction = walk_tile_reaction((uint16_t)(attrs >> 1)) != 0;
      return true;
    }
  }
  return false;
}

static uint16_t ro_abs_diff(const Wram* w, uint16_t v, uint16_t rec,
                            uint16_t field_off) {
  const uint16_t d = (uint16_t)(v - wram_r16(w, (uint16_t)(rec + field_off)));
  return (d & 0x8000u) ? (uint16_t)(~d + 1u) : d;
}

static uint16_t ro_manhattan(const Wram* w, uint16_t x, uint16_t y,
                             uint16_t rec) {
  return (uint16_t)(ro_abs_diff(w, x, rec, ACTOR_X) +
                    ro_abs_diff(w, y, rec, ACTOR_Y));
}

static bool ro_tether_blocked(const Wram* w, uint16_t x, uint16_t y) {
  const bool is_a = wram_r16(w, W_SCHED_CUR_TASK) == wram_r16(w, W_PLAYER_A_TASK);
  const uint16_t ref = is_a ? wram_r16(w, W_PLAYER_B_RECORD)
                            : wram_r16(w, W_PLAYER_A_RECORD);
  if (ref == 0) return false;

  const uint16_t wx = (uint16_t)((uint16_t)(x - wram_r16(w, (uint16_t)(ref + ACTOR_X))) +
                                 TETHER_BIAS_X);
  if (wx < TETHER_SPAN_X) {
    const uint16_t wy = (uint16_t)((uint16_t)(y - wram_r16(w, (uint16_t)(ref + ACTOR_Y))) +
                                   TETHER_BIAS_Y);
    if (wy < TETHER_SPAN_Y) return false;
  }

  const uint16_t want = ro_manhattan(w, x, y, ref);
  const uint16_t a_rec = wram_r16(w, W_PLAYER_A_RECORD);
  const uint16_t b_rec = wram_r16(w, W_PLAYER_B_RECORD);
  const uint16_t apart = (uint16_t)(
      ro_abs_diff(w, wram_r16(w, (uint16_t)(a_rec + ACTOR_X)), b_rec, ACTOR_X) +
      ro_abs_diff(w, wram_r16(w, (uint16_t)(a_rec + ACTOR_Y)), b_rec, ACTOR_Y));
  // The ROM allows only a move that makes the two players strictly closer.
  return apart <= want;
}

static bool ro_axis_near(uint16_t pos, uint16_t target) {
  return (uint16_t)(pos - target + AT_POINT_HALF_WINDOW) < AT_POINT_WINDOW;
}

static bool ro_obstacle_at_point(const Wram* w, uint16_t x, uint16_t y) {
  const uint16_t count = wram_r16(w, W_VISIBLE_ACTOR_COUNT);
  if (count == 0) return false;
  const uint16_t player_a = wram_r16(w, W_PLAYER_A_RECORD);
  const uint16_t player_b = wram_r16(w, W_PLAYER_B_RECORD);

  for (int32_t i = (int32_t)count - 2; i >= 0; i -= 2) {
    const uint16_t rec = wram_r16(w, W_VISIBLE_ACTORS + (uint32_t)i);
    if (rec == player_a || rec == player_b) continue;
    const uint16_t flags = wram_r16(w, (uint32_t)rec + ACTOR_FLAGS);
    if (!(flags & ACTOR_ACTIVE)) continue;
    const uint16_t id = wram_r16(w, (uint32_t)rec + ACTOR_COLLIDE_ID);
    if (id == 0) continue;
    if (id >= OBSTACLE_ID_BAND_LO) {
      if (id <= OBSTACLE_ID_BAND_HI) continue;
      if (id >= OBSTACLE_ID_CEILING) continue;
    }
    bool named = false;
    for (int k = 0; k < OBSTACLE_ID_SKIP_COUNT; k++) {
      if (id == OBSTACLE_ID_SKIP[k]) { named = true; break; }
    }
    if (named) continue;
    if (!ro_axis_near(wram_r16(w, (uint32_t)rec + ACTOR_X), x)) continue;
    if (!ro_axis_near(wram_r16(w, (uint32_t)rec + ACTOR_Y), y)) continue;
    return true;
  }
  return false;
}

static bool ro_out_of_bounds(const Wram* w, uint16_t x, uint16_t y) {
  if (x & 0x8000u) return true;
  const uint16_t cx = (uint16_t)(x >> 2);
  if (cx < TERRAIN_BOUNDS_MIN_X) return true;
  if ((uint16_t)(cx + TERRAIN_BOUNDS_PAD_X) >= wram_r16(w, W_TILEMAP_ROW_BYTES))
    return true;

  if (y & 0x8000u) return true;
  const uint16_t cy = (uint16_t)(y >> 3);
  if (cy < TERRAIN_BOUNDS_MIN_Y) return true;
  return (uint16_t)(cy + TERRAIN_BOUNDS_PAD_Y) >= wram_r16(w, W_TILEMAP_ROWS);
}

static bool ro_can_step(const Wram* w, Point from, Point to, bool* unsupported) {
  bool reaction = false;
  if (ro_ground_blocked(w, to.x, to.y, &reaction)) {
    if (reaction) *unsupported = true;
    return false;
  }
  if (ro_tether_blocked(w, to.x, to.y)) return false;
  if (ro_obstacle_at_point(w, to.x, to.y) &&
      !ro_obstacle_at_point(w, from.x, from.y))
    return false;
  if (ro_out_of_bounds(w, to.x, to.y)) return false;
  return true;
}

bool walk_supported_readonly(const Wram* w, const Rom* rom, uint16_t page) {
  if (wram_r16(w, (uint32_t)page + WALK_DP_MODE) & WALK_TWICE) return false;

  uint16_t want_x = 0, want_y = 0;
  ro_step_propose(w, rom, page, &want_x, &want_y);
  Point here = {wram_r16(w, (uint32_t)page + WALK_DP_X),
                wram_r16(w, (uint32_t)page + WALK_DP_Y)};
  bool unsupported = false;

  const Point across = {want_x, here.y};
  if (ro_can_step(w, here, across, &unsupported)) here.x = across.x;
  if (unsupported) return false;

  const Point vertical = {here.x, want_y};
  (void)ro_can_step(w, here, vertical, &unsupported);
  return !unsupported;
}

bool walk_supported(Wram* w, const Rom* rom, uint16_t page) {
  Walk k = {w, rom, page, false, NULL};
  if (field(&k, WALK_DP_MODE) & WALK_TWICE) {
    PORT_COVER(walk_twice);
    return false;
  }
  walk(&k);
  return !k.met_reaction;
}
