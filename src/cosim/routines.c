// The registry: every routine `src/port/` has replaced, plus the shim that
// adapts it to the 65816's calling convention.
//
// The split matters. `src/port/` holds ordinary C with ordinary signatures —
// `sprite_frame_tile(Wram*, uint16_t frame)` — and knows nothing about
// registers, stacks or emulators. Everything about *how the ROM called it*
// lives here, in the harness, and dies with the harness in Phase 4. Without
// that line, "port code" would slowly turn into 65816 written in C.
//
// A shim's whole job is to say what the registers meant on the way in and what
// the ROM leaves in them on the way out. The second half is the fiddly one, so
// each shim below cites the instruction that decides it.

#include <string.h>

#include "cosim/cosim.h"

#include "port/apu.h"
#include "port/boss.h"
#include "port/bossbg.h"
#include "port/camera.h"
#include "port/collide.h"
#include "port/r52_fast_guard.h"
#include "port/fade.h"
#include "port/floor.h"
#include "port/hud.h"
#include "port/levelmap.h"
#include "port/lzss.h"
#include "port/monster.h"
#include "port/oam.h"
#include "port/player.h"
#include "port/player_resume.h"
#include "port/hotpaths.h"
#include "port/rng.h"
#include "port/bodies.h"
#include "port/sched.h"
#include "port/score.h"
#include "port/sprite_cache.h"
#include "port/step.h"
#include "port/walk.h"
#include "port/chase.h"
#include "port/terrain.h"
#include "port/thread.h"
#include "port/trig.h"
#include "port/vblank.h"

// R36 shipping path: once an ordinary translated routine runs atomically on
// the Xbox CPU, its old 65816 cycle-price is diagnostics only.  Avoid executing
// those pricing expressions in the hot path; hardware-timed/reference calls
// still request exact costs through cosim_native_lean_costs().
#if defined(XBOX_PORT) && defined(ZAMN_R36_NATIVE_LEAN)
#define R36_COSIM_COST(expr) do { if (!cosim_native_lean_costs()) cosim_cost((expr)); } while (0)
#define R36_COSTS_NEEDED() (!cosim_native_lean_costs())
#else
#define R36_COSIM_COST(expr) cosim_cost((expr))
#define R36_COSTS_NEEDED() (true)
#endif

// ---------------------------------------------------------------------------
// $80:B9D6  sprite_frame_tile — A = frame number, A = OAM tile word
// ---------------------------------------------------------------------------

// The routine opens with `STX $38` and closes with `LDX $38`: with one index
// register, spilling the caller's X to scratch is the only way to use X for the
// lookup. `sprite_frame_tile()` keeps it in a C local, so the spill slot is the
// one place its WRAM legitimately differs from the ROM's. Nothing reads $38
// across the call — `$80:CD20` uses the same two bytes as decompressor state,
// which is only safe because both treat it as scratch.
static const CosimExclude SPRITE_TILE_EXCLUDES[] = {
    {0x0038, 2, "the ROM spills the caller's X here; the port keeps it in a local"},
};

// What one lookup cost the 65816, from which of its runs the lookup took. Same
// shape as the display list's three walks, with one difference worth naming:
// **this is the first model whose bytes are not all program bytes.**
//
// `LDA $B447,X`, `LDA $B547,X` and `LDA $B647,X` read the slot geometry through
// the data bank, and with the data bank at `$80` that is a fast ROM read — 6
// master cycles a byte while `$420D` is set and 8 while it is clear, exactly
// like an opcode fetch. `CosimRun::bytes` is "bytes that cost 2 more with
// FastROM off", so those six data bytes belong in it: the hit path is 12
// program bytes and 14 counted ones.
//
// This is what made `tools/cycles816.py` grow a `fast_rom()` and start calling
// the column FastROM bytes rather than program bytes. Every model before this
// one either touched WRAM only or reached its table through a low bank, where
// the two counts are the same number, so the distinction had never come up.
//
// The whole thing turns on the data bank being `$80`, so the shim checks.
static const CosimRun TILE_COST[TILE_BLOCK_COUNT] = {
    // $80:B9D6..$80:B9EB, `BMI` not taken. 288, and 288 is exactly the minimum
    // `verify` measures over the corpus — the model's first witness.
    [TILE_BLK_HIT]        = {104 + 184, 10 + 14},
    // The same prologue with the branch taken, then $80:B9EC..$80:B9F5.
    [TILE_BLK_MISS]       = {104 + 6 + 132, 10 + 12},
    // $80:B9F6 `CMP : BNE` not taken, then `INX : INX : CPX : BNE` taken.
    [TILE_BLK_SCAN_NEXT]  = {52 + 54 + 6, 5 + 7},
    // ...and the same with that `BNE` falling through to `LDX #$0000 : BRA`.
    [TILE_BLK_SCAN_WRAP]  = {52 + 54 + 30 + 6, 5 + 7 + 5},
    [TILE_BLK_SCAN_FOUND] = {52 + 6, 5},
    // $80:BA07..$80:BA11, `BMI` taken.
    [TILE_BLK_SLOT_EMPTY] = {120 + 6, 11},
    // ...not taken, so $80:BA12..$80:BA19 unmaps the frame that was there.
    [TILE_BLK_SLOT_EVICT] = {120 + 70, 11 + 8},
    // $80:BA1A..$80:BA50. Two ROM table reads, hence 55 program bytes and 59.
    [TILE_BLK_TAIL]       = {724, 55 + 4},
};

static int tile_cycles(const SpriteTileWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < TILE_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&TILE_COST[i], fast);
  return cycles;
}

static void shim_sprite_frame_tile(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  uint16_t queued_before = wram_r16(w, W_SPRITE_UPLOAD_COUNT);

  SpriteTileWork work;
  out->a = sprite_frame_tile_counted(w, in->a, &work);

  // The direct page has to be zero rather than merely page-aligned: the port
  // reads `W_SPRITE_TICK` and friends at their absolute addresses, so a call on
  // any other page would be a porting bug before it was a pricing one. The data
  // bank has to be `$80` for the three table reads above to be fast ROM.
  if (in->d == 0 && in->db == 0x80) R36_COSIM_COST(tile_cycles(&work, in->fastrom));

  // `LDX $38` puts the caller's X back, unchanged.
  out->x = in->x;

  // Y is only touched on the miss path, where `LDY $7C ... INY INY STY $7C`
  // leaves it holding the new upload count. On a hit the routine never mentions
  // Y at all.
  uint16_t queued_after = wram_r16(w, W_SPRITE_UPLOAD_COUNT);
  out->y = queued_after != queued_before ? queued_after : in->y;

  // That same `LDX $38` is the last flag-setting instruction before the `RTS`,
  // so N and Z describe the restored X.
  out->n = (out->x & 0x8000) != 0;
  out->z = out->x == 0;

  // Carry is claimed rather than derived. The last instruction to touch it is
  // `ASL A` on the hit path — carry is bit 15 of the frame number — and the
  // `ADC` that adds the frame array's bank on the miss path. Frame numbers are
  // 12 bits and the bank add cannot overflow, so both are 0 for every frame the
  // game can actually draw. Asserting the simple answer and letting `verify`
  // check it against all 10,354 real calls is a better trade than duplicating
  // the address arithmetic here to predict a bit that never varies.
  out->c = false;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B9C7  sprite_cache_age — no arguments
// ---------------------------------------------------------------------------

static void shim_sprite_cache_age(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  (void)rom;
  sprite_cache_age(w);

  // `LDA $0020 : DEC A` before the loop, and the loop never reloads A.
  out->a = (uint16_t)(wram_r16(w, W_SCHED_TICK) - 1);
  // The loop is `LDX #$00FE ... DEX DEX BPL`, so it falls out at $FFFE with the
  // branch's N and Z describing it.
  out->x = 0xfffe;
  out->y = in->y;
  out->n = true;
  out->z = false;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;
}

// ---------------------------------------------------------------------------
// $80:8398  thread_tick_waits — no arguments
// ---------------------------------------------------------------------------

static void shim_thread_tick_waits(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  thread_tick_waits(w);

  // A holds whatever the last iteration — slot 0 — left there, and each of the
  // three paths through the loop body leaves A equal to the word the slot ends
  // up holding: unchanged when the slot is empty or already expired, and the
  // decremented value when it is stored. So A is just slot 0, afterwards.
  out->a = wram_r16(w, W_THREAD_WAIT);
  out->x = 0xfffe;  // `DEX DEX BPL`, as above
  out->y = in->y;
  out->n = true;
  out->z = false;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;

  // The only instruction here that touches carry is `CMP #$8000`, and it is
  // only reached for a slot that is live — where A is $8000 or above by
  // definition, so the comparison always sets it. A pass over 24 empty slots
  // never executes the `CMP` at all and leaves carry alone, which is why the
  // claim is conditional rather than a flat `true`.
  for (int slot = 0; slot < WRAM_THREAD_SLOTS; slot++) {
    if (wram_r16(w, W_THREAD_WAIT + (uint32_t)slot * 2) & 0x8000) {
      out->c = true;
      out->flags |= COSIM_FLAG_C;
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// $80:83AE / $80:8418  the vblank queue adders — A = address, Y = bank
// ---------------------------------------------------------------------------

// Carry is these two routines' *return value*, and it is the one flag here that
// a caller definitely reads: `$82:AE3A` and `$82:AEA8` both do
// `JSL vbl_queue_b_add : BCS <back>` and spin until the job is accepted.
//
// `CPY #$0008 : BCS` is what sets it — carry clear means the count was below
// the cap and the job went in, carry set means it did not. Nothing after that
// touches carry, so it survives to the `RTL` on both paths.
//
// This is worth dwelling on, because getting it wrong is what the harness was
// built to catch and it very nearly was not caught. `verify` compares only the
// flags a shim claims to model, and the first version of these shims claimed N
// and Z but not carry — so 107 calls passed while the substitution left carry
// at whatever the caller happened to have. Under `run`, the caller's retry loop
// never exited and the routine was entered 147,405 times instead of 107. An
// unclaimed flag is not a small omission; it is an unchecked output.
//
// The three lines that decide it now live in `port/thread.c`, because
// `$80:C07F` ends `JML $8083AE` and hands the same three flags back as its own
// — see `port/hud.c`. What stays here is which of them this shim claims.
static void queue_flags(Wram* w, uint32_t count_at, uint16_t y_in, bool added,
                        CosimRegs* out) {
  VblQueueFlags f;
  vbl_queue_flags(w, count_at, y_in, added, &f);
  out->n = f.n;
  out->z = f.z;
  out->c = f.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_vbl_queue_a_add(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  int slot = vbl_queue_a_add(w, in->a, in->y);
  if (slot < 0) {  // full: `PLY : RTL` puts everything back
    out->a = in->a;
    out->x = in->x;
    out->y = in->y;
    queue_flags(w, W_VBL_QUEUE_A_COUNT, in->y, false, out);
    return;
  }
  // `DEC A : TAY` leaves the stored address in both A and Y, and X is the slot
  // the search stopped on.
  out->a = (uint16_t)(in->a - 1);
  out->y = out->a;
  out->x = (uint16_t)slot;
  queue_flags(w, W_VBL_QUEUE_A_COUNT, in->y, true, out);
}

static void shim_vbl_queue_b_add(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  // Queue B has no `TAY`, so Y keeps the last word its search read. The search
  // stops the moment it reads a zero, so that is 0 — except when it falls off
  // the bottom without testing slot 0, which leaves slot 1's address in Y.
  uint16_t probe = wram_r16(w, W_VBL_QUEUE_B + 4);

  int slot = vbl_queue_b_add(w, in->a, in->y);
  if (slot < 0) {
    out->a = in->a;
    out->x = in->x;
    out->y = in->y;
    queue_flags(w, W_VBL_QUEUE_B_COUNT, in->y, false, out);
    return;
  }
  // The bank is pulled back off the stack into A *after* the address is stored,
  // so A ends up holding the bank rather than the address.
  out->a = in->y;
  out->x = (uint16_t)slot;
  out->y = slot == 0 ? probe : 0;
  queue_flags(w, W_VBL_QUEUE_B_COUNT, in->y, true, out);
}

// ---------------------------------------------------------------------------
// $80:891A  fade_in — no arguments, and it suspends
// ---------------------------------------------------------------------------

// The first shim for a routine that does not run to completion.
//
// A suspension is an exit like any other, and the reason to say so out loud is
// the carry bug above. It would be easy to treat a yield as "the routine is not
// finished, so there is nothing to check yet" — but the state at the `JSL
// thread_yield` is handed straight to `PHP`, parked with the thread, and given
// back by `PLP` when it resumes. Anything wrong there is wrong for the rest of
// the routine, and in native mode nothing else would ever set it. So a
// suspension declares its registers and flags exactly as a return does.
//
// `in` is captured per *segment* — at the routine's entry, and again at each
// resumption — so "unchanged" here means unchanged across this run of the
// routine's own instructions, not across the suspension. That is the claim the
// routine's listing can actually support, and it is the one that survives Phase
// 4 replacing the scheduler underneath it.
//
//   * Neither X nor Y is mentioned anywhere in `$80:891A-$80:8932`, so both come
//     back as the segment found them.
//   * At a suspension, `LDA #$0001` is the last instruction before the `JSL`:
//     A is the sleep count, and N and Z describe it.
//   * Carry at a suspension depends on **which** suspension, and this is the one
//     thing here that is not obvious from reading the routine top to bottom. The
//     loop reaches the `JSL` by falling through `CMP #$000F`, which borrows for
//     every brightness below 15 and so leaves carry clear. The *first*
//     suspension is entered from the top of the routine and never executes that
//     `CMP` at all, so it carries the caller's own carry through untouched.
//     Fifteen of the sixteen segments agree with the simple answer, which is
//     exactly why it is worth getting right rather than guessing.
//   * At the return, `LDA $136C : CMP #$000F` is the tail: A is $000F, and 15
//     minus 15 is zero with no borrow, so N=0, Z=1, C=1. Nothing between that
//     and the `RTL` touches any of them.
static PortStep shim_fade_in(Wram* w, const Rom* rom, const CosimRegs* in,
                             CosimRegs* out, void* ctx, uint16_t* ticks) {
  (void)rom;
  FadeCtx* fade = (FadeCtx*)ctx;
  // Which segment is about to run, read before the call advances it.
  bool from_top = fade->co.resume == PORT_CORO_ENTRY;

  PortStep step = fade_in(w, fade, ticks);

  out->x = in->x;
  out->y = in->y;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;

  if (step == PORT_YIELDED) {
    // A is filled in from `ticks` by the harness — the sleep count *is* the
    // accumulator, and having one place decide that keeps the two from drifting.
    out->n = false;  // the count is 1: positive, non-zero
    out->z = false;
    out->c = from_top ? in->c : false;
    return step;
  }

  out->a = 0x000f;
  out->n = false;
  out->z = true;
  out->c = true;
  return step;
}

// ---------------------------------------------------------------------------
// $80:BC7F  actor_depth_sort — no arguments
// ---------------------------------------------------------------------------

// The relink needs to remember the record in front of the one it is looking at,
// and with one index register the only place to keep it is a direct-page byte.
// It is the same $38 `sprite_frame_tile` spills the caller's X into, and it is
// scratch for the same reason: nothing reads it across the call. The port keeps
// the predecessor in a C local.
static const CosimExclude DEPTH_SORT_EXCLUDES[] = {
    {0x0038, 2, "the ROM keeps the walk's predecessor here; the port uses a local"},
};

// What one pass would have cost the 65816, from what the pass did — the routine
// `cosim_cost` was written for, and the case that showed why a constant is not
// enough. `$80:BC7F` runs every frame and costs anywhere from 92 cycles to
// 7,524; declared at its mean of 1,605 it drifts `movies/level25-2p` off the
// stock core's framebuffer by frame 2,700.
//
// Every constant below is the sum of one straight-line run of the listing, at
// the access times this ROM actually gets: 6 master cycles for an opcode or
// operand byte (the boot code sets `$420D`, so banks $80+ are fast), 8 for a
// data byte — the display list and its head both live under `$7E:2000`, and
// every bank the data-bank register can hold reaches them in 8 — and 6 for an
// internal cycle. `LDX $1B5E : BEQ : RTS` is 34 + 18 + 40 = 92, which is exactly
// the minimum `verify` measures, and the whole model is checked against the ROM
// on every call.
//
// Refresh is deliberately absent, and so is the fetch penalty: see `cosim_cost`
// and `CosimRun`.
//
// $80:BC7F LDX $1B5E : BEQ (taken) : $80:BCE1 RTS.
static const CosimRun SORT_EMPTY = {34 + 18 + 40, 6};
// ...BEQ not taken, then LDY $12,X : BEQ (taken) : RTS.
static const CosimRun SORT_SINGLE = {34 + 12 + 34 + 18 + 40, 10};
// The same four instructions with both branches falling through: what every
// call with a pair in it starts with.
static const CosimRun SORT_PROLOGUE = {34 + 12 + 34 + 12, 9};
// $80:BCB0 LDY $12,X : BEQ, not taken — one more record to look at.
static const CosimRun SORT_STEP = {34 + 12, 4};
// ...and taken, which is where every full walk ends: + $80:BCE1 RTS.
static const CosimRun SORT_EXIT = {34 + 18 + 40, 5};
// $80:BCAD STX $38 : TYX, the advance both no-swap paths branch to and the head
// falls into.
static const CosimRun SORT_ADVANCE = {28 + 12, 3};
// $80:BCA3 / $80:BCCF, the two relinks. The head's moves the list head; the
// other has a predecessor to fix up, reloads X from `$38`, and jumps back to
// the top of the walk rather than through the advance.
static const CosimRun SORT_SWAP_HEAD = {40 + 34 + 34 + 34, 10};
static const CosimRun SORT_SWAP_MID = {40 + 34 + 34 + 28 + 34 + 28 + 40 + 18, 18};

// The four compares, in `ActorSortCmp` order. Each is `LDA $00,X : EOR $0000,Y
// : AND #$0020` — 92 cycles over 8 bytes — plus its own branch and whichever
// second test it needed, and each includes the branch it ends on, so the caller
// adds nothing.
static const CosimRun SORT_CMP[ACTOR_SORT_CMP_COUNT] = {
    [ACTOR_SORT_CMP_FIRST_SWAP] = {92 + 12 + 40 + 18 + 18, 18},
    [ACTOR_SORT_CMP_FIRST_NOSWAP] = {92 + 12 + 40 + 18 + 12 + 18, 20},
    [ACTOR_SORT_CMP_Y_SWAP] = {92 + 18 + 34 + 40 + 12, 17},
    [ACTOR_SORT_CMP_Y_NOSWAP] = {92 + 18 + 34 + 40 + 18, 17},
};

static int depth_sort_cycles(const ActorSortWork* k, bool fast) {
  if (k->empty) return cosim_run_cycles(&SORT_EMPTY, fast);
  if (k->single) return cosim_run_cycles(&SORT_SINGLE, fast);

  int cycles = cosim_run_cycles(&SORT_PROLOGUE, fast) +
               cosim_run_cycles(&SORT_EXIT, fast);
  for (int i = 0; i < ACTOR_SORT_CMP_COUNT; i++)
    cycles += k->compares[i] * cosim_run_cycles(&SORT_CMP[i], fast);
  if (k->swap_head) cycles += cosim_run_cycles(&SORT_SWAP_HEAD, fast);
  cycles += k->swap_mid * cosim_run_cycles(&SORT_SWAP_MID, fast);
  cycles += k->steps * cosim_run_cycles(&SORT_STEP, fast);
  // Once for the head, then once per loop step that did not relink — a mid-list
  // swap jumps straight back to `$80:BCB0`.
  cycles += (1 + k->steps - k->swap_mid) * cosim_run_cycles(&SORT_ADVANCE, fast);
  return cycles;
}

static void shim_actor_depth_sort(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  (void)rom;
  ActorSortWork work;
  uint16_t tail = actor_depth_sort_counted(w, &work);

  // Every direct-page address the routine touches costs one extra internal
  // cycle when the direct page is not page-aligned, and the model above does not
  // carry that term because no caller has ever presented one. If a caller ever
  // does, this reports nothing and the routine falls back to its declared mean —
  // visibly, as a `priced` below `checked` in the cost-model report.
  //
  // The data bank needs no such guard: every address the walk touches is under
  // `$2000`, which costs 8 cycles a byte through any bank there is.
  if ((in->d & 0xff) == 0)
    R36_COSIM_COST(depth_sort_cycles(&work, in->fastrom));

  // Every one of the three `RTS` paths is reached by a taken `BEQ`, so N and Z
  // are the same on all of them however the walk ended.
  out->n = false;
  out->z = true;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;

  // X is whichever record the walk stopped on: the tail, found by `LDY $12,X`
  // reading a zero link — or 0 from `LDX $1B5E` when the list was empty, which
  // is the one path that never touches Y.
  out->x = tail;
  out->y = tail != 0 ? 0 : in->y;
  out->regs = COSIM_REG_X | COSIM_REG_Y;

  // A and carry are not claimed, and the reason is worth writing down given how
  // much of this file is about unclaimed outputs being unchecked ones.
  //
  // Both are left holding an intermediate of whichever comparison ended the
  // pass, and *which* intermediate differs per path: A is either the flags word
  // masked to bit 5, or the Y coordinate that lost a `CMP`, or the link a swap
  // read; carry is that `CMP`'s result, or the caller's own if the pass never
  // reached one. Reproducing that here would mean writing the comparison a
  // second time in the shim, which is exactly the drift this file exists to
  // prevent.
  //
  // They are dead. The single caller is `$80:BD27`, and the next thing it does
  // is `JSR $80:BCE2`, which opens `LDY #$0000 : LDX $1B5E : BEQ` — it reads
  // neither — and whose own first use of carry is a `SEC`.
}

// ---------------------------------------------------------------------------
// $80:BCE2  actor_cull — no arguments
// ---------------------------------------------------------------------------

// What each run of `$80:BCE2` costs, indexed by `ActorCullBlock`. Same rules as
// the sort's table above: 6 cycles for a program byte with FastROM on, 8 for a
// byte of the display list, the camera or `visible_actors` — all of which live
// under `$7E:2000` and cost 8 through every bank the data-bank register can hold
// — and 6 for an internal cycle. Branch costs are folded into the block the
// outcome names, so a tally of the blocks needs nothing added to it.
//
// The empty-list case is the check that the rest is priced the same way:
// `LDY #$0000 : LDX $1B5E : BEQ` taken plus `STY $9C : RTS` is 70 + 68 = 138,
// which is exactly the minimum `verify` measures over the corpus.
static const CosimRun CULL_COST[CULL_BLOCK_COUNT] = {
    [CULL_BLK_EMPTY] = {18 + 34 + 18 + 28 + 40, 11},
    [CULL_BLK_PROLOGUE] = {18 + 34 + 12, 8},
    [CULL_BLK_UNDRAWN] = {34 + 18, 4},
    [CULL_BLK_DRAWN] = {34 + 12, 4},
    [CULL_BLK_SCREEN] = {12 + 18, 3},
    [CULL_BLK_WORLD] = {12 + 12, 3},
    // LDA $02,X : SEC : SBC $1B6A : CMP #$FF80 : BCS — 98 over 9 bytes, plus
    // the branch. The Y axis is the same five instructions off `$06` and `$1B6C`.
    [CULL_BLK_X_HIGH] = {98 + 18, 11},
    [CULL_BLK_X_TEST] = {98 + 12, 11},
    [CULL_BLK_X_IN] = {18 + 12, 5},
    [CULL_BLK_X_OUT] = {18 + 18, 5},
    [CULL_BLK_Y_HIGH] = {98 + 18, 11},
    [CULL_BLK_Y_TEST] = {98 + 12, 11},
    [CULL_BLK_Y_IN] = {18 + 12, 5},
    [CULL_BLK_Y_OUT] = {18 + 18, 5},
    [CULL_BLK_EMIT] = {12 + 40 + 12 + 12, 6},
    [CULL_BLK_ADVANCE] = {34 + 12 + 18, 5},
    [CULL_BLK_EXIT] = {34 + 12 + 12 + 28 + 40, 8},
};

static int cull_cycles(const ActorCullWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < CULL_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&CULL_COST[i], fast);
  return cycles;
}

static void shim_actor_cull(Wram* w, const Rom* rom, const CosimRegs* in,
                            CosimRegs* out) {
  (void)rom;
  ActorCullWork work;
  actor_cull_counted(w, rom, &work);

  // `LDA $00,X`, `LDA $02,X`, `LDA $06,X`, `LDA $12,X` and `STY $9C` each cost
  // one extra internal cycle when the direct page is not page-aligned, and the
  // table does not carry that term because no caller presents one. The data bank
  // needs no guard: every address the walk touches is under `$2000`.
  if ((in->d & 0xff) == 0) R36_COSIM_COST(cull_cycles(&work, in->fastrom));

  // The walk ends on `LDA $12,X : TAX : BNE`, so it falls out with the zero
  // link in both A and X. An empty list exits earlier, from `LDX $1B5E : BEQ`,
  // which leaves X zero the same way but never touches A. `actor_cull` does not
  // move the list, so its head still says which of the two happened.
  out->a = wram_r16(w, W_ACTOR_LIST_HEAD) != 0 ? 0 : in->a;
  out->x = 0;
  // `STY $9C` is the count, straight out of Y.
  out->y = wram_r16(w, W_VISIBLE_ACTOR_COUNT);
  out->n = false;
  out->z = true;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;

  // Carry is the last window comparison the walk happened to make — one of four
  // `CMP`s, on whichever record was tested last, or the caller's if every record
  // was skipped on its flags. It is dead: the caller's next instruction is
  // `JSR $80:BC23`, which reaches its first `ADC` through the `CLC` at
  // $80:BC2F.
}

// ---------------------------------------------------------------------------
// $80:BC23  oam_buffer_clear — no arguments
// ---------------------------------------------------------------------------

// The one routine here whose cost does not depend on anything, and it is worth
// pricing anyway.
//
// `PHD : LDA #$13BE : TCD` means the caller's direct page cannot reach it, the
// eight iterations are `LDX #$0008` and nothing else, and every address it
// touches is under `$2000`. So this is a straight line 372 bytes long and there
// is nothing to count: 94 for the prologue, 24 for `LDY #$E0 : CLC`, 3,898 for
// eight passes of the sixteen `STY`s (482 each, +6 for the seven taken `BNE`s),
// 562 for `LDA #$AAAA` and its sixteen stores, and 92 to unwind.
//
// Every direct-page access in it pays an extra internal cycle, because `$13BE`
// is not page-aligned and neither is any of the eight pages `ADC #$0040` walks
// it through — the low byte alternates `$BE` and `$FE` and is never `$00`.
//
// **The interesting part is that pricing a constant is not redundant**, and the
// reason is the difference between the two burns rather than anything about
// this routine. `.cycles` is a mean of what `verify` *measured*, so it already
// contains the three or four refreshes the call crossed; `cycles_burn` hands it
// to the core in a single piece and the core adds one more. A reported cost is
// instruction cycles only and `cycles_burn_modelled` hands it over twelve at a
// time, so the core puts every refresh back exactly where the scanlines are.
//
// For this routine that is 4,814 + 40 against a real 4,790..4,830: about 43
// cycles a call too slow, every call, forever. It is the least interesting kind
// of drift there is and also the easiest to leave in place, because a routine
// whose measured spread is 40 wide looks like one there is nothing left to say
// about.
static const CosimRun OAM_CLEAR_COST = {94 + 24 + 3898 + 562 + 92, 372};

static void shim_oam_buffer_clear(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  (void)rom;
  oam_buffer_clear(w);
  R36_COSIM_COST(cosim_run_cycles(&OAM_CLEAR_COST, in->fastrom));

  // `LDA #$AAAA` and the sixteen stores of it are the last thing to touch A.
  out->a = 0xaaaa;
  // `SEP #$10` zeroes the high bytes of both index registers on the way in, so
  // when `REP #$30` widens them again X is the loop counter run down to 0 and Y
  // is still the $E0 it was seeded with.
  out->x = 0x0000;
  out->y = 0x00e0;
  // `PLD` is the last flag-setting instruction, so N and Z describe the direct
  // page it restores rather than anything the routine computed. The only caller
  // is `sprite_build_oam`, which has just done `PEA $0000 : PLD`, so what comes
  // back off the stack is zero.
  out->n = false;
  out->z = true;
  // `CLC` at $80:BC2F, and the `ADC #$0040` that walks the direct page across
  // the buffer eight times starts at $13BE and never carries out of 16 bits.
  out->c = false;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:825E  thread_spawn — A:Y = the far entry, D = the caller's page
// ---------------------------------------------------------------------------

// The one routine so far whose *third* argument is the caller's direct page
// itself rather than something on it: its last act is to copy five words off
// that page onto the new thread's, so `in->d` is an input in the same way
// `player_collide`'s is, and for a completely different reason.
static void shim_thread_spawn(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  int slot = thread_spawn(w, rom, in->a, in->y, in->d);

  // `TXA : RTL` on success, `LDA #$0000 : RTL` when the board is full — and the
  // ROM cannot tell those two apart either, because slot 0 doubled is also 0.
  out->a = slot < 0 ? 0 : (uint16_t)slot;
  // X is the slot the search settled on and survives to the `RTL`; on the full
  // path the search ran off the bottom at $FFFE and `PLA : PLD` do not touch it.
  out->x = slot < 0 ? 0xfffe : (uint16_t)slot;
  // Y is *not* the bank any more by the time it returns: the argument copy ends
  // `LDY #$0008 : LDA ($01,S),Y`, so what comes back is the last offset it read.
  // The harness found this on call 1 — WRAM matched and only Y did.
  out->y = slot < 0 ? in->y : (THREAD_SPAWN_ARGS - 1) * 2;
  // `TXA` is the last flag-setting instruction on the success path and the
  // `LDA #$0000` on the other; both describe what is in A.
  out->n = (out->a & 0x8000) != 0;
  out->z = out->a == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;
}

// ---------------------------------------------------------------------------
// $80:8480  thread_call_handler — X = slot x2, Y = the argument, and it may
//                                 decline
// ---------------------------------------------------------------------------

// The dispatcher's tail, and the only place it is written down.
//
// `$80:8480` has two exits and they set N and Z from completely different
// instructions. A slot with no handler leaves through `LDA $1300,X : ORA
// $1330,X : BEQ $84B0`, so the flags describe the zero that `ORA` produced. A
// slot with one leaves through `PLX : PLD : PLB`, and `PLB` is the last of
// those to set a flag — so N and Z describe the *data bank* being restored,
// which has nothing to do with anything the routine computed. That is why
// `CosimRegs` carries `db`: it is an input to this routine's flags.
//
// A, X, Y and carry are the port's, because they are the handler's and the
// dispatcher passes them straight through.
static void handler_exit(const CosimRegs* in, const ThreadCallResult* t,
                         CosimRegs* out) {
  out->a = t->a;
  out->x = t->x;
  out->y = t->y;
  out->c = t->c;
  out->n = t->entered ? (in->db & 0x80) != 0 : false;
  out->z = t->entered ? in->db == 0 : true;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static int fast_guard_thread_call_handler(const Wram* live,
                                          const CosimRegs* in) {
  // The $1300 thread table is WRAM only in low-bank mirrors.
  if (!(in->db < 0x40 || (in->db >= 0x80 && in->db < 0xc0)))
    return R52_DEFER_TO_SANDBOX;
  uint32_t missing = 0;
  R52GuardDecision decision = r52_handler_slot_decision(live, in->x, &missing);
#if defined(ZAMN_R68_INPUT_READONLY_GUARDS)
  if (decision == R52_DEFER_TO_SANDBOX)
    decision = r68_thread_input_decision(live, in->x, in->y);
#endif
#if defined(ZAMN_R70_HOT_THREAD_9A6D)
  if (decision == R52_DEFER_TO_SANDBOX)
    decision = r70_thread_hot_ignore_decision(live, in->x, in->y);
#endif
  if (decision == R52_REJECT_UNPORTED && missing)
    cosim_census_note("handler", missing);  // preserve diagnostic census
  return (int)decision;
}

static bool guard_thread_call_handler(Wram* scratch, const Rom* rom,
                                      const CosimRegs* in) {
  ThreadCallResult out;
  if (thread_call_handler(scratch, rom, in->x, in->y, in->c, &out)) return true;

  // Declined. `handler_unported` already counts these; what it cannot say is
  // *which* handler, and a count with no address is not a work list. The
  // dispatcher hands the address back in `unported`.
  //
  // It is zero when the port *has* the handler and the handler declined one
  // level down — a jump-table entry, or an id an enemy `JML`s out on. Censusing
  // the door those came through would name the wrong routine, and each of them
  // is named properly by its own registry entry's guard.
  //
  // **This used to be a list of the four handlers that could decline internally,
  // and by the eighth copy of `$81:8888` it wanted eight.** Nothing would have
  // reported a missing entry: the symptom is a census line naming a routine the
  // port already has, which is exactly the shape of the bug that hid
  // `monster_collide`'s missing dispatch for four rounds.
  if (out.unported) cosim_census_note("handler", out.unported);
  return false;
}

// What the dispatch cost, or `false` when the handler it entered has no cost
// table yet. Defined far below, after every handler's own cost function, since
// it is the sum of the frame and one of those — see `ThreadCallWork`.
static bool thread_call_cycles(const ThreadCallWork* k, const CosimRegs* in,
                               int* out);

static void shim_thread_call_handler(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  ThreadCallResult t;
  ThreadCallWork work;
  // the guard allowed it
  thread_call_handler_counted(w, rom, in->x, in->y, in->c, &t, &work);
  handler_exit(in, &t, out);

  // `LDA $1300,X` and the two beside it reach the thread tables through the
  // data bank, and every bank that mirrors low WRAM costs the same 8 a byte —
  // so the model holds for `$7E` and for the `$80` the sprite pass leaves, and
  // asks only that it is not a bank where `$1300` would be ROM.
  if (R36_COSTS_NEEDED()) {
    int cycles;
    if ((in->db < 0x40 || (in->db >= 0x80 && in->db < 0xc0)) &&
        thread_call_cycles(&work, in, &cycles))
      R36_COSIM_COST(cycles);
  }
}

// ---------------------------------------------------------------------------
// $80:F7F7  player_collide — A = the other actor's id, D = the player's page
// ---------------------------------------------------------------------------

// Registered even though `thread_call_handler` already calls it, for the reason
// that made `actor_collide_notify` worth its own entry: the enclosing routine
// declines every dispatch whose handler is unported, so a handler seen only
// through it would never be offered the calls that go somewhere else. Its own
// entry PC gets all of them.
//
// It is also the first ported routine whose direct page is not `$0000`. The
// dispatcher installed the player thread's page from `$80:82DE` before `RTL`ing
// here, and every `$xx` in the listing is an offset into it — which is why the
// shim hands the port `in->d` rather than assuming, and why a listing read with
// `zamn_disasm` needs the same caveat (it resolves direct-page operands as
// though `D` were zero).
static bool guard_player_collide(Wram* scratch, const Rom* rom,
                                 const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y};
  uint32_t unported = 0;
  if (player_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  // Name the routine that is missing, and let the port say which one that is.
  // For most ids it is the jump-table entry — the entry, not the id, because
  // twelve ids sharing a target are one piece of work and `$80:F8D6` is more
  // use in a report than `$29`. For a pickup it is not the entry at all:
  // `$80:F87B` *is* ported and what it ran out of road on is the auto-select
  // it tail-calls. Censusing the table entry there would put a routine that
  // already exists at the top of the work list.
  cosim_census_note("player id table", unported);
  return false;
}

// Both handlers return the same way — `CLC : RTL` for the player, `CLC : RTL`
// or `SEC : RTL` for the enemy — so what a shim has to say is just which
// registers the path it took left behind. The port fills all of them, because
// which exit ran is exactly the thing the port knows and the shim does not.
static void handler_regs(const ActorHandlerRegs* r, CosimRegs* out) {
  out->a = r->a;
  out->x = r->x;
  out->y = r->y;
  out->n = r->n;
  out->z = r->z;
  out->c = r->c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// What `$80:F950` costs, indexed by `PlayerHurtBlock`. Every one of its reads
// is direct-page except `LDA $1CBC,X`, which goes through the data bank to a
// WRAM mirror, so the third column carries almost the whole routine.
static const CosimRun HURT_COST[HURT_BLOCK_COUNT] = {
    [HURT_BLK_STATE_A] = {28 + 18 + 18, 7, 1},
    [HURT_BLK_STATE_B] = {28 + 18 + 12 + 18 + 18, 12, 1},
    // ...and neither matched, so `LDX $0E : LDA $1CBC,X : CMP #$0004` runs.
    [HURT_BLK_STATE_PASS] = {28 + 18 + 12 + 18 + 12 + 28 + 40 + 18, 20, 2},
    [HURT_BLK_WEAPON_OTHER] = {18, 2},
    [HURT_BLK_WEAPON_MATCH] = {12 + 28, 4, 1},
    [HURT_BLK_HEALTH_SET] = {18, 2},
    [HURT_BLK_HEALTH_ZERO] = {12, 2},
    [HURT_BLK_TIMER] = {28, 2, 1},
    [HURT_BLK_IFRAMES] = {18, 2},
    // $80:F96E LDA #$8001 : STA $50 : LDA #$0040 : STA $52, behind the `BPL`
    // that did not branch.
    [HURT_BLK_TAKEN] = {12 + 18 + 28 + 18 + 28, 12, 2},
    [HURT_BLK_RTS] = {40, 1},
};

static int hurt_cycles(const uint16_t* blk, bool fast, bool dp_unaligned) {
  int cycles = 0;
  for (int i = 0; i < HURT_BLOCK_COUNT; i++)
    cycles +=
        blk[i] * cosim_run_cycles_dp(&HURT_COST[i], fast, dp_unaligned);
  return cycles;
}

// `$80:F7F7`'s own two blocks. The dispatch block runs from the `CMP` to the
// `RTL` and includes the `JSR ($F808,X)` but not what it reached: that is the
// target's, and `PlayerCollideWork::target` says which.
static const CosimRun PLAYER_COST[PLAYER_BLOCK_COUNT] = {
    [PLAYER_BLK_IGNORE] = {18 + 18 + 12 + 42, 7},
    [PLAYER_BLK_DISPATCH] = {18 + 12 + 12 + 12 + 34 + 28 + 52 + 12 + 42, 19, 1},
};

// `$80:F87A`, the entry seventeen ids share: a bare `RTS` and nothing else.
static const CosimRun PLAYER_TARGET_NOP = {40, 1};

// Returns false when the id dispatched to a jump-table entry with no table
// here. Ten of the seventeen targets are ported and two are priced, so this
// declines on more calls than it serves for now — see `docs/cosim.md`.
static bool player_cycles(const PlayerCollideWork* k, bool fast,
                          bool dp_unaligned, int* out) {
  int cycles = 0;
  for (int i = 0; i < PLAYER_BLOCK_COUNT; i++)
    cycles +=
        k->blocks[i] * cosim_run_cycles_dp(&PLAYER_COST[i], fast, dp_unaligned);

  if (k->blocks[PLAYER_BLK_IGNORE]) {  // the id was out of range: no dispatch
    *out = cycles;
    return true;
  }
  switch (k->target) {
    case PLAYER_COLLIDE_NOP:
      cycles += cosim_run_cycles_dp(&PLAYER_TARGET_NOP, fast, dp_unaligned);
      break;
    case PLAYER_COLLIDE_HURT:
      cycles += hurt_cycles(k->hurt, fast, dp_unaligned);
      break;
    default:
      return false;
  }
  *out = cycles;
  return true;
}

static void shim_player_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y};
  PlayerCollideWork work;
  // the guard allowed it
  player_collide_counted(w, rom, in->d, in->a, &r, NULL, &work);
  handler_regs(&r, out);

  if (R36_COSTS_NEEDED()) {
    int cycles;
    if (player_cycles(&work, in->fastrom, (in->d & 0xff) != 0, &cycles))
      R36_COSIM_COST(cycles);
  }
}

// ---------------------------------------------------------------------------
// $80:CC3B  apu_play_sfx — A = the sound effect id
// ---------------------------------------------------------------------------

// The first ported routine that talks to hardware, and the first whose flags
// are decided by a register nobody thought they were passing: `PLD` restores
// the caller's direct page and sets N and Z from it. See `port/apu.h`.
//
// Registered in its own right rather than only through the two collision
// entries that reach it, and this one earns it more than most: the movie's
// callers are six different routines (`$80:9738`, `$80:D085`, `$80:E9D1`,
// `$80:EA9F`, `$80:F87F`, `$82:AE94`), only one of which is on the collision
// path. Everything else about it would go unchecked.
static void shim_apu_play_sfx(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  (void)rom;
  ApuSfxRegs r;
  apu_play_sfx(w, in->a, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:CCBF  apu_next_byte — nothing in, one byte out, cursor advanced
// ---------------------------------------------------------------------------

// The most-called entry in this registry, and the one where a shim's "which
// registers did you model" mask earns its keep the other way round: X and Y are
// never touched by five instructions that mention neither, so they are claimed
// by simply handing back what came in, and any surprise is a diff.
//
// The routine runs eight bits wide, and `out->a`'s high byte is the caller's —
// see `port/apu.h`, which is the same preservation `apu_send` documents.
static void shim_apu_next_byte(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  ApuNextRegs r;
  apu_next_byte(w, rom, in->a, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;
}

// ---------------------------------------------------------------------------
// $80:9D5B  spawn_has_room — nothing in, carry out
// ---------------------------------------------------------------------------

// Six instructions, no arguments and no writes: it reads two globals and
// answers with a flag. The port takes a `const Wram*` for that reason, which is
// the only shim here whose routine could not modify memory if it wanted to.
static void shim_spawn_has_room(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)rom;
  SpawnRoomRegs r;
  spawn_has_room(w, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:9C90  sin_deg — A = degrees, A = sin x 128
// ---------------------------------------------------------------------------

// Registered at the inner `JSR` target rather than at `$80:9C8C`, the four-byte
// `JSR : RTL` trampoline in front of it, because that is where the work is and
// every call reaches it either way. The trampoline is therefore *subsumed*: it
// is two instructions neither side can get wrong.
//
// N and Z come off the closing `PLX`, so they belong to the caller's own index
// register — the third routine in this registry whose flags describe an
// argument nobody thought they were passing, after `apu_play_sfx`'s `PLD` and
// `terrain_point_bit2`'s. Carry is the `CMP #$FF`'s and means *sentinel*.
static void shim_sin_deg(Wram* w, const Rom* rom, const CosimRegs* in,
                         CosimRegs* out) {
  (void)w;
  SinRegs r;
  sin_deg(rom, in->a, in->x, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:F327  actor_publish_pos — nothing in; the record is on the thread's page
// ---------------------------------------------------------------------------

// Everything it needs is on `in->d`, which is why `CosimRegs` carries a direct
// page at all: this routine takes no register arguments and would be
// uncallable without it.
static void shim_actor_publish_pos(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  PublishRegs r;
  actor_publish_pos(w, in->d, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;
}

// ---------------------------------------------------------------------------
// $81:8024  nearest_player_dist — the point is on the page too
// ---------------------------------------------------------------------------

static void shim_nearest_player_dist(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  NearestRegs r;
  nearest_player_dist(w, in->d, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:C05A  sprite_cache_init — A:Y is the frame array, and DB is the answer
// ---------------------------------------------------------------------------

// **The first routine here that `verify` cannot score and `run` can.**
// `$80:C05A` is 16,900 instructions of two stores in a loop — 346,734 master
// cycles, which is 0.97 of a frame — so an NMI lands inside very nearly every
// call and the per-call harness abandons all of them: two calls on `boot.zmv`,
// two on `level1.zmv`, zero checked, twice interrupted. For three rounds that
// was read as "unregisterable", and `tools/native_share.py` still carried the
// address in `BLOCKED` beside `$80:CD20 lzss_decompress` and `$80:AD2B
// blockmap_expand`.
//
// That reading was one instrument too narrow. What an interrupt breaks is the
// *rewind-and-replay* claim: the ROM's NMI handler wrote WRAM inside the call
// window and the port, which models the routine and not the handler, cannot
// account for it. It breaks nothing about substitution. Under `run` the ROM
// never executes these instructions at all, so there is no window for an
// interrupt to land inside — the core sits at the instruction after the `JSL`
// while the budget is burned, and if an NMI falls due during it the core takes
// it exactly as it would have anyway.
//
// So this is the first entry marked `run_only`, and it is the mirror of
// `verify_only` in both mechanism and honesty: that flag means *checked on
// every call and never substituted*, this one means *substituted and never
// checked per call*. What checks it instead is `run` itself, which compares all
// 128 KB of WRAM once per scheduler pass — a claim about a stretch rather than
// about a call, which is the claim Phase 4 has to be built on. See
// `CosimRoutine::run_only`.
//
// **Its cycle figure is a count rather than a mean**, which is the other half of
// what makes it safe to substitute unmeasured. Every other entry's `.cycles` is
// an average `verify` observed; there is no average to observe here, and there
// does not need to be one — the routine has no data dependence and no branch
// that is not the loop, so `tools/cycles816.py` prices it exactly:
//
//     prologue $C05A..$C06A                                       184
//     loop 1   4,097 x (STA abs,X + DEX + DEX) + 4,096 taken BPL  335,948
//     LDX #$00FE                                                   18
//     loop 2   128 x the same + 127 taken BPL                   10,490
//     epilogue PLB : PLB : RTL                                      94
//
// `BPL` and not `BNE` is why the counts are 4,097 and 128 rather than 4,096 and
// 127 — the same off-by-one that makes the routine write `$2002` bytes, which
// `port/sprite_cache.h` records from the other direction.
//
// **0.97 of a frame is a safety margin and not a coincidence to ignore.** The
// core takes at most one pending interrupt when it resumes, so a substituted
// call that spans more than one NMI boundary would leave one NMI un-taken that
// the ROM took — a real divergence, and the thing that makes "outlives a frame"
// the right worry even under `run`. At 346,734 the call fits inside a frame and
// can straddle at most one boundary, so it cannot. With FastROM *off* the same
// routine costs 405,930, which can straddle two. Measured, `run` burns 357,194
// master cycles a call including the refreshes the core adds, which is 346,734
// plus 261 of them — so `$420D` is set on every call any movie makes, and the
// margin is real rather than assumed. If an input ever reaches this routine
// with FastROM off, this is the line that says what to expect.
//
// It is also the first routine here whose *only* claimed flags come from
// `in->db`. That field was added for `$80:8480 thread_call_handler`, whose exit
// `PLB` restores its caller's bank; this is the same instruction used the same
// way, two banks apart.
static const CosimRun SPRITE_CACHE_INIT_COST = {184 + 335948 + 18 + 10490 + 94,
                                                29598};

static void shim_sprite_cache_init(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;  // no table, no ROM read
  SpriteCacheInitRegs r;
  sprite_cache_init(w, in->a, in->y, in->db, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;
  R36_COSIM_COST(cosim_run_cycles(&SPRITE_CACHE_INIT_COST, in->fastrom));
}

// ---------------------------------------------------------------------------
// $80:9570  wave_hdma_build — everything is on the wobble thread's page
// ---------------------------------------------------------------------------

// **Three of the sixteen blocks price a routine that has a registry entry of
// its own.** The far call at `$80:959A` costs the trampoline `$80:9C8C JSR :
// RTL` plus whichever arm of `$80:9C90 sin_deg` the table byte sent it down,
// plus the caller's own `BIT #$8000 : BEQ : ORA #$FF00`, which is decided by
// the same bit as the arm. `sin_deg` is in the registry two entries up and that
// is not a duplicate: when `wave_hdma_build` is substituted its body never
// runs, so the `JSL` never happens and the shim that would have priced those
// 223 calls is never entered. One entry is what `verify` checks `sin_deg`
// with; this is what `run` pays for it.
//
// The two agree, which is the check. `sin_deg`'s own row measures a floor of
// **240** cycles over 2,676 calls, and 118 + 48 + 74 — the shared prologue, the
// sentinel arm, `PLX : RTS` — is 240 exactly. Its call count is the other
// check: **2,676 is 12 x 223**, the twelve calls a level movie makes here
// against the 223 iterations of a full-length table, and it was measured long
// before anything counted the loop.
//
// The three arms differ by 36 cycles and the negative one is taken on roughly
// half the circle, so the difference is real money — about 2,700 cycles a
// full-length call, which is 1.7% of it and forty times the spread the twelve
// level-movie calls measure between them.
// In the three `SIN` rows, 82 is the trampoline's `JSR $9C90 : RTL`; 118 is
// `PHX : SEP : TAX : LDA $839431,X : CMP : REP` — the `LDA` is a long-indexed
// read out of bank $83, so with FastROM on it costs 30 and the data byte is one
// of the run's fetched bytes; 74 is `PLX : RTS`; and the 36 or 48 on the end is
// the caller's. None of it touches the direct page: `sin_deg` is written to be
// callable from anywhere and it is.
static const CosimRun WAVE_COST[WAVE_BLOCK_COUNT] = {
    // $80:9570 LDA $70 : BMI taken : $95DA RTS.
    [WAVE_BLK_OVER] = {28 + 18 + 40, 5, 1},
    // ...BMI not taken, through `LDA #$00F8 : STA $7E8000,X : INX`. The header
    // store is sixteen bits into bank $7E, which is 8 a byte and never fast.
    [WAVE_BLK_PROLOGUE] = {244, 26, 3},
    // ...with `$957C LDA #$0000` in it, which is the phase's yearly wrap.
    [WAVE_BLK_PROLOGUE_WRAP] = {256, 29, 3},
    // $80:958D INY x4 : CPY #$0168 : BCC taken.
    [WAVE_BLK_HEAD] = {48 + 18 + 18, 9},
    // ...not taken : LDY #$0000. Twice or three times a call, four degrees of
    // arc at a time.
    [WAVE_BLK_HEAD_WRAP] = {48 + 18 + 12 + 18, 12},
    // $80:9599 TYA : JSL $809C8C ... $95A6 STA $7E8000,X : INX : INX : CPX
    // #$00F1 — everything in the body that does not depend on an outcome.
    [WAVE_BLK_ITER] = {12 + 54 + 40 + 12 + 12 + 18, 14},
    // $80:9C9C BNE not taken : LDA #$0080 : BRA taken, and bit 15 clear, so the
    // caller's `BEQ` skips its `ORA`: + 36.
    [WAVE_BLK_SIN_SENTINEL] = {82 + 118 + 12 + 18 + 18 + 74 + 36, 31},
    // ...taken : BIT #$0080 : BEQ not taken : ORA #$FF00 : BRA taken. The one
    // arm that sets bit 15, so the caller's dead `ORA` runs too: + 48.
    [WAVE_BLK_SIN_NEGATIVE] = {82 + 118 + 18 + 18 + 12 + 18 + 18 + 74 + 48, 39},
    // ...and `BEQ` taken : AND #$00FF.
    [WAVE_BLK_SIN_POSITIVE] = {82 + 118 + 18 + 18 + 18 + 18 + 74 + 36, 34},
    // $80:95AF BNE taken: 222 of the 223 iterations of a full-length call.
    [WAVE_BLK_HEADER_SKIP] = {18, 2},
    // ...not taken : PHA : LDA #$F800 : STA $7E7FFF,X : INX : PLA. Once.
    [WAVE_BLK_HEADER_FIXUP] = {12 + 28 + 18 + 40 + 12 + 34, 12},
    // $80:95BB CPX $70 : BCC taken / not taken.
    [WAVE_BLK_STEP] = {28 + 18, 4, 1},
    [WAVE_BLK_EXIT] = {28 + 12, 4, 1},
    // $80:95BF PHA : LDA #$0000 : STA $7E8000,X : PLA : CMP #$0000 — 138 over
    // 12 bytes — and then one of three tails, each ending on the shared `RTS`.
    [WAVE_BLK_OFF_AXIS] = {138 + 18 + 40, 15},
    [WAVE_BLK_HOLD] = {138 + 12 + 28 + 12 + 12 + 28 + 18 + 40, 24, 2},
    // `DEC $70` twice, and a direct-page read-modify-write is 50 apiece.
    [WAVE_BLK_RETRACT] = {138 + 12 + 28 + 18 + 50 + 50 + 40, 23, 3},
};

static int wave_cycles(const WaveWork* k, bool fast, bool dp_unaligned) {
  int cycles = 0;
  for (int i = 0; i < WAVE_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles_dp(&WAVE_COST[i], fast,
                                                 dp_unaligned);
  return cycles;
}

// Carry is an input for the same reason `enemy_collide`'s is: one exit does not
// touch it. Here it is the first instruction's `BMI`, which is the exit taken
// on every frame after the effect has finished — so the path that needs the
// passthrough is the common one rather than the rare one.
static void shim_wave_hdma_build(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  WaveRegs r;
  WaveWork work;
  wave_hdma_build_counted(w, rom, in->d, in->x, in->y, in->c, &r, &work);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
  R36_COSIM_COST(wave_cycles(&work, in->fastrom, (in->d & 0xff) != 0));
}

// ---------------------------------------------------------------------------
// $81:9BF3  actor_step_bearing — A is a doubled direction
// ---------------------------------------------------------------------------

// X is claimed, and it is the leftover of whichever of three *other* ported
// routines refused the second axis. That is only checkable because all three
// model their own X: `terrain_blocked_enemy` and `actor_at_point` write it, and
// `terrain_out_of_bounds` provably does not touch either index register. The
// port therefore reproduces a register it never chose, by composition.
static void shim_actor_step_bearing(Wram* w, const Rom* rom, const CosimRegs* in,
                                    CosimRegs* out) {
  StepBearingRegs r;
  actor_step_bearing(w, rom, in->d, in->a, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $81:C16B, $81:C00B — the monster's walk, and its load
// ---------------------------------------------------------------------------

// Two entries for what is nearly one routine: `$81:C16B` falls into `$81:C00B`
// on three of its four paths, so registering the caller alone would leave the
// callee unchecked on the frames it is reached from anywhere else — and there
// is nowhere else, which is exactly the sort of claim worth making the harness
// prove rather than reading off a listing.
//
// V is not claimed by either. On `$81:C00B`'s working path it is a coordinate
// addition's overflow, on its guard path it is the caller's own, and the walk
// above never touches it; the one caller in the ROM reads none of the four.
static void shim_monster_place_carried(Wram* w, const Rom* rom,
                                       const CosimRegs* in, CosimRegs* out) {
  MonsterCarryRegs r;
  monster_place_carried(w, rom, in->d, in->a, in->x, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_monster_anim(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  MonsterAnimRegs r;
  monster_anim(w, rom, in->d, in->x, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $81:BB75, $81:BBA4  monster_seek, monster_deliver — nothing in but the page
// ---------------------------------------------------------------------------
//
// The other half of each of the creature's states: `monster_anim` draws and one
// of these two decides where to go. Neither takes an argument — the first
// instruction of one is `STZ $24` and of the other `LDA $0A` — so the direct
// page is the whole calling convention.
//
// Both end in a `JMP` to a two-instruction stub whose `RTS` is the one that
// returns, which is why `ret_op` below points at an address that does not look
// like either routine's last instruction. `ret_op` decides where a *substituted*
// call is sent, not how a returning one is recognised, so any `RTS` inside the
// routine does the job; see `port/monster.h`.
static void shim_monster_seek(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  MonsterSeekRegs r;
  monster_seek(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_monster_deliver(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  MonsterDeliverRegs r;
  monster_deliver(w, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:BE0C, $80:BE41 — a display record's two ends
// ---------------------------------------------------------------------------

static void shim_actor_slot_alloc(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  (void)rom;
  SlotAllocRegs r;
  actor_slot_alloc(w, in->db, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// `in->d` is not scratch here the way it is everywhere else in this file — the
// routine installs page zero for itself and puts the caller's back with `PLD`,
// which is what decides N and Z on the only path that changes anything.
static void shim_actor_slot_free(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  SlotFreeRegs r;
  actor_slot_free(w, in->a, in->d, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $81:8888  enemy_collide — the same argument, on an enemy's page
// ---------------------------------------------------------------------------

// Carry is an input as well as an output, and only on one path: `enemy_die`
// hands whatever it arrived with to `score_add`, whose discard path passes it
// straight through to the `RTL`. Every other exit sets it outright.
static bool guard_enemy_collide(Wram* scratch, const Rom* rom,
                                const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  // Two ids left, and the census names the routine each goes to rather than the
  // id, for the reason `player_collide` does: the routine is the piece of work.
  cosim_census_note("enemy id", unported);
  return false;
}

// The two exits that end inside `$81:8888`, indexed by `EnemyCollideBlock`.
// `ENEMY_BLK_DEEP` has no entry on purpose: a call that reached it left through
// a `JML` or a `JSR` into a routine this file does not describe, and the model
// declines rather than pricing the part it can see.
static const CosimRun ENEMY_COST[ENEMY_BLOCK_COUNT] = {
    [ENEMY_BLK_IGNORE] = {18 + 12 + 12 + 42, 7},
    // $81:888F through the damage subtraction, the `BMI` falling through, and
    // the `BEQ` at `$81:88AF` taking it to the `CLC : RTL` at `$81:88C8`.
    [ENEMY_BLK_NO_DAMAGE] = {312 + 18 + 54, 43, 3},
};

static bool enemy_cycles(const EnemyCollideWork* k, bool fast,
                         bool dp_unaligned, int* out) {
  if (k->blocks[ENEMY_BLK_DEEP]) return false;
  int cycles = 0;
  for (int i = 0; i < ENEMY_BLOCK_COUNT; i++)
    cycles +=
        k->blocks[i] * cosim_run_cycles_dp(&ENEMY_COST[i], fast, dp_unaligned);
  *out = cycles;
  return true;
}

static void shim_enemy_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  EnemyCollideWork work;
  // the guard allowed it
  enemy_collide_counted(w, rom, in->d, in->a, &r, NULL, &work);
  handler_regs(&r, out);

  if (R36_COSTS_NEEDED()) {
    int cycles;
    if (enemy_cycles(&work, in->fastrom, (in->d & 0xff) != 0, &cycles))
      R36_COSIM_COST(cycles);
  }
}

// ---------------------------------------------------------------------------
// $81:C4A6  monster_collide — a second enemy subsystem, on a third kind of page
// ---------------------------------------------------------------------------

// Carry is an input on the same one path `enemy_collide`'s is: the death tail
// hands whatever arrived to `score_add`, whose discard path passes it through.
static bool guard_monster_collide(Wram* scratch, const Rom* rom,
                                  const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (monster_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  // One decline left — id `$5D`'s `JML $81:BB05`, the other splice in the
  // `$81:8506` family — and the census names the routine rather than the id for
  // the reason it always has.
  cosim_census_note("monster id", unported);
  return false;
}

// The five exits that end inside `$81:C4A6`, indexed by `MonsterCollideBlock`,
// and the same table prices `$81:C440` — see the enum for the three bytes that
// differ and why none of them is on a priced path.
//
// **The last block is a cross-check and it is an exact one.** This routine is
// `$81:8888` on a different page, and its zero-damage exit prices to
// `384` over `43` bytes with `3` direct-page instructions — which is
// `ENEMY_BLK_NO_DAMAGE`, digit for digit, arrived at from a listing eleven
// kilobytes away and never compared until it was written down. Neither has ever
// been measured: no shot in the game carries a damage-table entry of zero, so
// both are transcription and both say the same thing.
static const CosimRun MONSTER_COST[MONSTER_BLOCK_COUNT] = {
    // $81:C4A6 CMP #$005C : BCS not taken : CMP #$000C : BCC taken, into the
    // shared `CLC : RTL` at `$81:C50A`.
    [MON_BLK_IGNORE_LOW] = {18 + 12 + 18 + 18 + 12 + 42, 12},
    // ...taken instead, then `CMP #$0033 : BCC` not taken, which falls into a
    // *different* `CLC : RTL` — the one at `$81:C4B5`.
    [MON_BLK_IGNORE_HIGH] = {18 + 12 + 18 + 12 + 18 + 12 + 12 + 42, 17},
    // ...the third comparison branching to `$81:C4EC LDA $26 : BNE` taken.
    [MON_BLK_LATCHED] = {96 + 28 + 18 + 12 + 42, 21, 1},
    // ...not taken, so the record is rewritten, the latch stored and
    // `JSR $C04A` — `LDA #$C050 : STA $12 : RTS`, 86 — queues the next routine.
    // The `BNE $C503` is taken here: `$0042` is not the one value that redirects.
    [MON_BLK_TAKE] = {96 + 28 + 12 + 28 + 18 + 40 + 34 + 18 + 18 + 28 + 40 + 86 +
                          12 + 42,
                      48, 4},
    // ...and not taken, which costs the branch's 6 back and one more absolute
    // load: `LDA $0046` at 34 over 3 bytes.
    [MON_BLK_TAKE_ALT] = {96 + 28 + 12 + 28 + 18 + 40 + 34 + 18 + 12 + 34 + 28 +
                              40 + 86 + 12 + 42,
                          51, 4},
    // $81:C4B7 through the damage subtraction, the `BMI` falling through, and
    // the `BEQ` at `$81:C4D7` taking it to the `CLC : RTL` at `$81:C50A`.
    [MON_BLK_NO_DAMAGE] = {18 + 18 + 312 + 18 + 54, 43, 3},
};

static bool monster_cycles(const MonsterCollideWork* k, bool fast,
                           bool dp_unaligned, int* out) {
  if (k->blocks[MON_BLK_DEEP]) return false;
  int cycles = 0;
  for (int i = 0; i < MONSTER_BLOCK_COUNT; i++)
    cycles +=
        k->blocks[i] * cosim_run_cycles_dp(&MONSTER_COST[i], fast, dp_unaligned);
  *out = cycles;
  return true;
}

static void shim_monster_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  MonsterCollideWork work;
  // the guard allowed it
  monster_collide_counted(w, rom, in->d, in->a, &r, NULL, &work);
  handler_regs(&r, out);

  if (R36_COSTS_NEEDED()) {
    int cycles;
    if (monster_cycles(&work, in->fastrom, (in->d & 0xff) != 0, &cycles))
      R36_COSIM_COST(cycles);
  }
}

// ---------------------------------------------------------------------------
// $81:C440  monster_c440_collide — the same creature one stage earlier
// ---------------------------------------------------------------------------

// Registered separately even though the port body is shared with the routine
// above, and the reason is the same one that made `player_collide` worth its own
// entry: the two are different addresses in the ROM, so `verify` intercepting
// one never intercepts the other, and the flags each leaves are checked only at
// its own entry PC. A shared implementation is a claim that they compute the
// same thing; two registry entries are what test it.
static bool guard_monster_c440_collide(Wram* scratch, const Rom* rom,
                                       const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (monster_c440_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("c440 id", unported);
  return false;
}

static void shim_monster_c440_collide(Wram* w, const Rom* rom,
                                      const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  MonsterCollideWork work;
  // the guard allowed it
  monster_c440_collide_counted(w, rom, in->d, in->a, &r, NULL, &work);
  handler_regs(&r, out);

  if (R36_COSTS_NEEDED()) {
    int cycles;
    if (monster_cycles(&work, in->fastrom, (in->d & 0xff) != 0, &cycles))
      R36_COSIM_COST(cycles);
  }
}

// ---------------------------------------------------------------------------
// $81:B41C  enemy_b41c_collide — the same routine a third time
// ---------------------------------------------------------------------------

// Carry is not an input here, unlike both twins: this copy's death path awards
// nothing, so there is no `score_add` to pass a caller's carry through. Every
// exit sets it.
static bool guard_enemy_b41c_collide(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_b41c_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("b41c id", unported);
  return false;
}

static void shim_enemy_b41c_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_b41c_collide(w, rom, in->d, in->a, &r, NULL);  // the guard allowed it
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:D7F6  enemy_d7f6_collide — the fifth copy, on level 17
// ---------------------------------------------------------------------------

static bool guard_enemy_d7f6_collide(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_d7f6_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("d7f6 id", unported);
  return false;
}

static void shim_enemy_d7f6_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_d7f6_collide(w, rom, in->d, in->a, &r, NULL);  // the guard allowed it
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:9B6B  enemy_9b6b_collide — the sixth copy, on level 21
// ---------------------------------------------------------------------------

static bool guard_enemy_9b6b_collide(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_9b6b_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("9b6b id", unported);
  return false;
}

static void shim_enemy_9b6b_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_9b6b_collide(w, rom, in->d, in->a, &r, NULL);  // the guard allowed it
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:9063  enemy_9063_collide — the seventh copy, on level 5
// ---------------------------------------------------------------------------

static bool guard_enemy_9063_collide(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_9063_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("9063 id", unported);
  return false;
}

static void shim_enemy_9063_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_9063_collide(w, rom, in->d, in->a, &r, NULL);  // the guard allowed it
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:AC92  enemy_ac92_collide — the ninth copy, on level 49
// ---------------------------------------------------------------------------

static bool guard_enemy_ac92_collide(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_ac92_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("ac92 id", unported);
  return false;
}

static void shim_enemy_ac92_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_ac92_collide(w, rom, in->d, in->a, &r, NULL);  // the guard allowed it
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:E6E4  enemy_e6e4_collide — the tenth copy, and the one with two owners
// ---------------------------------------------------------------------------
//
// It has no `supported` guard, and that is a claim rather than an omission: both
// ids that leave by `JML` are served (`$81:83C6` is `enemy_bubble_react`,
// `$81:847E` is `enemy_freeze`), so there is no argument this routine can be
// handed that it declines. `actor_845e_collide` is the other entry with none,
// for the opposite reason — it cannot write, so there is nothing to try on a
// scratch copy. This one can write plenty; it just never gives up.

static void shim_enemy_e6e4_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_e6e4_collide(w, rom, in->d, in->a, &r, NULL);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:990B  enemy_990b_collide — the eleventh copy, and the one the census
//                                asked for by name
// ---------------------------------------------------------------------------
//
// No `supported` guard, for `enemy_e6e4_collide`'s reason: `$5E` goes to
// `enemy_bubble_react` and `$5D` to `enemy_freeze` through a store of its own,
// and both have been ported since the level-49 round, so nothing this routine
// can be handed makes it give up.
//
// `$81:9633` gets no registry entry of its own. It is reached by `JSR` from one
// branch of one caller and never by `JSL`, so the harness has no call to offer
// it — the four instructions are checked as part of this entry or not at all.

static void shim_enemy_990b_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_990b_collide(w, rom, in->d, in->a, &r, NULL);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:96E4  enemy_990b_spin_collide — the census line, answered
// ---------------------------------------------------------------------------
//
// The entry above put an address on the decline census the round it was written,
// and said in advance which one: `$81:9643` installs this as the collision
// handler for the length of the spin, so anything that staggers the creature
// then touches it is asking for a routine the port did not have. It is the
// second entry in the registry to arrive by that route — name predicted first,
// bytes read second — and the shortest of the two by a factor of thirteen.
//
// No `rom`, because there is no damage table on this path: the routine that
// looks one up is the one this replaces.

static void shim_enemy_990b_spin_collide(Wram* w, const Rom* rom,
                                         const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_990b_spin_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:845E  actor_845e_collide — no WRAM at all, so no `w` and no guard body
// ---------------------------------------------------------------------------

// It cannot decline and it cannot write, so `supported` is left NULL: there is
// nothing to try on a scratch copy. It is the first entry in the registry with
// that shape, and the reason is the routine's, not the harness's.
static void shim_actor_845e_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)w;
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_845e_collide(in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:EDAA  shot_edaa_collide — one byte
// ---------------------------------------------------------------------------

// `out` arrives as a copy of `in`, so "nothing changed" needs no assignment at
// all — only the claim. All four flags, because an `RTL` sets none of them and
// the point of the entry is that this is checkable: the one routine in the
// registry whose entire specification is that it does nothing.
static void shim_shot_edaa_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)w;
  (void)rom;
  (void)in;
  shot_edaa_collide();
  out->flags =
      COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C | COSIM_FLAG_V;
}

// ---------------------------------------------------------------------------
// $81:F6A3  shot_f6a3_collide — one shot handler, four weapons
// ---------------------------------------------------------------------------

// It cannot decline — three named ids and an else — so there is nothing for a
// guard to try, and like `actor_845e` it leaves `supported` NULL. Unlike
// `actor_845e` it does write, so it gets `w` and the whole-WRAM diff covers the
// one store.
static void shim_shot_f6a3_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  shot_f6a3_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $82:F4EF  actor_f4ef_collide — the two players, and nothing else
// ---------------------------------------------------------------------------

static void shim_actor_f4ef_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                    CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_f4ef_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:D301  enemy_d301_collide — the eighth copy, on level 9
// ---------------------------------------------------------------------------

static bool guard_enemy_d301_collide(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  uint32_t unported = 0;
  if (enemy_d301_collide(scratch, rom, in->d, in->a, &r, &unported)) return true;
  cosim_census_note("d301 id", unported);
  return false;
}

static void shim_enemy_d301_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  // The seeded carry is load-bearing here in a way it is not for the rest of the
  // family: a survivor's tail hands whatever `enemy_survived_react` returned to
  // `rng_next`, whose `ROL` shifts it into the state byte. Get it wrong and the
  // creature draws a different number.
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_d301_collide(w, rom, in->d, in->a, &r, NULL);  // the guard allowed it
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:847E  enemy_freeze — reached by JML, so its RTL is its caller's caller's
// ---------------------------------------------------------------------------

// Registered on its own entry PC as well as being called from six handlers, for
// `player_collide`'s reason: a routine seen only through a caller that declines
// is never offered the calls that go somewhere else. Here it is the other way
// round — every one of the six is ported — but the entry PC is also the only
// place the *flags* it leaves can be checked against the ROM's at the exact
// instruction the ROM leaves them.
//
// `in->y` is a genuine input: the raw collision id is still in Y from
// `$80:84A3  TYA`, and bit 15 of it is which player's tally this counts.
static void shim_enemy_freeze(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_freeze(w, in->d, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:83C6  enemy_bubble_react — the `$5D` twin's twin, reached the same way
// ---------------------------------------------------------------------------

// Registered on its own entry PC for `enemy_freeze`'s reason, and `in->y` is
// *not* an input here: this routine never reads Y, having no side to credit.
static void shim_enemy_bubble_react(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_bubble_react(w, in->d, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $80:9D39  rng_next — no arguments, and one of them is the caller's carry
// ---------------------------------------------------------------------------

static void shim_rng_next(Wram* w, const Rom* rom, const CosimRegs* in,
                          CosimRegs* out) {
  (void)rom;
  RngResult rng;
  rng_next(w, in->c, &rng);
  out->a = rng.a;
  // X and Y are never mentioned between the entry and the `RTL`.
  out->x = in->x;
  out->y = in->y;
  out->n = rng.n;
  out->z = rng.z;
  out->c = rng.c;
  out->v = rng.v;
  // The only shim in the registry that claims V, and the only one that needs to.
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C | COSIM_FLAG_V;
}

// ---------------------------------------------------------------------------
// $82:DEEB and $82:F1C2 — the two smallest handlers in the game
// ---------------------------------------------------------------------------

static void shim_actor_deeb_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_deeb_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// This routine contains no `CLC` and no `SEC`, which the first version of the
// port read as "carry passes through" — and `verify` failed it on call 2 with
// `flag C: ROM 1, port 0`. `CMP` sets carry; every exit here has run one. The
// seed stays because the shim's job is to hand the port what the ROM was called
// with, not because anything depends on it now.
static void shim_actor_f1c2_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_f1c2_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:F534  actor_f534_collide, and $83:A264  victim_a264_collide
// ---------------------------------------------------------------------------
//
// Neither declares a guard: between them they answer every id they are given
// and the only routine either calls (`$81:8191`) is inlined into the port.

static void shim_actor_f534_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_f534_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

static void shim_victim_a264_collide(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  victim_a264_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:CDDE  enemy_cdde_collide — the first handler with no guard
// ---------------------------------------------------------------------------

// No `supported` hook, and that is the entry worth noticing rather than an
// omission. Every other collision handler in the registry has ids it hands back
// — a jump-table entry nobody has written, a `JML` into the `$81:8506` family —
// and declares a guard to say so honestly. This one answers all three of its ids
// itself and the only routine it calls (`$81:CC0A`) is ported with it, so there
// is no condition under which it steps aside and nothing for a guard to report.
static void shim_enemy_cdde_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;  // no table lookup: this one's damage is a decrement
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_cdde_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:B592  enemy_b592_collide — no guard either, and even less to guard
// ---------------------------------------------------------------------------

static void shim_enemy_b592_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_b592_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $82:9660  boss_9660_collide — no guard either, and the first in bank $82
// ---------------------------------------------------------------------------

// `rom` is back, because unlike the two above this one does index
// `ENEMY_DAMAGE_TABLE`. `in->d` matters more here than anywhere else in the
// registry: this handler reads *both* its thread's page and two absolute
// globals, and getting the two confused is the one way to write it wrong.
static void shim_boss_9660_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  boss_9660_collide(w, rom, in->d, in->a, &r);
  handler_regs(&r, out);
}

// $82:AA2E  boss_aa2e_collide — the same shape again, and no guard.
static void shim_boss_aa2e_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  boss_aa2e_collide(w, rom, in->d, in->a, &r);
  handler_regs(&r, out);
}

// The last six handlers the dispatcher declined on. None declares a guard:
// every routine they reach is ported or inlined.
static void shim_actor_f330_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_f330_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

static void shim_actor_a638_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_a638_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

static void shim_actor_84ac_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_84ac_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

static void shim_enemy_b95f_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_b95f_collide(w, rom, in->d, in->a, &r);
  handler_regs(&r, out);
}

static void shim_enemy_eff0_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  enemy_eff0_collide(w, rom, in->d, in->a, &r);
  handler_regs(&r, out);
}

static void shim_actor_c8c3_collide(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  actor_c8c3_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $81:FE0E  shot_collide — the same argument again, on a weapon shot's page
// ---------------------------------------------------------------------------

// Carry is an input on exactly one of its three paths: id 0 reaches the `RTL`
// without executing a single `CMP`, so what leaves in carry is what arrived.
// The other two paths set it. No guard — the port has all of this routine, so
// there is no condition under which it could decline.

// What each exit of `$81:FE0E` costs, indexed by `ShotCollideBlock`.
//
// **The first table in this file with a third column.** A handler runs on the
// page `$80:84A2  TCD` installed, not on page zero, and half of the twenty-four
// pages in `$80:82DE` have a non-zero low byte — so the two `STA`s in the tail
// cost an idle more on half the actors in the game. Every routine priced before
// this one ran on page zero and could leave the column implicitly `0`.
//
// The four stop prefixes are cumulative walks down the `CMP` chain: 12 for the
// `TAY`, then 12 and 18 for each comparison that misses, and 18 for the branch
// that finally hits.
static const CosimRun SHOT_COST[SHOT_BLOCK_COUNT] = {
    [SHOT_BLK_STOP_A] = {12 + 18, 3},
    [SHOT_BLK_STOP_B] = {12 + 12 + 18 + 18, 8},
    [SHOT_BLK_STOP_C] = {12 + 12 + 18 + 12 + 18 + 18, 13},
    [SHOT_BLK_STOP_D] = {12 + 12 + 18 + 12 + 18 + 12 + 18 + 18, 18},
    // $81:FE21 LDY $0A : LDA #$0000 : STA $000E,Y : LDA #$0001 : STA $42 : RTL.
    // `LDY $0A` and `STA $42` are the two direct-page instructions; the store in
    // between goes through the record's absolute address and does not care.
    [SHOT_BLK_TAIL] = {174, 14, 2},
    // Falling through all four tests to `$81:FE20 RTL` — and 156 is exactly the
    // minimum `verify` measures for this routine.
    [SHOT_BLK_FLY] = {156, 19},
};

static int shot_cycles(const ShotCollideWork* k, bool fast, bool dp_unaligned) {
  int cycles = 0;
  for (int i = 0; i < SHOT_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles_dp(&SHOT_COST[i], fast,
                                                 dp_unaligned);
  return cycles;
}

static void shim_shot_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  (void)rom;  // no table, no ROM read
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y, .c = in->c};
  ShotCollideWork work;
  shot_collide_counted(w, in->d, in->a, &r, &work);
  handler_regs(&r, out);
  R36_COSIM_COST(shot_cycles(&work, in->fastrom, (in->d & 0xff) != 0));
}

// ---------------------------------------------------------------------------
// $82:9660  boss_9660_collide, priced
// ---------------------------------------------------------------------------
//
// This one has no shim of its own — the handler is not in the registry and is
// only ever reached through `$80:8480` — so unlike the four models above there
// is no `verify` minimum to check it against. What stands in for that witness is
// the same thing that found the last two tool bugs: the dispatcher's own error
// has to stay a non-negative multiple of 40 on the 9,802 calls of `level25-2p`
// that arrive here, and a table that is wrong anywhere makes it negative
// somewhere.
//
// It is also the first handler priced whose costs are not all direct-page.
// `LDY $0078`, `LDX $000E,Y` and `LDA $0020` are absolute reads of low WRAM
// through the data bank; `$3A` to `$44` are on the thread's own page. The two
// columns are what tells them apart, and `$82:948F` writing absolute `$003C` as
// a *coordinate* in the routine that seeds direct `$3C` to 70 is what makes
// getting it wrong plausible.
static const CosimRun BOSS_COST[BOSS_BLOCK_COUNT] = {
    // $82:9660..$9668, the `BEQ` taking it to `$9674  STZ $42 : CLC : RTL`.
    [BOSS_BLK_INVULN] = {92 + 18 + 82, 15, 1},
    // ...falling through, then `LDX $40` and its `BNE` to the same three.
    [BOSS_BLK_FLASHING] = {92 + 12 + 28 + 18 + 82, 19, 2},
    // ...and the `CMP #$005C` under it not carrying, which lands there again.
    [BOSS_BLK_IGNORE] = {92 + 12 + 28 + 12 + 18 + 12 + 82, 24, 2},
    // ...or carrying, so `$9678  STA $42 : AND #$7FFF` and into the chain.
    [BOSS_BLK_HIT] = {92 + 12 + 28 + 12 + 18 + 18 + 46, 25, 2},
    // $967D `CMP #$0062` matching, the tick read, and one of the two answers.
    [BOSS_BLK_REMAP_62_CHEAP] = {18 + 12 + 52 + 12 + 18 + 18, 18},
    [BOSS_BLK_REMAP_62_DEAR] = {18 + 12 + 52 + 18 + 18 + 18, 18},
    // ...or not, and `$968F  CMP #$0070` matching one comparison later.
    [BOSS_BLK_REMAP_70_CHEAP] = {18 + 18 + 18 + 12 + 52 + 12 + 18 + 18, 23},
    [BOSS_BLK_REMAP_70_DEAR] = {18 + 18 + 18 + 12 + 52 + 18 + 18 + 18, 23},
    // Two comparisons in, and no tick: these two are not a coin toss.
    [BOSS_BLK_REMAP_61] = {18 + 18 + 18 + 18 + 18 + 12 + 18 + 18, 20},
    [BOSS_BLK_REMAP_6F] = {18 + 18 + 18 + 18 + 18 + 18 + 18 + 12 + 18 + 18, 25},
    // All four `CMP`s and all four branches taken, and nothing rewritten.
    [BOSS_BLK_REMAP_NONE] = {18 + 18 + 18 + 18 + 18 + 18 + 18 + 18, 20},
    // $96BA through the `SBC $818561,X` — whose two-byte read of a ROM table is
    // why the byte column is 19 for seventeen bytes of instruction — then the
    // `BMI` taking it to `$96D5  DEC $3A` and the shared `SEC : RTL`.
    [BOSS_BLK_DIED] = {230 + 18 + 50 + 54, 25, 4},
    // ...not taken, and `CMP $3C : BEQ` finding the subtraction took nothing.
    [BOSS_BLK_NO_DAMAGE] = {230 + 12 + 28 + 18 + 54, 27, 4},
    // ...or finding it took something, so `$96D1  STA $3C` first.
    [BOSS_BLK_SURVIVED] = {230 + 12 + 28 + 12 + 28 + 18 + 54, 31, 5},
};

// No guard. Every exit of `$82:9660` is inside `$82:9660` — there is no `JML`
// out of it and nothing under it that is not ported — so unlike `enemy_cycles`
// this one cannot fail, and the dispatcher's `switch` treats it like
// `shot_cycles` rather than like the two that can decline.
static int boss_cycles(const BossCollideWork* k, bool fast, bool dp_unaligned) {
  int cycles = 0;
  for (int i = 0; i < BOSS_BLOCK_COUNT; i++)
    cycles +=
        k->blocks[i] * cosim_run_cycles_dp(&BOSS_COST[i], fast, dp_unaligned);
  return cycles;
}

// ---------------------------------------------------------------------------
// $80:8480  thread_call_handler, priced
// ---------------------------------------------------------------------------

// The frame the dispatcher builds around a handler. Four blocks and no table:
// its control flow is two branches, and everything it touches is either the
// stack, low WRAM through the data bank, or the page table in bank $80 ROM.
//
// None of its own instructions is direct-page — it is running on the *caller's*
// page and deliberately touches nothing on it — so the third column is 0 here
// and non-zero only in the handler tables it sums.
static const CosimRun THREAD_CALL_NONE = {40 + 40 + 18 + 42, 9};
// $80:8480..$80:84A4, the `BEQ` falling through and the `RTL` that enters the
// handler. The two `PHA`s at `$80:848F` and `$80:8496` run under `SEP #$20` and
// push one byte each, and `PEA` pushes its operand rather than reading it —
// between them that is the 6 cycles this constant was first written 510 for.
static const CosimRun THREAD_CALL_ENTER = {504, 39};
// $80:84A5 PLX : BCC taken : PLD : PLB : RTL.
static const CosimRun THREAD_CALL_RESUME = {34 + 18 + 34 + 26 + 42, 6};
// ...and not taken, so `LDA #$8000 : STA $1180,X` parks the thread first.
static const CosimRun THREAD_CALL_PARK = {34 + 12 + 18 + 40 + 34 + 26 + 42, 12};

static bool thread_call_cycles(const ThreadCallWork* k, const CosimRegs* in,
                               int* out) {
  bool fast = in->fastrom;
  int cycles = k->blocks[THREAD_CALL_BLK_NONE] *
                   cosim_run_cycles(&THREAD_CALL_NONE, fast) +
               k->blocks[THREAD_CALL_BLK_ENTER] *
                   cosim_run_cycles(&THREAD_CALL_ENTER, fast) +
               k->blocks[THREAD_CALL_BLK_RESUME] *
                   cosim_run_cycles(&THREAD_CALL_RESUME, fast) +
               k->blocks[THREAD_CALL_BLK_PARK] *
                   cosim_run_cycles(&THREAD_CALL_PARK, fast);

  // No handler ran, so the frame is the whole routine — and this is the path
  // that costs 140, which is what `verify` measures as the minimum.
  if (k->entry == 0) {
    *out = cycles;
    return true;
  }

  // `$80:84A2  TCD` installed the thread's own page a few instructions ago, and
  // whether its low byte is zero is what the handler's direct-page instructions
  // cost an extra idle on.
  bool dp_unaligned = (k->dp & 0xff) != 0;
  switch (k->entry) {
    case SHOT_COLLIDE_ENTRY:
      cycles += shot_cycles(&k->shot, fast, dp_unaligned);
      break;
    case BOSS_9660_COLLIDE_ENTRY:
      cycles += boss_cycles(&k->boss, fast, dp_unaligned);
      break;
    case PLAYER_COLLIDE_ENTRY: {
      int handler;
      if (!player_cycles(&k->player, fast, dp_unaligned, &handler)) return false;
      cycles += handler;
      break;
    }
    case ENEMY_COLLIDE_ENTRY: {
      int handler;
      if (!enemy_cycles(&k->enemy, fast, dp_unaligned, &handler)) return false;
      cycles += handler;
      break;
    }
    // The two copies share a table because they share a body. `$81:C440` is
    // 96% of what `sprite_build_oam` used to decline on and `$81:C4A6` was on
    // the same work list; pricing one without the other would have meant
    // writing the same six constants twice.
    case MONSTER_COLLIDE_ENTRY:
    case MONSTER_C440_COLLIDE_ENTRY: {
      int handler;
      if (!monster_cycles(&k->monster, fast, dp_unaligned, &handler))
        return false;
      cycles += handler;
      break;
    }
    default:
      // A handler with no table. Not an error and not a gap in the port — the
      // port has twenty-five of these and this file prices four of them.
      //
      // Which addresses land here is the work list for the next round, and the
      // way to read it is a run with a `printf` on this line: `$82:9660` was
      // first by a distance and is now above, so what is left is `$81:C4A6`,
      // `$81:C440`, `$80:CAEE` and `$81:8888`'s three deep exits. It is not
      // wired into `cosim_census_note` — this function is called again for
      // every level of the four models stacked on top of it, so a census here
      // would count one dispatch up to four times and read like a frequency
      // when it is not one.
      return false;
  }
  *out = cycles;
  return true;
}

// ---------------------------------------------------------------------------
// $83:A364  victim_collide — the same argument again, on a victim's page
// ---------------------------------------------------------------------------

// No guard, for the same reason `shot_collide` has none: every exit is ported,
// so there is no condition it could decline on. Carry is an output on all nine
// paths and an input on none — eight `SEC`s and a `CLC`, and the entry guard's
// `BNE` reaches one of them without reading it.
static void shim_victim_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)rom;  // no table, no ROM read
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y};
  victim_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $80:CAEE  object_collide — the same argument, on the object manager's page
// ---------------------------------------------------------------------------

// No guard, for the same reason `shot_collide` and `victim_collide` have none.
// Carry is an output on all three paths and an input on none: two `CLC`s and a
// `SEC`, and nothing reads it on the way to any of them.
static void shim_object_collide(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)rom;  // no table, no ROM read
  ActorHandlerRegs r = {.a = in->a, .x = in->x, .y = in->y};
  object_collide(w, in->d, in->a, &r);
  handler_regs(&r, out);
}

// ---------------------------------------------------------------------------
// $80:EA63  weapon_select_next — no arguments but the player's direct page
// ---------------------------------------------------------------------------

// Registered in its own right for the reason `score_add` is: `player_pickup`
// reaches it, and so does `$80:D267`, the player's input handler noticing that
// B was pressed. Those two call sites have nothing to do with each other, and
// the second one is by far the more common — a pickup is rare and pressing B
// is not.
//
// It is reached two different ways, too. `$80:D267` is a `JSR`; `$80:F8A8` is a
// `JMP`, so on that path the return address on the stack is `player_collide`'s
// and the frame is still `COSIM_RTS`-shaped. The engine reads the frame rather
// than assuming, so both work.
static void shim_weapon_select_next(Wram* w, const Rom* rom, const CosimRegs* in,
                                    CosimRegs* out) {
  WeaponSelectRegs r;
  weapon_select_next(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:EAA8  item_select_next — the same, one array over
// ---------------------------------------------------------------------------

// Registered separately from `weapon_select_next` even though the two are the
// same twenty-two instructions, because they are two routines in the ROM at two
// addresses with four callers between them and no way to tell from a call site
// which one is meant. `$80:F903` is the `JMP` under `item_pickup`; `$80:D278` is
// the input handler noticing **A**, which is to items what B is to weapons.
static void shim_item_select_next(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  WeaponSelectRegs r;
  item_select_next(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:C7D9  score_add — X = the BCD award, A's sign = the side
// ---------------------------------------------------------------------------

// Registered in its own right for the reason `player_collide` is: the collision
// path reaches it, but so does the victim-rescue thread at `$83:A1EC`, and those
// two call sites have nothing to do with each other. Intercepted here it is
// checked on both — and on the second one even in `run` mode, where
// `enemy_collide` is substituted whole and the ROM never reaches the first.
//
// The `BMI` at the entry means this is the second routine whose *input* includes
// a flag. `player_collide` needed `d`; this one needs `n`, which `CosimRegs`
// already carries because the diff compares it on the way out.
static bool guard_score_add(Wram* scratch, const Rom* rom, const CosimRegs* in) {
  return score_add_supported(scratch, rom, in->n);
}

static void shim_score_add(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  ScoreResult r;
  score_add(w, rom, in->n, in->x, in->c, &r);  // the guard allowed it
  out->a = r.a;
  out->x = r.x;
  // Y is never mentioned between the entry and any of the three `RTL`s.
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:BE8F  actor_collide_notify — X = the pair's second record, and it may
//                                  decline
// ---------------------------------------------------------------------------

// Registered in its own right even though `actor_overlap_pass` already calls
// it, and that is the point: the pass declines every call containing a
// collision the port cannot dispatch, so without this entry the plumbing would
// only ever be checked on the passes where nothing happened. Intercepted here,
// it is checked on **every** collision the movie produces, including the ones
// that go on to enter a handler and end the enclosing pass.
//
// The two arguments are where the ROM left them at `$80:BF0E  JSR $BE8F`: the
// inner record in X, and the outer one reachable through the walk cursor the
// pass parked in `$3C`. `LDX $1380,Y` is `visible_actors + 2 + $3C`, two past
// the base because the cursor has already been stepped back.
static uint16_t notify_outer(const Wram* w) {
  return wram_r16(w, W_VISIBLE_ACTORS + 2 + wram_r16(w, W_OVERLAP_CURSOR));
}

static int fast_guard_actor_collide_notify(const Wram* live,
                                          const CosimRegs* in) {
  if (!(in->db < 0x40 || (in->db >= 0x80 && in->db < 0xc0)))
    return R52_DEFER_TO_SANDBOX;
#if defined(ZAMN_R74_COLLISION_OAM_INPUT_PROOF)
  return (int)r74_notify_decision(live, notify_outer(live), in->x);
#elif defined(ZAMN_R58_COLLISION_THREAD_FASTPATH)
  return (int)r58_notify_decision(live, notify_outer(live), in->x);
#else
  return (int)r52_notify_decision(live, notify_outer(live), in->x);
#endif
}

static bool guard_actor_collide_notify(Wram* scratch, const Rom* rom,
                                       const CosimRegs* in) {
  ThreadCallResult tail = {.c = in->c};
  return actor_collide_notify(scratch, rom, notify_outer(scratch), in->x, &tail);
}

// $80:BE8F..$80:BEC8 in full, both `JSL`s included and neither dispatch. The
// routine has no branches — twenty-six instructions of straight line — so this
// is one constant rather than a table, and the only thing that varies call to
// call is what the two dispatches cost.
//
// 856 + 2 x 140, the two slots having no handler registered, is 1,136, which is
// exactly the minimum `verify` measures for this routine.
static const CosimRun NOTIFY_COST = {856, 58, 23};

static bool notify_cycles(const CollideNotifyWork* k, const CosimRegs* in,
                          int* out) {
  int cycles =
      cosim_run_cycles_dp(&NOTIFY_COST, in->fastrom, (in->d & 0xff) != 0);
  for (int i = 0; i < 2; i++) {
    int call;
    if (!thread_call_cycles(&k->call[i], in, &call)) return false;
    cycles += call;
  }
  *out = cycles;
  return true;
}

static void shim_actor_collide_notify(Wram* w, const Rom* rom,
                                      const CosimRegs* in, CosimRegs* out) {
  uint16_t a = notify_outer(w);
  ThreadCallResult tail = {.c = in->c};
  CollideNotifyWork work;
  // the guard allowed it
  actor_collide_notify_counted(w, rom, a, in->x, &tail, &work);

  // The routine's last instruction is the second `JSL $80:8480`, so everything
  // it returns is really the dispatcher's — including X and Y, which are the
  // arguments `$80:BEC0`/`$80:BEC2` set up and which the dispatcher hands back
  // untouched. So this defers to `handler_exit` rather than restating it, which
  // keeps one description of that tail rather than two.
  handler_exit(in, &tail, out);

  if (R36_COSTS_NEEDED()) {
    int cycles;
    if ((in->db < 0x40 || (in->db >= 0x80 && in->db < 0xc0)) &&
        notify_cycles(&work, in, &cycles))
      R36_COSIM_COST(cycles);
  }
}

// ---------------------------------------------------------------------------
// $80:BEC9  actor_overlap_pass — no arguments, and it may decline
// ---------------------------------------------------------------------------

// The first routine whose port covers only part of what the ROM's version does,
// so it is the first to need a guard — see `CosimGuard` in `cosim.h`.
//
// The guard is the whole routine, run on a throwaway copy of WRAM. That is not
// a shortcut: "can the port handle this call?" and "what does the port do with
// this call?" are the same question here, because the condition it declines on
// is one only the walk can find. Asking it any other way would mean writing the
// pairwise test a second time in the harness, where it could drift.
//
// The condition has narrowed twice since it was written. A hit no longer ends
// the pass by itself: `actor_collide_notify` serves the dispatch, and
// `thread_call_handler` serves the two handlers a collision in ordinary play
// reaches. What is left to decline is a collision that enters a *third*
// handler, or one of the two on a branch that leaves through unported code.
static bool guard_actor_overlap_pass(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  (void)in;
  return actor_overlap_pass(scratch, rom);
}

// What each run of `$80:BEC9` costs, indexed by `ActorOverlapBlock`. Same rules
// as the two tables above; `visible_actors`, the records and the four scratch
// words are all under `$7E:2000` and cost 8 a byte through every bank.
//
// The empty-list case is again the check: `LDY $9C : BEQ` taken plus `RTL` is
// 28 + 18 + 42 = 88, which is exactly the minimum `verify` measures.
//
// There is no entry for the hit. `$80:BF0D PHY : JSR $BE8F : PLY` would be 102
// cycles of its own, but the `JSR` enters a dispatch into two actor handlers and
// what *they* cost is a tree this table does not describe, so a pass with a hit
// in it is not priced at all rather than priced short. See `ActorOverlapWork`.
static const CosimRun OVL_COST[OVL_BLOCK_COUNT] = {
    [OVL_BLK_EMPTY] = {28 + 18 + 42, 5},
    [OVL_BLK_SINGLE] = {28 + 12 + 24 + 18 + 42, 9},
    [OVL_BLK_PROLOGUE] = {28 + 12 + 24 + 12, 8},
    // $80:BED1 LDX $137E,Y : DEY : DEY : STY $3C : LDA $0E,X — 126 over 9 bytes
    // — and the `BEQ` that decides whether the record has an id at all.
    [OVL_BLK_OUTER_NOID] = {126 + 18, 11},
    // ...and the four instructions that publish it: 126 + 12 + 152.
    [OVL_BLK_OUTER_ID] = {126 + 12 + 152, 21},
    // $80:BEE6 LDX $137E,Y : LDA $0E,X — 74 over 5 bytes — plus its `BEQ`.
    [OVL_BLK_INNER_NOID] = {74 + 18, 7},
    [OVL_BLK_INNER_ID] = {74 + 12, 7},
    [OVL_BLK_SAME_ID] = {28 + 18, 4},
    [OVL_BLK_DIFF_ID] = {28 + 12, 4},
    // LDA $02,X : SEC : SBC $38 : CLC : ADC #$0008 : CMP #$0010 — 122 over 12
    // bytes — plus the `BCS`. The Y axis is the same six off `$06` and `$3A`.
    [OVL_BLK_X_FAR] = {122 + 18, 14},
    [OVL_BLK_X_NEAR] = {122 + 12, 14},
    [OVL_BLK_Y_FAR] = {122 + 18, 14},
    [OVL_BLK_Y_NEAR] = {122 + 12, 14},
    [OVL_BLK_INNER_NEXT] = {24 + 18, 4},
    [OVL_BLK_INNER_DONE] = {24 + 12, 4},
    [OVL_BLK_OUTER_NEXT] = {28 + 18, 4},
    [OVL_BLK_OUTER_DONE] = {28 + 12 + 42, 5},
};

// `$80:BF0D  PHY : JSR $BE8F : PLY`, the three instructions a hit costs on top
// of the two axis tests — not counting the dispatch itself, which is
// `notify_cycles`.
static const CosimRun OVL_HIT = {28 + 40 + 34, 5};

// False when a hit in this pass entered a handler with no cost table, or when
// there were more hits than `ActorOverlapWork` can describe. The blocks are
// summed either way: what is missing is never a *part* of the answer, it is the
// answer, so there is no version of this that reports a number it half knows.
static bool overlap_cycles(const ActorOverlapWork* k, const CosimRegs* in,
                           int* out) {
  bool fast = in->fastrom;
  int cycles = 0;
  for (int i = 0; i < OVL_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&OVL_COST[i], fast);

  if (k->hits > OVL_MAX_PRICED_HITS) return false;
  for (int i = 0; i < k->hits; i++) {
    int notify;
    if (!notify_cycles(&k->notify[i], in, &notify)) return false;
    cycles += cosim_run_cycles(&OVL_HIT, fast) + notify;
  }
  *out = cycles;
  return true;
}

static void shim_actor_overlap_pass(Wram* w, const Rom* rom, const CosimRegs* in,
                                    CosimRegs* out) {
  // The guard already established it will not decline.
  ActorOverlapWork work;
  actor_overlap_pass_counted(w, rom, &work);

  // Priced only off a page-aligned direct page — `$9C`, `$3C`, `$4A`, `$38`,
  // `$3A` and the four `$xx,X` reads would each cost one more internal cycle
  // otherwise. A hit no longer disqualifies the pass by itself; what does is a
  // hit whose handler has no table, and `overlap_cycles` is what knows that.
  if (R36_COSTS_NEEDED()) {
    int cycles;
    if ((in->d & 0xff) == 0 && overlap_cycles(&work, in, &cycles))
      R36_COSIM_COST(cycles);
  }

  // All three `RTL` paths arrive with Y zero and the flags of whatever loaded
  // it: `LDY $9C` on an empty list, `DEY DEY` on a single record, and `LDY $3C`
  // at the end of the walk, which is what the loop exits on.
  out->y = 0;
  out->n = false;
  out->z = true;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z;
  out->regs = COSIM_REG_Y;

  // A, X and carry are not claimed, for the same reason `actor_depth_sort`
  // does not claim them: each is an intermediate of whichever comparison the
  // walk happened to stop on — the id that read zero, or one of the four box
  // tests — and predicting it here would mean writing the walk twice. They are
  // dead. The only caller is `$80:BDCC`, whose next instructions are `PLD :
  // PLB : LDA $20 ... TAX` and then a `SEC`, so all three are overwritten
  // before anything reads them.
}

// ---------------------------------------------------------------------------
// $80:BD1F  sprite_build_oam — one argument, the caller's page, and it may decline
// ---------------------------------------------------------------------------

// The one address the pass legitimately leaves alone, and it is the same $38 two
// of the routines it calls already declare. `actor_depth_sort` keeps its walk
// predecessor there and `sprite_frame_tile` spills the emitter's X there; the
// port keeps both in C locals.
//
// It does reproduce the second of the two, because that one is derivable: the
// last frame lookup of a pass spills the OAM index of the last piece drawn,
// which is four bytes back from where the buffer ends. That is worth doing even
// though the exclude means it is not checked here — dropping it moves the first
// divergence from call 192 to call 130 with the exclude off, so it is right far
// more often than not. What is left is the depth sort's spill on passes that
// draw nothing, which would need that routine to model its own walk in WRAM.
//
// The exclude costs less coverage than it looks: `actor_overlap_pass` writes $38
// too and runs after everything else here, so whenever it has a record to test,
// this address is checked exactly — by that routine, registered separately and
// compared on all 1,016 of its own calls.
static const CosimExclude BUILD_OAM_EXCLUDES[] = {
    {0x0038, 2, "scratch: actor_depth_sort's walk predecessor, then the emitter's X"},
};

#if defined(XBOX_PORT) && defined(ZAMN_R56_SAFE_OAM_CUTOVER)
static int r56_fast_guard_oam(const Wram* live, const Rom* rom,
                              const CosimRegs* in) {
  (void)in;
#if defined(ZAMN_R57_OAM_COLLISION_PROOF)
  return sprite_build_oam_r57_fast_decision(live, rom);
#else
  return sprite_build_oam_readonly_safe(live, rom) ? 1 : 0;
#endif
}
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R53_NATIVE_MOVEMENT)
// R53: An empty actor linked list always culls to a zero visible count; there
// are then no metasprites or collision handlers for the preflight to reject.
// All nonempty cases defer to the original R40 64KiB scratch guard.
static int r53_fast_guard_empty_oam(const Wram* live, const CosimRegs* in) {
  (void)in;
  return wram_r16(live, W_ACTOR_LIST_HEAD) == 0 ? 1 : 0;
}
#endif

static bool guard_sprite_build_oam(Wram* scratch, const Rom* rom,
                                   const CosimRegs* in) {
#if defined(XBOX_PORT) && defined(ZAMN_R40_FAST_OAM_PREFLIGHT)
  return sprite_build_oam_supported_preflight(scratch, rom, in->d);
#else
  return sprite_build_oam(scratch, rom, in->d);
#endif
}

// ---------------------------------------------------------------------------
// ...and what a pass costs, which is the sum of six of these tables.
//
// `$80:BD1F` is the outermost substituted routine on the sprite path, so it is
// the only one whose model is ever consulted: the sort, the cull, the buffer
// clear, the overlap pass and the frame cache are all reached from inside it
// and every one of them has exactly one caller. Their models have been right
// and unreachable for two rounds. This is where they start being used.
//
// It also settles the guards. Standalone, four of those five have to check the
// caller's direct page before they can price anything, and the cache has to
// check the data bank as well. Reached from here they need neither: `$80:BD21
// PEA $0000 : PLD` and `$80:BD25 PHK : PLB` establish page zero and bank $80
// for the whole pass. Only two instructions run outside that window — the
// `LDA $20` and `LDA $BDE6,X` after `$80:BDD0 PLD : PLB` — and they are the
// only reason anything below asks about the caller at all.
// ---------------------------------------------------------------------------

static CosimRun run_plus(CosimRun a, CosimRun b) {
  CosimRun r = {a.cycles + b.cycles, a.bytes + b.bytes};
  return r;
}

// One emitter, in `SpriteEmitBlock` order, priced as `$80:BA51` — the unflipped
// one. The other three are this plus the deltas below; see `SpriteEmitWork` for
// why that is a fair description of them rather than a convenience.
static const CosimRun EMIT_COST[EMIT_BLOCK_COUNT] = {
    [EMIT_BLK_PROLOGUE]   = {28, 2},
    // $80:BA53..$80:BA60 is 164 over 16, and every path through the piece pays
    // it: `LDY #$0002 : LDA [$8A],Y : CLC : ADC $90 : STA $13BF,X : CMP #$FFF1`.
    // Two of those 16 bytes are the metasprite word itself, read through a long
    // pointer into bank $8F or $90 — fast ROM, and so FastROM-sensitive.
    [EMIT_BLK_Y_HIGH]     = {164 + 18, 18},
    [EMIT_BLK_Y_LOW]      = {164 + 12 + 18 + 12, 23},
    [EMIT_BLK_Y_DROP]     = {164 + 12 + 18 + 18, 23},
    // $80:BA68..$80:BA76, 174 over 17, then the same two-test shape on x.
    [EMIT_BLK_X_NEAR]     = {174 + 18, 19},
    [EMIT_BLK_X_DROP]     = {174 + 12 + 18 + 18, 24},
    // ...and $80:BA7E..$80:BA89, the four instructions that set the sprite's
    // ninth x bit in the high table. Two ROM table reads, hence 16 bytes.
    [EMIT_BLK_X_WRAP]     = {174 + 12 + 18 + 12 + 152, 40},
    // $80:BA8A..$80:BAA8, 390 over 35: the attribute word, the frame number,
    // the `JSR $B9D6` itself — 40, with the lookup's own cost coming from
    // `TILE_COST` — and the four `INX`.
    [EMIT_BLK_PIECE]      = {390 + 12, 37},
    [EMIT_BLK_PIECE_FULL] = {390 + 18, 37},
    // $80:BAAB..$80:BAB4, the pointer advance and `DEC $86`.
    [EMIT_BLK_NEXT]       = {136 + 18, 12},
    [EMIT_BLK_DONE]       = {136 + 12, 12},
    [EMIT_BLK_EXIT]       = {68, 3},
};

// `EOR #$FFFF : SEC : SBC #$000F`, the mirror. In front of the y offset for a
// vertical flip, in front of the x offset for a horizontal one.
static const CosimRun EMIT_MIRROR = {48, 7};
// `EOR #$4000` / `#$8000` / `#$C000` on the finished OAM word — one instruction
// with three operands, present in all three flipped emitters and in none of the
// unflipped one.
static const CosimRun EMIT_FLIP_EOR = {18, 3};
// The loop-back. Three extra bytes of body per piece put `$80:BA53` out of a
// relative branch's reach, so the flipped emitters spend `BEQ : JMP` where
// `$80:BAB5` spends a `BNE` — dearer to go round, dearer to stop.
static const CosimRun EMIT_FAR_NEXT = {12, 3};
static const CosimRun EMIT_FAR_DONE = {6, 0};

static int emit_cycles(const SpriteBuildWork* k, bool fast) {
  int cycles = 0;
  for (int flip = 0; flip < 8; flip += 2) {
    const bool fx = (flip & SPRITE_FLIP_X) != 0;
    const bool fy = (flip & SPRITE_FLIP_Y) != 0;
    for (int i = 0; i < EMIT_BLOCK_COUNT; i++) {
      if (k->emit[flip][i] == 0) continue;
      CosimRun r = EMIT_COST[i];
      if (fy && (i == EMIT_BLK_Y_HIGH || i == EMIT_BLK_Y_LOW ||
                 i == EMIT_BLK_Y_DROP))
        r = run_plus(r, EMIT_MIRROR);
      if (fx && (i == EMIT_BLK_X_NEAR || i == EMIT_BLK_X_DROP ||
                 i == EMIT_BLK_X_WRAP))
        r = run_plus(r, EMIT_MIRROR);
      if (flip != SPRITE_FLIP_NONE) {
        if (i == EMIT_BLK_PIECE || i == EMIT_BLK_PIECE_FULL)
          r = run_plus(r, EMIT_FLIP_EOR);
        if (i == EMIT_BLK_NEXT) r = run_plus(r, EMIT_FAR_NEXT);
        if (i == EMIT_BLK_DONE) r = run_plus(r, EMIT_FAR_DONE);
      }
      cycles += k->emit[flip][i] * cosim_run_cycles(&r, fast);
    }
  }
  return cycles;
}

// The walk itself, `$80:BD1F`..`$80:BDE2`, in `SpriteBuildBlock` order. The
// three `JSR`s and the one `JSL` are in here at 40 and 54; what they call is
// not.
static const CosimRun BUILD_COST[BUILD_BLOCK_COUNT] = {
    // $80:BD1F..$80:BD36 is 378 over 25 with the `BEQ` falling through, so an
    // empty pass is that with the branch taken plus `$80:BD77 BRA $BDCC`.
    [BUILD_BLK_EMPTY]      = {378 + 6 + 18, 27},
    [BUILD_BLK_NONEMPTY]   = {378 + 46, 30},
    // $80:BD3D..$80:BD43 `STY $9A : LDX $137E,Y : LDA $00,X`, then the `BPL`.
    [BUILD_BLK_UNDRAWN]    = {114 + 6, 9},
    [BUILD_BLK_DRAWN]      = {114, 9},
    // Each of the next three pairs ends on the store the two paths share, so
    // one of each pair is counted per drawable record and nothing is left over.
    [BUILD_BLK_PRIO_PLAIN] = {18 + 18 + 18 + 28, 10},
    [BUILD_BLK_PRIO_TOP]   = {18 + 18 + 12 + 18 + 28, 13},
    [BUILD_BLK_ATTR_PLAIN] = {18 + 34 + 18 + 18 + 28, 12},
    [BUILD_BLK_ATTR_SET]   = {190 + 28, 21},
    [BUILD_BLK_SCREEN]     = {34 + 12 + 12 + 124 + 18, 15},
    [BUILD_BLK_WORLD]      = {34 + 12 + 18 + 262, 24},
    // $80:BD8C onwards: three ways to be rejected, at 7, 16 and 21 bytes, and
    // then the count byte. The prefix each one shares is folded in, so exactly
    // one of the five below is counted per drawable record.
    [BUILD_BLK_NO_META]    = {34 + 18 + 18, 7},
    [BUILD_BLK_BANK_LOW]   = {64 + 28 + 34 + 18 + 18, 16},
    [BUILD_BLK_BANK_HIGH]  = {64 + 92 + 18 + 18, 21},
    [BUILD_BLK_EMPTY_META] = {186 + 140, 34},
    // ...and the one that draws: `INC $8A : LDA $00,X : AND #$0006 : TAX` and
    // the `JSR ($BDEA,X)` at 52, whose two vector bytes come out of bank $80
    // and so belong in the byte column with the program.
    [BUILD_BLK_DRAW]       = {186 + 300, 47},
    [BUILD_BLK_FULL]       = {18 + 18, 5},
    [BUILD_BLK_NOT_FULL]   = {18 + 12, 5},
    [BUILD_BLK_NEXT]       = {28 + 12 + 12 + 28 + 18 + 18, 11},
    // ...or the last record, which falls through to `$80:BDC4`'s terminator.
    [BUILD_BLK_LAST]       = {92 + 86, 16},
    // $80:BDCC..$80:BDE2, with the `JSL` at 54 and the `RTL` at 42. Priced with
    // the four-byte table read fast; `build_cycles` corrects it when the data
    // bank the `PLB` restored cannot reach bank $80 in six.
    [BUILD_BLK_EPILOGUE]   = {314, 25},
};

// The two program bytes of `$80:BDD8 LDA $BDE6,X` cost 8 apiece rather than 6
// when the caller's data bank is a low one, and stop being FastROM-sensitive
// when they do. It is the only instruction in the pass that asks.
static const CosimRun BUILD_EPILOGUE_SLOW_TABLE = {4, -2};

// False for the same reason `overlap_cycles` is: a collision inside this pass
// entered a handler with no table. Everything else about the pass is priced
// whether or not that happens.
static bool build_cycles(const SpriteBuildWork* k, const CosimRegs* in,
                         int* out) {
  const bool fast = in->fastrom;
  int cycles = 0;
  for (int i = 0; i < BUILD_BLOCK_COUNT; i++) {
    CosimRun r = BUILD_COST[i];
    if (i == BUILD_BLK_EPILOGUE && in->db < 0x80)
      r = run_plus(r, BUILD_EPILOGUE_SLOW_TABLE);
    cycles += k->blocks[i] * cosim_run_cycles(&r, fast);
  }
  // ...and everything the walk called. `oam_buffer_clear` runs once per pass
  // and is the only one of the five that costs the same every time.
  cycles += depth_sort_cycles(&k->sort, fast);
  cycles += cull_cycles(&k->cull, fast);
  cycles += cosim_run_cycles(&OAM_CLEAR_COST, fast);
  int overlap;
  if (!overlap_cycles(&k->overlap, in, &overlap)) return false;
  cycles += overlap;
  cycles += emit_cycles(k, fast);
  cycles += tile_cycles(&k->tile, fast);
  *out = cycles;
  return true;
}

static void shim_sprite_build_oam(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  // The guard already established it will not decline.
  SpriteBuildWork work;
  sprite_build_oam_counted(w, rom, in->d, &work);

  // `$80:BDD2 LDA $20` runs after `PLD`, on the caller's page, so an unaligned
  // one costs an internal cycle the table above does not carry. `$80:BDDE STA
  // $1B64` runs after `PLB` and reaches low WRAM in 8 through every bank a
  // caller could plausibly leave behind — but not through $40..$7F or $C0+,
  // where it would not be WRAM at all, so those decline rather than guess.
  // ...and the pass still inherits `actor_overlap_pass`'s refusal, but it is a
  // narrower one than it was. A pair that actually touched sends `$80:BF0E JSR
  // $BE8F` into the collision handler tree; four of those handlers now have
  // cost tables, so a pass whose collisions all landed on one of the four is
  // priced like any other, and only the rest report nothing. That is why
  // `build_cycles` returns a bool now instead of taking `hits == 0` as a
  // precondition.
  if (R36_COSTS_NEEDED()) {
    int cycles;
    if ((in->d & 0xff) == 0 &&
        (in->db < 0x40 || (in->db >= 0x80 && in->db < 0xc0)) &&
        build_cycles(&work, in, &cycles))
      R36_COSIM_COST(cycles);
  }

  // The tail at `$80:BDD2` is what decides all of this, and it runs on every
  // path: `LDA $20 : AND #$0003 : TAX : LDA $BDE6,X : AND #$00FF : STA $1B64 :
  // SEC : RTL`.
  //
  //   * A is the table entry after the mask — the value just stored.
  //   * X is the low two bits of `$20` on the **caller's** page, from the `TAX`.
  //     `$80:BDD0  PLD` has already restored it, so this is `in->d + $20` and
  //     not `W_SCHED_TICK`; two of the three callers are threads. Because all
  //     four table entries are $80, the difference is invisible in WRAM and
  //     shows up here and nowhere else — `movies/level49-bubble.zmv` is the
  //     first input to reach one of those callers, and it failed on this
  //     register with 128 KB matching.
  //   * `AND #$00FF` is the last flag-setting instruction, so N and Z describe
  //     that same value. It is $80 for all four entries, so Z is false and N is
  //     false too: $0080 is positive in 16 bits.
  //   * Carry is the `SEC`, and it is the one output here a caller could
  //     plausibly read.
  //
  // Y is never mentioned between `$80:BDD0` and the `RTL`, but it is not the
  // caller's either — the pass ran a great deal of code that used it. It is
  // whatever `actor_overlap_pass` left, and that is 0 on all three of its exits,
  // including the one taken when nothing is visible at all.
  out->a = wram_r16(w, W_SPRITE_PASS_PHASE);
  out->x = (uint16_t)(wram_r16(w, (uint32_t)((in->d + SPRITE_PASS_PHASE_DP) &
                                            0xffff)) &
                     3);
  out->y = 0;
  out->n = (out->a & 0x8000) != 0;
  out->z = out->a == 0;
  out->c = true;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B123  actor_nearest — X, Y = the point; X = winner, A = its distance
// ---------------------------------------------------------------------------
//
// `$80:B189  LDX $44 : LDA $38 : PLD : RTL`.
//
// **N and Z do not come from the distance, they come from the `PLD`** — a pull
// sets them from the value pulled, so what the caller sees is the sign and
// zeroness of *its own direct page*, restored one instruction before the `RTL`.
// The first version of this shim read them off `LDA $38` and passed 1,006 of
// 1,006 calls on `movies/level1.zmv`, because every search on that movie found
// something and a distance under `$8000` has the same sign bit as a thread page
// does. It failed the moment a search came up empty and left `$FFFF` in A:
// `flag N: ROM 0, port 1`, on four movies at once.
//
// Carry is the one flag that is really the routine's: `PLD` does not touch it,
// so it is still what the `CPX #$185E` that ended the walk left — always clear,
// because the walk always ends the same way, running a fixed 32 slots. Y is
// untouched after the `STY $3C` on the way in.
// What each run of a block of `$80:B123` costs, indexed by `ActorNearestBlock`,
// and of `$80:B18F` too: see `ActorNearestWork` for why one table is enough for
// both and what the two id blocks in the middle are doing there.
//
// **No `dp_unaligned` anywhere, and no shim here checks `in->d`.** `$80:B124
// LDA #$0000 : TCD` puts the routine on page zero before it touches a
// direct-page word, exactly as `$80:BF1D PEA $0000 : PLD` does for
// `actor_notify_box`. The `dp` counts are recorded all the same, because they
// are how a reader checks the instruction list against the listing.
//
// The four id blocks climb **152, 182, 212, 236**, and the last spacing is 24
// where the first two are 30. That is not a slip: a failed test costs `CMP` +
// an untaken branch = 30 either way, but the first three *succeed* on a taken
// `BEQ` (36) while the fourth succeeds by falling through its `BNE` (30). The
// same asymmetry is the reason `NEAR_BLK_WRONG_ID` is exactly six more than
// `NEAR_BLK_ID_D` — one branch, taken instead of not.
static const CosimRun NEAREST_COST[NEAREST_BLOCK_COUNT] = {
    // `PHD : LDA #$0000 : TCD : STX $3A : STY $3C : LDA #$FFFF : STA $38 :
    // LDX #$1ACA`, and at the far end `LDX $44 : LDA $38 : PLD : RTL`. Both
    // run exactly once, so they are one block.
    [NEAR_BLK_FIXED] = {28 + 18 + 12 + 28 + 28 + 18 + 28 + 18 + 28 + 28 + 34 +
                            42,
                        23, 5},
    // $80:B134 LDA $0000,X : BPL taken.
    [NEAR_BLK_UNDRAWN] = {40 + 18, 5},
    // ...not taken, then `LSR A : BCC` taken.
    [NEAR_BLK_INACTIVE] = {40 + 12 + 12 + 18, 7},
    // ...not taken either, then `LDA $000E,X` and the four `CMP`s, all missed.
    [NEAR_BLK_WRONG_ID] = {40 + 12 + 12 + 12 + 40 + 18 + 12 + 18 + 12 + 18 + 12 +
                               18 + 18,
                           31, 0},
    [NEAR_BLK_ID_A] = {40 + 12 + 12 + 12 + 40 + 18 + 18, 16},
    [NEAR_BLK_ID_B] = {40 + 12 + 12 + 12 + 40 + 18 + 12 + 18 + 18, 21},
    [NEAR_BLK_ID_C] = {40 + 12 + 12 + 12 + 40 + 18 + 12 + 18 + 12 + 18 + 18, 26},
    [NEAR_BLK_ID_D] = {40 + 12 + 12 + 12 + 40 + 18 + 12 + 18 + 12 + 18 + 12 +
                           18 + 12,
                       31},
    // $80:B1AB CMP #$0003 : BNE, not taken and taken. The sibling's whole
    // difference from the routine above, in two lines.
    [NEAR_BLK_ID3_MATCH] = {40 + 12 + 12 + 12 + 40 + 18 + 12, 16},
    [NEAR_BLK_ID3_MISS] = {40 + 12 + 12 + 12 + 40 + 18 + 18, 16},
    // $80:B153 LDA $0002,X : SEC : SBC $3A : STA $3E : BCS taken.
    [NEAR_BLK_DX_POS] = {40 + 12 + 28 + 28 + 18, 10, 2},
    // ...not taken, so `EOR #$FFFF : INC A` runs. Always 24 more: the two
    // instructions' 30, less the 6 the branch saves by not being taken.
    [NEAR_BLK_DX_NEG] = {40 + 12 + 28 + 28 + 12 + 18 + 12, 14, 2},
    // $80:B161 STA $42 : LDA $0006,X : SEC : SBC $3C : STA $40 : BCS taken.
    [NEAR_BLK_DY_POS] = {28 + 40 + 12 + 28 + 28 + 18, 12, 3},
    // ...and the same 24 on the other axis.
    [NEAR_BLK_DY_NEG] = {28 + 40 + 12 + 28 + 28 + 12 + 18 + 12, 16, 3},
    // $80:B171 CLC : ADC $42 : STA $42 : CMP $38 : BCS taken — not nearer.
    [NEAR_BLK_KEPT] = {12 + 28 + 28 + 28 + 18, 9, 3},
    // ...not taken, then `STA $38 : STX $44`.
    [NEAR_BLK_CLOSER] = {12 + 28 + 28 + 28 + 12 + 28 + 28, 13, 5},
    // $80:B17E TXA : SEC : SBC #$0014 : TAX : CPX #$185E : BCS taken.
    [NEAR_BLK_LOOP_NEXT] = {12 + 12 + 18 + 12 + 18 + 18, 11},
    // ...not taken, which is the 32nd slot and no other.
    [NEAR_BLK_LOOP_DONE] = {12 + 12 + 18 + 12 + 18 + 12, 11},
};

// No `bool`: there is nothing here to decline. A fixed 32 slots, no dispatch,
// no leaf call, and every branch in the routine has a block.
static int nearest_cycles(const ActorNearestWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < NEAREST_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&NEAREST_COST[i], fast);
  return cycles;
}

static void shim_actor_nearest(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  (void)rom;
  uint16_t dist = 0;
  ActorNearestWork work;
  uint16_t found = actor_nearest_counted(w, in->x, in->y, &dist, &work);
  R36_COSIM_COST(nearest_cycles(&work, in->fastrom));
  out->a = dist;
  out->x = found;
  out->y = in->y;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->c = false;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B093  actor_gap — Y = a record; A = its distance from $38/$3A
// ---------------------------------------------------------------------------
//
// **The only routine in the registry whose point arrives in memory rather than
// in registers.** It is a `JSR` leaf inside bank $80 with two callers, both of
// which write `$38` and `$3A` before entering it, so there is nothing to read
// off `in` but Y — and reading them from WRAM inside the shim is not a shortcut,
// it is the calling convention.
//
// Carry is declared on two of the three exits and withheld on the third. See
// `ActorGapRegs`: the empty-record exit does not execute anything that writes
// it, so claiming a value there would be asserting what the *caller* left, and
// `out->flags` is per call precisely so a shim can decline.
//
// X is untouched; Y is the record, and the routine never writes either.
static void shim_actor_gap(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  (void)rom;
  ActorGapRegs r;
  actor_gap(w, in->y, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | (r.has_c ? COSIM_FLAG_C : 0);
}

// ---------------------------------------------------------------------------
// $80:B18F  actor_nearest_id3 — X, Y = the point; X = winner, A = its distance
// ---------------------------------------------------------------------------
//
// `actor_nearest`'s register contract exactly, because it is `actor_nearest`
// with one collision id in place of four: N and Z off the closing `PLD` and so
// the caller's own direct page, carry clear from the `CPX` that ends a walk
// which always ends the same way, Y untouched from the `STY $3C` on the way in.
static void shim_actor_nearest_id3(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  uint16_t dist = 0;
  ActorNearestWork work;
  uint16_t found = actor_nearest_id3_counted(w, in->x, in->y, &dist, &work);
  R36_COSIM_COST(nearest_cycles(&work, in->fastrom));
  out->a = dist;
  out->x = found;
  out->y = in->y;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->c = false;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B22A  actor_bearing — X = the record to look from, Y = the one to find
// ---------------------------------------------------------------------------
//
// A is the direction, X is the table index that produced it, Y is the record
// asked about and is never written. N and Z are the closing `PLD`'s — the same
// split as `actor_nearest`, and for the same reason.
//
// Carry is the routine's own and is **not** the horizontal `CMP`'s: the `ADC`
// under it overwrites it on every path but the equal one. See
// `ActorBearingRegs`, where the 2,078 diverging calls that said so are
// recorded.
static void shim_actor_bearing(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  ActorBearingRegs r;
  actor_bearing(w, rom, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = in->y;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B1EC  actor_bearing_point — A = a record, X and Y = the point
// ---------------------------------------------------------------------------
//
// The same three registers as `actor_bearing`, and the same flags, off a
// routine that reaches only three of its twelve table entries. Y out is the
// record — `PHA : ... : PLA : TAY` puts the accumulator argument there on the
// way in and nothing moves it afterwards — which is *not* the `y` that came in,
// and is the one place this routine's contract differs from its sibling's.
static void shim_actor_bearing_point(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  ActorBearingRegs r;
  actor_bearing_point(w, rom, in->a, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = in->a;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B26B  player_in_range — A = the range, X and Y = the point
// ---------------------------------------------------------------------------
//
// Three exits, three different carries, and two of them return the same player;
// `PlayerPickRegs` is where the four cases are set out. N and Z are the `PLD`'s
// as everywhere in this family.
static void shim_player_in_range(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  PlayerPickRegs r;
  player_in_range(w, in->a, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B2A5  player_bearing — A = the range, X and Y = the point
// ---------------------------------------------------------------------------
//
// The busiest of the six: 19,898 calls over twenty-seven sites in three banks,
// and the one the enemies actually steer by.
//
// All three registers are claimed and all three are different per exit — the
// direction exit hands back a distance in X and a record in Y, both pulled off
// the stack around the table lookup, while the zero exit hands back the
// caller's own `x` and whatever `$D4` held. Carry is clear on every direction
// exit and set on the zero one, and it is not a `SEC`/`CLC` pair that makes it
// so: it is the `ASL` that doubles a word-table index, which never carries.
static void shim_player_bearing(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  PlayerPickRegs r;
  player_bearing(w, rom, in->a, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:BF67  actor_at_point — A = self, X, Y = the point; carry = occupied
// ---------------------------------------------------------------------------
//
// Both exits are `PLD` and then an explicit `SEC` or `CLC`, so **carry is the
// routine's own** and N and Z are the `PLD`'s — the same split as
// `actor_nearest`, arrived at from the opposite direction, and the reason to
// read the last three instructions of a routine rather than the last one that
// looks like it computes something.
//
// A, X and Y are all claimed and none of them is tidy. The ROM never tidies
// them: it falls out of the loop with whatever the last iteration left, so what
// a caller sees is the last comparison's arithmetic in A, the loop index in X —
// `$FFFE` when the walk ran out, because the count is a byte count and always
// even — and the last entry it looked at in Y, which on the found path is the
// record that matched and is presumably the point.
// What each block of `$80:BF67` costs, indexed by `AtPointBlock`. `$80:BF68
// PEA $0000 : PLD` again, so again no `dp_unaligned`, and again the `dp` counts
// are here to be checked against the listing rather than to be paid.
//
// The per-record dismissals climb **86, 150, 202, 262, 274**, and every step is
// the question the record just failed plus the six a taken branch costs over an
// untaken one. `ID_33` at 262 sitting *under* `ID_BAND` at 274 is the whole of
// the reason those two are separate blocks.
static const CosimRun AT_POINT_COST[AT_POINT_BLOCK_COUNT] = {
    // `PHD : PEA $0000 : PLD : STA $38 : STX $3A : STY $3C : LDX $9C : BEQ`
    // not taken, and the first `DEX DEX`.
    [AT_BLK_PROLOGUE] = {28 + 34 + 34 + 28 + 28 + 28 + 28 + 12 + 12 + 12, 17, 4},
    // ...taken instead, and the `PLD : CLC : RTL` the walk's end shares.
    [AT_BLK_EMPTY] = {28 + 34 + 34 + 28 + 28 + 28 + 28 + 18 + 34 + 12 + 42, 18,
                      4},
    // $80:BF78 LDY $137E,X : CPY $38 : BEQ taken.
    [AT_BLK_SELF] = {40 + 28 + 18, 7, 1},
    // ...not taken, then `LDA $0000,Y : LSR A : BCC` taken.
    [AT_BLK_INACTIVE] = {40 + 28 + 12 + 40 + 12 + 18, 13, 1},
    // ...not taken, then `LDA $000E,Y : BEQ` taken.
    [AT_BLK_NO_ID] = {40 + 28 + 12 + 40 + 12 + 12 + 40 + 18, 18, 1},
    // ...not taken, then `CMP #$000C : BCC` not taken and `CMP #$0033 : BEQ`
    // taken. The band's ceiling, and the cheapest way out of the band.
    [AT_BLK_ID_33] = {40 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 + 12 + 18 + 18,
                      28, 1},
    // ...not taken either, then the `BCC` under it: $0C..$32.
    [AT_BLK_ID_BAND] = {40 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 + 12 + 18 +
                            12 + 18,
                        30, 1},
    // $80:BF8D CMP #$000C : BCC taken — under the band, on to the named tests.
    [AT_BLK_ARRIVE_LOW] = {40 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 + 18, 23,
                           1},
    // ...and over the band, falling out of the bottom of the range tests into
    // the same two comparisons, which no id up there can match.
    [AT_BLK_ARRIVE_HIGH] = {40 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 + 12 +
                                18 + 12 + 12,
                            30, 1},
    // One `CMP #$xxxx : BEQ`, missed and hit. Counted per test rather than per
    // path: two named tests and two arrivals would otherwise be four blocks.
    [AT_BLK_NAME_MISS] = {18 + 12, 5},
    [AT_BLK_NAME_HIT] = {18 + 18, 5},
    // $80:BFA0 LDA $0002,Y : SEC : SBC $3A : CLC : ADC #$0006 : CMP #$000C :
    // BCS taken, and the same six instructions on Y at $80:BFAF.
    [AT_BLK_FAR_X] = {40 + 12 + 28 + 12 + 18 + 18 + 18, 15, 1},
    [AT_BLK_NEAR_X] = {40 + 12 + 28 + 12 + 18 + 18 + 12, 15, 1},
    [AT_BLK_FAR_Y] = {40 + 12 + 28 + 12 + 18 + 18 + 18, 15, 1},
    [AT_BLK_NEAR_Y] = {40 + 12 + 28 + 12 + 18 + 18 + 12, 15, 1},
    // $80:BFBE PLD : SEC : RTL, which leaves the walk without the loop tail.
    [AT_BLK_HIT] = {34 + 12 + 42, 3},
    // $80:BFC1 DEX DEX : BPL taken.
    [AT_BLK_LOOP_NEXT] = {12 + 12 + 18, 4},
    // ...not taken, and the `PLD : CLC : RTL` at $80:BFC5.
    [AT_BLK_LOOP_DONE] = {12 + 12 + 12 + 34 + 12 + 42, 7},
};

// Nothing to decline, as with `nearest_cycles`: no dispatch, no leaf call, and
// a block for every branch.
static int at_point_cycles(const AtPointWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < AT_POINT_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&AT_POINT_COST[i], fast);
  return cycles;
}

static void shim_actor_at_point(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)rom;
  AtPointRegs r;
  AtPointWork work;
  actor_at_point_counted(w, in->a, in->x, in->y, &r, &work);
  R36_COSIM_COST(at_point_cycles(&work, in->fastrom));
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.found;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:BFC8  actor_obstacle_at_point — X, Y = the point; carry = the step is
//           blocked
// ---------------------------------------------------------------------------
//
// The same flag contract as `actor_at_point` next door, for the same reason:
// `PLD` and then an explicit `SEC`/`CLC`, so carry is the routine's and N and Z
// are the caller's own direct page coming back off the stack. Written that way
// from the start this time, rather than found by four movies failing at once.
//
// **A is an input here even though the routine never reads it.** There is no
// `STA` on the way in, so the `LDX $9C : BEQ` path returns with the caller's
// accumulator untouched, and a shim that published a constant would diverge on
// the first frame with an empty display list. `in->a` is not passed because the
// routine wants it — it is passed because the routine's silence about it is
// part of the contract.
// `AT_POINT_COST` with this routine's own id chain, indexed by `ObstacleBlock`.
//
// **Its last seven entries are `AT_POINT_COST`'s last seven, to the cycle**,
// because `$80:C021`-`$80:C03E` is `$80:BFA0`-`$80:BFBD` byte for byte and the
// loop tails match too. That is not a shortcut taken here — both were counted
// off their own listings — it is the check that neither was miscounted.
//
// Where they part is the top of a record: two `CPY` against the player records
// instead of one against a caller's, a `CMP #$005C` ceiling the other has no
// use for, and seven named comparisons where the other has two. The dismissals
// climb **86, 126, 190, 242, 302, 314, 344**, one question apiece.
static const CosimRun OBSTACLE_COST[OBSTACLE_BLOCK_COUNT] = {
    // `PHD : PEA $0000 : PLD : STX $3A : STY $3C : LDX $9C : BEQ` not taken,
    // and the first `DEX DEX`. No `STA`: this one files no self.
    [OBST_BLK_PROLOGUE] = {28 + 34 + 34 + 28 + 28 + 28 + 12 + 12 + 12, 15, 3},
    [OBST_BLK_EMPTY] = {28 + 34 + 34 + 28 + 28 + 28 + 18 + 34 + 12 + 42, 16, 3},
    // $80:BFD7 LDY $137E,X : CPY $D2 : BEQ taken.
    [OBST_BLK_PLAYER_A] = {40 + 28 + 18, 7, 1},
    // ...not taken, then `CPY $D4 : BEQ` taken.
    [OBST_BLK_PLAYER_B] = {40 + 28 + 12 + 28 + 18, 11, 2},
    // ...not taken, then `LDA $0000,Y : LSR A : BCC` taken.
    [OBST_BLK_INACTIVE] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 18, 17, 2},
    // ...not taken, then `LDA $000E,Y : BEQ` taken.
    [OBST_BLK_NO_ID] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 12 + 40 + 18, 22, 2},
    // ...not taken, then `CMP #$000C : BCC` not taken, `CMP #$0033 : BEQ`
    // taken.
    [OBST_BLK_ID_33] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 +
                            12 + 18 + 18,
                        32, 2},
    // ...the `BCC` under it instead: $0C..$32.
    [OBST_BLK_ID_BAND] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 +
                              12 + 18 + 12 + 18,
                          34, 2},
    // ...neither, then `CMP #$005C : BCS` taken: over the ceiling.
    [OBST_BLK_ID_HIGH] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 12 + 40 + 12 + 18 +
                              12 + 18 + 12 + 12 + 18 + 18,
                          39, 2},
    // $80:BFF0 CMP #$000C : BCC taken — under the band.
    [OBST_BLK_ARRIVE_LOW] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 12 + 40 + 12 +
                                 18 + 18,
                             27, 2},
    // ...$34..$5B, which is past the band, under the ceiling, and falls into
    // the named chain anyway. The only ids for which `CMP #$0037` is live.
    [OBST_BLK_ARRIVE_HIGH] = {40 + 28 + 12 + 28 + 12 + 40 + 12 + 12 + 40 + 12 +
                                  18 + 12 + 18 + 12 + 12 + 18 + 12,
                              39, 2},
    [OBST_BLK_NAME_MISS] = {18 + 12, 5},
    [OBST_BLK_NAME_HIT] = {18 + 18, 5},
    // From here down, `AT_POINT_COST`'s tail exactly.
    [OBST_BLK_FAR_X] = {40 + 12 + 28 + 12 + 18 + 18 + 18, 15, 1},
    [OBST_BLK_NEAR_X] = {40 + 12 + 28 + 12 + 18 + 18 + 12, 15, 1},
    [OBST_BLK_FAR_Y] = {40 + 12 + 28 + 12 + 18 + 18 + 18, 15, 1},
    [OBST_BLK_NEAR_Y] = {40 + 12 + 28 + 12 + 18 + 18 + 12, 15, 1},
    [OBST_BLK_HIT] = {34 + 12 + 42, 3},
    [OBST_BLK_LOOP_NEXT] = {12 + 12 + 18, 4},
    [OBST_BLK_LOOP_DONE] = {12 + 12 + 12 + 34 + 12 + 42, 7},
};

static int obstacle_cycles(const ObstacleWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < OBSTACLE_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&OBSTACLE_COST[i], fast);
  return cycles;
}

static void shim_actor_obstacle_at_point(Wram* w, const Rom* rom,
                                         const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  ObstacleRegs r;
  ObstacleWork work;
  actor_obstacle_at_point_counted(w, in->a, in->x, in->y, &r, &work);
  R36_COSIM_COST(obstacle_cycles(&work, in->fastrom));
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:AE14 / $80:AE97  terrain_blocked — X, Y = the point; carry = blocked
// ---------------------------------------------------------------------------
//
// Two routines, one shim shape. Both open `PHD` and close `PLD : RTL`, so N and
// Z are the caller's direct page yet again; the difference is where carry comes
// from. `$80:AE14` ends its last probe on `LSR A` and lets the shifted-out bit
// *be* the answer — no branch, no `SEC`, the carry is simply bit 0 of the
// attribute word — while `$80:AE97` tests with `BIT #$0002`, which cannot set
// carry, and so needs an explicit `CLC`/`SEC` at each of its two exits.
//
// That difference is also why A comes back shifted from one and not the other,
// and `port/terrain.c` applies it in the wrapper rather than the shared body.
static void shim_terrain_blocked(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  terrain_blocked(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_terrain_blocked_enemy(Wram* w, const Rom* rom,
                                       const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  terrain_blocked_enemy(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B422  terrain_out_of_bounds — X, Y = the point; carry = off the map
// ---------------------------------------------------------------------------
//
// **The first routine here with no `PHD`, and it is the interesting case.**
// Every other shim in this file that publishes N and Z takes them from the
// `PLD` on the way out, because a pull sets them from the value pulled. This
// one never touches the direct page, so there is nothing to take them from and
// they are simply whatever the instruction that decided the answer left.
//
// There are six of those, and they do not agree. Four exits arrive at a shared
// `SEC : RTL`, which does not touch N or Z — so those carry the flags of the
// `TXA`, `TYA` or `CMP` that branched to it. A fifth branches straight to the
// `RTL` and keeps its own compare's carry rather than the `SEC`'s. The sixth
// falls off the end, and there the last `CMP` is the entire answer.
//
// Reading the routine's final instruction — which is `CMP $00B4` — and
// publishing that everywhere would be right on one path in six.
static void shim_terrain_out_of_bounds(Wram* w, const Rom* rom,
                                       const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  BoundsRegs r;
  terrain_out_of_bounds(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = in->x;  // `TXA`/`TYA` read them and nothing writes either
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $82:90F7  terrain_blocked_wide — X, Y = the point; carry = blocked
// ---------------------------------------------------------------------------
//
// `$80:AE14` with five tiles across instead of three, the loop written out ten
// times, a nine-bit tile mask, and a second rule: a tile whose index is below
// `W_TILE_PRIORITY_BELOW` is refused before its attribute word is read.
//
// `A` is therefore not always an attribute word. On the two exits the priority
// threshold decides it is the tile *index*, unshifted — and on those exits `Y`
// is the map offset, except for the first probe, which does not index at all
// and returns the caller's own `Y`.
//
// **Unrolling is what makes this priceable.** A rolled loop would hide its
// counter arithmetic in a block that has to be right ten times over; ten copies
// of the same two tests, each reached by its own load, price as a straight sum
// with nothing to iterate. The five loads are the only thing that varies, and
// the whole table is those five plus four outcomes and two exits.
//
// The routine forces `D` to zero itself — `LDA #$0000 : TCD`, four instructions
// in — so nothing here owes a direct-page penalty. The `dp` column is a
// listing cross-check and nothing more, and the two stores it counts in the
// prologue happen *after* the `TCD`, which is the only reason that is true.
static const CosimRun WIDE_COST[WIDE_BLOCK_COUNT] = {
    // $82:90F7-$82:911D. `PHD : TXA : SEC : SBC : PHA` on the caller's page,
    // then `TCD` and the same again for Y, then `ADC $7E4328,X` (40: the row
    // table is in bank $7E and costs 8 a byte) and the pointer into `$28`.
    [WIDE_BLK_PROLOGUE] = {28 + 12 + 12 + 18 + 28 + 18 + 12 + 12 + 12 + 18 +
                               12 + 12 + 18 + 12 + 34 + 12 + 12 + 18 + 12 +
                               40 + 28 + 18 + 28,
                           40, 2},  // 426
    // The five loads. `LDA [$28],Y` is 52 — three bytes of pointer at 6 apiece
    // for the fetch, then two WRAM reads at 8 and the indirection's own idle.
    [WIDE_BLK_LOAD_FIRST] = {52, 2, 1},              // `LDA [$28]`
    [WIDE_BLK_LOAD_IMM] = {18 + 52, 5, 1},           // `LDY #imm`
    [WIDE_BLK_LOAD_ROW] = {28 + 52, 4, 2},           // `LDY $B2`
    [WIDE_BLK_LOAD_ROW_INC] = {28 + 12 + 12 + 52, 6, 2},        // ...`INY:INY`
    [WIDE_BLK_LOAD_ROW_ADD] = {28 + 12 + 18 + 12 + 52, 9, 2},   // ...`ADC:TAY`
    // `AND #$01FF : CMP $00DC : BCC`. The `CMP` is absolute and lands in the
    // bank $82 mirror of low WRAM, which is why it is 34 and not 18.
    [WIDE_BLK_PRIO_PASS] = {18 + 34 + 12, 8, 0},  // 64
    [WIDE_BLK_PRIO_FAIL] = {18 + 34 + 18, 8, 0},  // 70
    // `ASL : TAY : LDA [$BA],Y : LSR : LSR : BCS`.
    [WIDE_BLK_ATTR_PASS] = {12 + 12 + 52 + 12 + 12 + 12, 8, 1},  // 112
    [WIDE_BLK_ATTR_FAIL] = {12 + 12 + 52 + 12 + 12 + 18, 8, 1},  // 118
    // ...and the tenth, which has no `BCS` because the exit is the next
    // instruction. One block for both outcomes: the branch it does not have is
    // the only thing the other two disagree about.
    [WIDE_BLK_ATTR_LAST] = {12 + 12 + 52 + 12 + 12, 6, 1},  // 100
    [WIDE_BLK_ROW_BRA] = {18, 2, 0},
    [WIDE_BLK_EXIT_SEC] = {12 + 34 + 40, 3, 0},  // 86
    [WIDE_BLK_EXIT_PLD] = {34 + 40, 2, 0},       // 74
};

// **634 is this routine's registry floor, and it is four blocks added up.**
// Prologue 426, the first probe's bare `LDA [$28]` 52, the priority test
// rejecting at 70, and `SEC : PLD : RTS` at 86. There is no cheaper call: the
// pass loop cannot be entered fewer than once and the first tile cannot be
// refused sooner than by its own number. The registry has said `634..3,268`
// since long before this table existed, from the corpus reporting the cheapest
// call it ever saw.
//
// The ceiling agrees the same way and is worth doing because it exercises every
// row of the table: all ten probes clear is 426 + 228 + 4x246 + 18 + 256 + 280
// + 2x298 + 286 + 74 = 3,148, and 3,268 - 3,148 = 120, which is three DRAM
// refreshes over a call three scanlines long.
static int wide_cycles(const TerrainWideWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < WIDE_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&WIDE_COST[i], fast);
  return cycles;
}

static void shim_terrain_blocked_wide(Wram* w, const Rom* rom,
                                      const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  TerrainWideWork work;
  memset(work.blocks, 0, sizeof work.blocks);
  terrain_blocked_wide_counted(w, in->x, in->y, &r, &work);
  R36_COSIM_COST(wide_cycles(&work, in->fastrom));
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:AF2C / $80:B05F / $80:B03B / $80:AF66 — the rest of the attribute word
// ---------------------------------------------------------------------------
//
// Four more masks out of the same sixteen-bit word, three of them one tile and
// the fourth the familiar six. All four `PHD`/`PLD`, so N and Z are the
// caller's direct page and only carry is ever the answer.
//
// The one thing worth watching in a shim here is **A**, because the four do not
// agree about it. `$80:AF2C` tests with `BIT #$0004` and hands back the whole
// attribute word; the other three test with `AND` and hand back the mask or
// zero, which says nothing carry did not. And `$80:AF2C` has a fifth exit
// before any of that — `JSL $80B422` deciding the point is off the map, with A
// the bounds test's own and X and Y never touched.
static void shim_terrain_point_bit2(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  terrain_point_bit2(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_terrain_point_bit8(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  terrain_point_bit8(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// X and Y are tile coordinates here, not pixels — this is the one whose caller
// has already divided. X comes back doubled, which is `tilemap_tile_addr`'s
// `PLX` showing through rather than anything this routine decided.
static void shim_terrain_tile_bit3(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  terrain_tile_bit3(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// The inverted one: carry *clear* means all six probes carried bit 12.
static void shim_terrain_footprint_bit12(Wram* w, const Rom* rom,
                                         const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TerrainRegs r;
  terrain_footprint_bit12(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:AFFB  partner_near — A = the caller's record, X/Y = a point
// ---------------------------------------------------------------------------
//
// Sat in the middle of those four in the ROM and belongs with `$80:A8B3`
// instead: the other test about the other player, with the leash taken off.
//
// **No `PHD`**, which puts it in the small class `terrain_out_of_bounds`
// started — N and Z are whatever decided, and there are two kinds of decision.
// The two absent-player exits leave a `BIT`'s flags, and `BIT abs` sets Z from
// **A AND memory** rather than from memory, so the routine's existence check is
// really an overlap test that works only because every actor record pointer in
// the game has bits 11 and 12 set. `port/step.h` says why at length.
static void shim_partner_near(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  (void)rom;
  PartnerRegs r;
  partner_near(w, in->a, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:E450  step_propose — the mover's page in D; $34/$36 = where it wants to be
// ---------------------------------------------------------------------------
//
// A `JSR`, so the direct page is the caller's and the caller is a thread: `D`
// is the mover's own 128-byte page, and the shim hands it over rather than
// assuming, exactly as the collision handlers above do.
//
// The published flags are not the ones the routine's last instruction sets.
// `CPX #$0000 : BEQ` sits between the first add and the second, so the single-
// step path — which is most of them — returns that compare's flags and not the
// arithmetic's: `Z` set, `C` set, `N` clear, regardless of where the mover
// ended up. Only a double step returns the `ADC`'s.
static void shim_step_propose(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  StepProposeRegs r;
  step_propose(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A8B3  step_tether_blocked — X, Y = the candidate; carry = too far away
// ---------------------------------------------------------------------------
//
// Back to a `PHD`, so N and Z are the caller's direct page again.
//
// The three register outputs are all live and all different per exit, which is
// why they are worth stating: the alone exit leaves `A` at zero — the `LDA
// #$0000` that set the direct page, never touched again — with `Y` the zero
// record it just read and `X` the candidate it was handed. The inside-window
// exit leaves the biased Y offset in `A` and the reference record in `Y`. The
// far path overwrites all three with the two players' separation, `$D2` and
// `$D4`.
static void shim_step_tether_blocked(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TetherRegs r;
  step_tether_blocked(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.blocked;
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A54D / $80:A588 / $80:9E6D -- the last three leaves under the camera
// ---------------------------------------------------------------------------
//
// Small enough to take together, and between them they finish the layer the
// four scroll routines stand on. See `port/camera.h` for each.
//
// `$80:9E6D` is the one worth pausing on. It opens `BIT $26`, and `BIT` against
// memory sets N from **bit 15 of the operand** but Z from **A AND the operand**
// -- so the Z this routine returns on its first exit is a fact about the
// caller's accumulator, which it never loads and has no other use for. A shim
// that derived Z from anything the routine computes would be wrong on every
// call that takes that path, and right by accident on the rest.
static void shim_camera_window_update(Wram* w, const Rom* rom,
                                      const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  CameraWindowRegs r;
  camera_window_update(w, &r);
  out->a = r.a;
  out->x = in->x;  // neither index is mentioned in twenty-seven instructions
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_camera_split_y(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)rom;
  CameraSplitRegs r;
  camera_split_y(w, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_vram_queue_request(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  VramRequestRegs r;
  vram_queue_request(w, in->a, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  // Seven bytes and none of them touches carry, on any of the three paths.
  out->c = in->c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A61D  tilemap_copy_row -- X = column, Y = row
// ---------------------------------------------------------------------------
//
// `tilemap_copy_column`'s twin, and the last leaf under the Y-axis scroll
// routines. It buys its own 66-byte strip out of the arena instead of being
// handed one, which is why it carries the allocator's guard as well.
//
// Its outputs are the least summary-like in the registry: A and carry are
// *whatever the thirty-third tile happened to be*, N and Z belong to a `DEY`
// that has already run off the end, and X belongs to the allocator's `PLX`
// three instructions before the loop even started. Four registers, four
// unrelated origins, and the routine returns nothing that describes its work.
static bool guard_tilemap_copy_row(Wram* scratch, const Rom* rom,
                                   const CosimRegs* in) {
  (void)rom;
  (void)in;
  if (tilemap_copy_row_supported(scratch)) return true;
  cosim_census_note("tilemap arena exhausted", TILEMAP_COPY_ROW_ENTRY);
  return false;
}

static void shim_tilemap_copy_row(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  (void)rom;
  TilemapCopyRegs r;
  tilemap_copy_row(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A401  tilemap_buffer_alloc -- A = bytes wanted; A = where they start
// ---------------------------------------------------------------------------
//
// The arena `$80:A5E5` fills. Registered with a guard rather than a coverage
// site, which is the interesting choice here: the routine's one branch is a
// spin waiting for the arena to be given back, no input in ten traces has ever
// taken it, and a site nothing can reach would sit in the untaken list forever
// diluting the number `coverage.h` exists to keep. A guard says the same thing
// and costs nothing while it never fires. See `port/camera.h`.
static bool guard_tilemap_buffer_alloc(Wram* scratch, const Rom* rom,
                                       const CosimRegs* in) {
  (void)rom;
  if (tilemap_buffer_alloc_supported(scratch, in->a)) return true;
  cosim_census_note("tilemap arena exhausted", TILEMAP_BUFFER_ALLOC_ENTRY);
  return false;
}

static void shim_tilemap_buffer_alloc(Wram* w, const Rom* rom,
                                      const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TilemapAllocRegs r;
  tilemap_buffer_alloc(w, in->a, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A5E5  tilemap_copy_column -- A = tiles, X = column, Y = row
// ---------------------------------------------------------------------------
//
// One step up the camera chain from `tilemap_tile_addr`, and the routine that
// puts a new strip of map on screen when the view has drifted eight pixels. The
// count arrives in A, goes onto the stack, and comes back off through `$01,S`
// after the `JSL` -- so the shim has nothing to do about it, but the registry
// entry's `stack_bytes` does.
//
// Its flags are, for once, the ones a reader would guess: `ADC $54` is the last
// thing before the `RTS` and N, Z and C all describe the destination pointer it
// returns in A. The chain's other two routines both end on a pull.
static void shim_tilemap_copy_column(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TilemapCopyRegs r;
  tilemap_copy_column(w, in->a, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:AD1C  tilemap_tile_addr -- X = column, Y = row; A = the address
// ---------------------------------------------------------------------------
//
// The leaf under the camera. `$80:A93F` is the top portable row on the work
// ranking at 1.6%, and it cannot be substituted without the four tilemap scroll
// routines it dispatches to, which cannot be substituted without this. Fifteen
// bytes, no calls, 42,167 of them, and `hotbytes.py` says every byte runs
// exactly once per call.
//
// Three registers come back and no two of them come from the same place: A from
// the `ADC`, X from a `PLX` that puts back a *doubled* column rather than the
// caller's, and N and Z from that `PLX` rather than from A. See
// `port/terrain.h`.
static void shim_tilemap_tile_addr(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  TilemapAddrRegs r;
  tilemap_tile_addr(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = in->y;  // never mentioned after the `TYA`
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:ADC8 / $80:ADF3  tile_attrs_at_pixel / tile_attrs_at_tile
// ---------------------------------------------------------------------------
//
// One tile's attribute word, which is what `terrain_blocked` reads six of. 22
// call sites across banks $80, $81 and $82 reach the pixel form and four reach
// the tile form, and between them they are how everything that is not a
// footprint test asks the map a question.
//
// X and Y come straight back out: `PHX : PHY` at the top saved the caller's,
// the six `LSR`s work on copies, and `PLY : PLX` put the originals back. The
// shim therefore hands `in->x` and `in->y` through rather than modelling them,
// which is also why `port/terrain.h`'s register struct has neither.
//
// **N and Z are the closing `PLB`'s**, so they are the caller's data bank byte
// and not the attribute word -- the same trap as every `PHD` routine above,
// one register over. `$80:8480` is the only other place `CosimRegs::db` is
// read, and it is read for exactly this.
static void shim_tile_attrs_at_pixel(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TileAttrsRegs r;
  tile_attrs_at_pixel(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->c = r.c;
  out->n = (in->db & 0x80u) != 0;
  out->z = in->db == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_tile_attrs_at_tile(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  (void)rom;
  TileAttrsRegs r;
  tile_attrs_at_tile(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->c = r.c;
  out->n = (in->db & 0x80u) != 0;
  out->z = in->db == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:E86D  floor_effect -- what the tile under the player does to them
// ---------------------------------------------------------------------------
//
// The first thing `$80:D1FF` does every frame, before it looks at a button.
// Two of its three callees are already in this registry -- `$80:ADC8` and
// `$80:AE14` -- and the third, `$80:F935`, has exactly one call site in the
// cartridge and is inlined into `port/floor.c` rather than registered.
//
// It takes no argument. Everything comes out of the player thread's direct
// page, which is the caller's and is why `in->d` is passed through; there is no
// `PHD` anywhere in it, so there is also no `PLD` to take N and Z from and the
// flags are whichever comparison the exit stopped at. Eleven of those.
static void shim_floor_effect(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  FloorRegs r;
  floor_effect(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:D1FF  player_state_normal -- the player's ordinary frame
// ---------------------------------------------------------------------------
//
// **A state handler, not a subroutine.** `$80:D1EC JMP ($D1EF,X)` reaches it
// 26,972 times and the one real `JSR $D1FF` at `$80:D40B` reaches it 726 more.
// The harness does not mind -- `cosim_step` intercepts on `pc == r->entry` and
// never looks at how the PC got there, and the `RTS` returns to whoever called
// the dispatcher -- but the ranking's calls column undercounts its entries by
// 38x, which matters when reading the standing check.
//
// Everything it reaches is already C except `$80:EAE1 item_use`, so the guard
// declines the frames that would reach that and nothing else.
static void shim_player_state_normal(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  PlayerStateRegs r;
  player_state_normal(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static bool guard_player_state_normal(Wram* w, const Rom* rom,
                                      const CosimRegs* in) {
  (void)rom;
  return player_state_normal_supported(w, in->d);
}

// ---------------------------------------------------------------------------
// $80:CD20  lzss_decompress — the stream picks every branch, and the price is
// still exact
// ---------------------------------------------------------------------------
//
// Four frames a call at the smallest and seventy-one at the largest, measured on
// the five `level1.zmv` makes, so the same structural fact as `$80:AD2B` above:
// an NMI lands inside every one of them, `verify` abandons the lot, and what
// stands behind the substitution is `run`'s per-pass comparison of all 128 KB
// rather than a per-call diff. `run_only`. The shim is four lines — the argument
// is the
// word at `s + 4` (`$80:CD27  LDA $06,S`, six deep because the routine's own
// `PHD` is already down) and the exit is `$80:CDD2  SEC : LDA $2C : SBC $40 :
// TAY : PLD : RTL`.
//
// **What is new here is the price.** Everything registered before this is costed
// one of two ways: a mean `verify` watched the ROM take, or a count nothing had
// to watch because the trip counts fall out of the arguments. This routine is
// neither. Every branch in it is decided by the compressed stream — how many
// tokens, which are literals, how long each match runs — and there is no
// argument, table or piece of state that predicts any of it.
//
// It is exact anyway, and the reason is worth stating because it is the general
// answer rather than a lucky property of this routine: **the port decompresses
// the same stream, so it takes the same branches, so it can count them.** The
// counts are not derived from the input, they are produced by doing the work.
// `LzssWork` is that tally and this is the ordinary `_counted` pattern; the
// thing that had to be checked was not whether the counts could be had but
// whether anything else moved the clock, and three things do:
//
//   * the `MVN` at `$CD42`, which is one instruction paid for `$0FEE` times;
//   * `LDA [$28]`, 4 master cycles dearer when the stream is in WRAM than in
//     the cartridge — hence `reads_fast` and `head_fast`;
//   * where the stream runs out, because the four places it can do that pop
//     different numbers of bytes on the way to `$CDD2`. Not a defensive case:
//     **every stream this game contains ends inside a match**, at the `BCS` on
//     `$CD87` — 31 of 31 measured across fifteen movies — because a flag byte
//     carries eight bits and the data runs out before all eight are spent, so
//     the leftover zero bits read as matches and the first of them finds
//     nothing. The tidy end-at-the-refill case has never once been seen, and a
//     model that assumed it would be short on every call in the game.
//
// Nothing else: `PEA $0000 : PLD` puts the routine on page zero, so no
// direct-page penalty anywhere, and the destination is WRAM by the guard.

// The window fill and everything either side of it. `MVN` is split out because
// it is one instruction whose cost is not in the instruction: `cpu.c` moves a
// single byte and then rewinds PC by three rather than looping inside the
// opcode, so each byte re-fetches all three program bytes and then reads one,
// writes one and idles twice. The count is A+1 and A is `$0FED`.
static const CosimRun LZSS_PROLOGUE_ROM = {668, 53};   // $CD20..$CD53, less MVN
static const CosimRun LZSS_PROLOGUE_WRAM = {672, 51};  // ...header word in WRAM
static const CosimRun LZSS_MVN_BYTE = {46, 3};         // $CD42, $0FEE of them
#define LZSS_MVN_BYTES 0x0feeu

// $CDD2..$CDD9, the `RTL` included. **Every model in this file is priced to the
// window `verify` measures**, which runs from the entry PC to the caller's
// return address and therefore contains the routine's own `RTS` or `RTL`. That
// is the only window a measurement can be taken over, so it is the one a model
// is written for, and the one place the boundary has to be stated is here.
//
// `run` used to disagree with that, silently. `native_return` parks the CPU on
// `ret_op` and the core executes it for real, so a budget that also contained it
// paid for it twice — 40 cycles for an `RTS`, which is a DRAM refresh to the
// cycle and so never looked like anything, and 42 for an `RTL`, which is what
// finally showed up: burn-end to back-at-the-caller measured 42 longer than the
// ROM's entry-to-`RTL` on three calls running. The harness now takes the tail
// off the budget itself, once, for every substituted call. See `tail_cycles` in
// `cosim/cosim.c`; nothing in this file has to remember it.
static const CosimRun LZSS_EPILOGUE = {156, 8};

// The token loop. Every branch is priced not taken and `LZSS_TAKEN` is added
// back per taken outcome, which is the same arrangement as `blockmap_cycles`.
static const CosimRun LZSS_HEAD = {24, 3};          // $CD56..$CD57
static const CosimRun LZSS_REFILL = {70, 8};        // $CD59..$CD5F
static const CosimRun LZSS_SHIFT = {36, 4};         // $CD61..$CD63
static const CosimRun LZSS_LITERAL = {338, 29};     // $CD65..$CD80, BRA taken
static const CosimRun LZSS_MATCH_HEAD = {460, 38};  // $CD82..$CDA6
static const CosimRun LZSS_RUN_BYTE = {384, 34};    // $CDA8..$CDC8
static const CosimRun LZSS_MATCH_TAIL = {114, 6};   // $CDCA..$CDCE, BRA taken
#define LZSS_TAKEN 6

// The two leaves. They are in the registry in their own right and `verify`
// scores both on every one of the million-odd calls the corpus makes — so these
// four numbers are the only ones in this model that have already been checked
// against the ROM by measurement, and they are the ones the model leans on
// hardest. Under substitution the leaves never execute, so their cost has to be
// here; `verify` is what says it is right.
static const CosimRun LZSS_READ_ROM = {258, 17};  // $CDDA..$CDE8, stream in ROM
static const CosimRun LZSS_READ_WRAM = {262, 15};
static const CosimRun LZSS_READ_SPENT = {98, 6};  // $CDDA, $CDE9, $CDEA
static const CosimRun LZSS_WRITE = {170, 9};      // $CDEB..$CDF3, dest in WRAM

// A stream that stops in the middle of a token. Each includes the `PLA`s at
// `$CDD0`/`$CDD1` that unwind what the block had pushed, which is the only
// reason the three differ at all and the reason `end` distinguishes them.
static const CosimRun LZSS_END_LIT = {120, 7};  // $CD65..$CD69, one PLA
static const CosimRun LZSS_END_M1 = {182, 9};   // $CD82..$CD87, two
static const CosimRun LZSS_END_M2 = {262, 16};  // $CD82..$CD8E, two

static int lzss_cycles(const LzssWork* k, bool fast) {
  // `tokens_lit` and `tokens_match` count blocks *entered*, so the last one is
  // in there whether or not the stream let it finish. These four are what the
  // model actually multiplies.
  const uint32_t tokens = k->tokens_lit + k->tokens_match;
  const uint32_t lits_full = k->tokens_lit - (k->end == 1 ? 1u : 0u);
  const uint32_t match_full = k->tokens_match - (k->end >= 2 ? 1u : 0u);
  // One `$CD56` per entered block, and one more when the stream ended at the
  // flag refill — that iteration reached the head and got no further.
  const uint32_t heads = tokens + (k->end == 0 ? 1u : 0u);
  // Every `JSR $CDDA` the call made: one per refill, one per literal, two per
  // match unless the first of the two is what ended it. Exactly one of them
  // found the stream spent, because that is what ended the loop.
  const uint32_t reads = k->refills + k->tokens_lit + 2u * k->tokens_match -
                         (k->end == 2 ? 1u : 0u);
  const uint32_t reads_slow = reads - 1u - k->reads_fast;

  long cycles = cosim_run_cycles(
                    k->head_fast ? &LZSS_PROLOGUE_ROM : &LZSS_PROLOGUE_WRAM,
                    fast) +
                cosim_run_cycles(&LZSS_EPILOGUE, fast) +
                (long)LZSS_MVN_BYTES * cosim_run_cycles(&LZSS_MVN_BYTE, fast);

  cycles += (long)heads * cosim_run_cycles(&LZSS_HEAD, fast);
  cycles += (long)k->refills * cosim_run_cycles(&LZSS_REFILL, fast);
  cycles += (long)tokens * cosim_run_cycles(&LZSS_SHIFT, fast);
  cycles += (long)lits_full * cosim_run_cycles(&LZSS_LITERAL, fast);
  cycles += (long)match_full * (cosim_run_cycles(&LZSS_MATCH_HEAD, fast) +
                                cosim_run_cycles(&LZSS_MATCH_TAIL, fast));
  cycles += (long)k->run_bytes * cosim_run_cycles(&LZSS_RUN_BYTE, fast);

  cycles += (long)k->reads_fast * cosim_run_cycles(&LZSS_READ_ROM, fast);
  cycles += (long)reads_slow * cosim_run_cycles(&LZSS_READ_WRAM, fast);
  cycles += cosim_run_cycles(&LZSS_READ_SPENT, fast);
  // One `JSR $CDEB` per literal that finished and one per byte of every match.
  cycles += (long)(lits_full + k->run_bytes) * cosim_run_cycles(&LZSS_WRITE,
                                                               fast);

  // The four branches, each of which the blocks above priced not taken.
  cycles += (long)LZSS_TAKEN *
            ((long)(heads - k->refills) +               // $CD57, flag bits left
             (long)k->tokens_match +                    // $CD63, a match token
             (long)(k->run_bytes - match_full) +        // $CDC8, another byte
             (long)(k->end == 0 ? 1 : 0));              // $CD5F, stream spent

  switch (k->end) {
    case 1:
      cycles += cosim_run_cycles(&LZSS_END_LIT, fast);
      break;
    case 2:
      cycles += cosim_run_cycles(&LZSS_END_M1, fast);
      break;
    case 3:
      cycles += cosim_run_cycles(&LZSS_END_M2, fast);
      break;
    default:
      break;
  }
  return (int)cycles;
}

static void shim_lzss_decompress(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  LzssDecompressRegs r;
  LzssWork work;
  lzss_decompress_wram_counted(w, rom, in->s, in->a, in->x, in->y, &r, &work);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.c;
  // The closing `PLD` restores the caller's page, so N and Z describe *that* and
  // not the count — the same trap every `PHD` routine in this file sets.
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
  R36_COSIM_COST(lzss_cycles(&work, in->fastrom));
}

static bool guard_lzss_decompress(Wram* scratch, const Rom* rom,
                                  const CosimRegs* in) {
  return lzss_decompress_supported(scratch, rom, in->s, in->a, in->x);
}

// ---------------------------------------------------------------------------
// $80:CDDA  lzss_read_byte / $80:CDEB  lzss_write_byte — its two leaves, which
// *can* be checked
// ---------------------------------------------------------------------------
//
// What stops `$80:CD20` being scored per call is that one call is frames long;
// these are eight instructions, so the interrupt problem inverts — a call
// is far too short for an NMI to land in, and the 1,071,108 of them across the
// corpus are **2.6% of every instruction the game executes**, which is more work
// than any single routine left on the ranking.
//
// They were the first pair of routines in the registry whose only caller was not
// in it, and now that it is they are worth more rather than less. Under `run`
// the body is substituted and these never execute, so their cost lives in
// `lzss_cycles` above as four constants — and `verify`, where the body is not
// substituted, still measures them against the ROM a million times a corpus.
// **That is the only part of a `run_only` model this project can check by
// measurement, and it happens to be the part that carries most of the total.**
//
// Both are `RTS` leaves that push nothing. Neither ends where it looks like it
// does: each closes on an `INC` of a direct-page pointer, so N and Z describe
// *the pointer*, not the byte. `port/lzss.h` has the detail.
//
// **Both are `verify_only`, and the volume that makes them worth having is
// exactly what stops them being substituted.** `cycles` is one number
// standing in for a range, and a million calls packed inside one multi-frame
// decompression do not let the error cancel: substituted, a level load lands
// three frames off and every input a movie applies by frame index afterwards
// moves with it. Tuning the budget cannot fix it -- the mean is already the
// mean, so what is left is variance and a constant has none -- and it was
// tried before being written down. See `CosimRoutine::verify_only`, which
// this is the second and quite different reason for.

static void shim_lzss_read_byte(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)in;
  LzssReadRegs r;
  lzss_read_byte_regs(w, rom, &r);
  out->a = r.a;
  // Neither index register is mentioned anywhere in the fifteen bytes.
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.spent;  // `CLC` on the way out with a byte, `SEC` when spent
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_lzss_write_byte(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  LzssWriteRegs r;
  lzss_write_byte_regs(w, in->a, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  // Nine bytes and not one of them touches carry, so it arrives back as it
  // came. Claiming that rather than omitting it is the point: `verify` then
  // checks the claim 697,920 times instead of ignoring the flag.
  out->c = in->c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A599  camera_split_x -- no arguments, and neither index touched
// ---------------------------------------------------------------------------
//
// `camera_split_y`'s counterpart, and four times the routine, because the
// tilemap is 64 columns stored as two 32x32 screens `$400` words apart: a row
// that crosses the seam is two transfers rather than one wrapped one. It hands
// back both of them, as `$5C`/`$5E`/`$60` and `$62`/`$64`/`$66`.
//
// Its two branches write those six words in opposite orders and it would be
// easy to publish two different flag expressions to match. They are the same
// one: both close on `LDA #$0042 : SEC : SBC <the run this branch measured>`,
// so A, N, Z and C agree even though the store underneath them does not.
static void shim_camera_split_x(Wram* w, const Rom* rom, const CosimRegs* in,
                                CosimRegs* out) {
  (void)rom;
  CameraSplitRegs r;
  camera_split_x(w, &r);
  out->a = r.a;
  out->x = in->x;
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:A68B / $A70A / $A789 / $A816 -- the four scroll routines
// ---------------------------------------------------------------------------
//
// The camera moves one pixel at a time and these are the four ways it can do
// it. Everything ported into `port/camera.h` before now exists to serve them,
// and they exist to serve `$80:A93F`, which is the 1.6% the chain was for.
//
// Two of the four are not in the listing as code at all -- `$80:A70A` and
// `$A816` are `.db` runs the tracer never proved were instructions -- and
// decoding them by hand is what shows they are their partners byte for byte
// with six substitutions. `port/camera.c` therefore has one X scroller and one
// Y scroller and a table of the six differences, which is the only way to write
// a mirror down such that the mirroring is checkable.
//
// **All four take carry as an input**, which nothing else in this registry
// does, and one exit is why: `$80:A68B` and `$A816` open `LDA $1B6A : BEQ out`
// with no `CMP` anywhere on that path, so a camera already against the near
// edge of the map returns the caller's carry untouched. The forward pair's
// `CMP $B8` overwrites it before anything can observe it, so they are handed it
// and ignore it -- and are handed it anyway, because a shim that passed
// `false` would be asserting something about the caller instead of about the
// routine.
//
// On the exits that do reach a strip, the three registers come from three
// places again: A, N and Z from `$80:9E6D`, the last call any of them makes; X
// from `STX $CE`, so it is the VRAM queue's new length; and Y from the `TAY`
// that indexed the destination table, so it is the tilemap cursor doubled.
// Carry belongs to the `ADC $1B7E` that built the last destination.
static bool guard_camera_scroll_left(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  (void)rom;
  (void)in;
  if (camera_scroll_left_supported(scratch)) return true;
  cosim_census_note("tilemap arena exhausted", CAMERA_SCROLL_LEFT_ENTRY);
  return false;
}

static bool guard_camera_scroll_right(Wram* scratch, const Rom* rom,
                                      const CosimRegs* in) {
  (void)rom;
  (void)in;
  if (camera_scroll_right_supported(scratch)) return true;
  cosim_census_note("tilemap arena exhausted", CAMERA_SCROLL_RIGHT_ENTRY);
  return false;
}

static bool guard_camera_scroll_down(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  (void)rom;
  (void)in;
  if (camera_scroll_down_supported(scratch)) return true;
  cosim_census_note("tilemap arena exhausted", CAMERA_SCROLL_DOWN_ENTRY);
  return false;
}

static bool guard_camera_scroll_up(Wram* scratch, const Rom* rom,
                                   const CosimRegs* in) {
  (void)rom;
  (void)in;
  if (camera_scroll_up_supported(scratch)) return true;
  cosim_census_note("tilemap arena exhausted", CAMERA_SCROLL_UP_ENTRY);
  return false;
}

static void publish_camera_scroll(const CameraScrollRegs* r, CosimRegs* out) {
  out->a = r->a;
  out->x = r->x;
  out->y = r->y;
  out->n = r->n;
  out->z = r->z;
  out->c = r->c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_camera_scroll_left(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  CameraScrollIn args = {in->x, in->y, in->c};
  CameraScrollRegs r;
  camera_scroll_left(w, rom, &args, &r);
  publish_camera_scroll(&r, out);
}

static void shim_camera_scroll_right(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  CameraScrollIn args = {in->x, in->y, in->c};
  CameraScrollRegs r;
  camera_scroll_right(w, rom, &args, &r);
  publish_camera_scroll(&r, out);
}

static void shim_camera_scroll_down(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  CameraScrollIn args = {in->x, in->y, in->c};
  CameraScrollRegs r;
  camera_scroll_down(w, rom, &args, &r);
  publish_camera_scroll(&r, out);
}

static void shim_camera_scroll_up(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  CameraScrollIn args = {in->x, in->y, in->c};
  CameraScrollRegs r;
  camera_scroll_up(w, rom, &args, &r);
  publish_camera_scroll(&r, out);
}

// ---------------------------------------------------------------------------
// $80:A93F  camera_follow -- no arguments; the camera one pixel further on
// ---------------------------------------------------------------------------
//
// The top of the camera chain and the whole reason for reading it bottom-up:
// eleven routines had to go in before this one could, because a substituted
// routine has to do everything the ROM's does and there is no way to call back
// into the ROM half-way through.
//
// It picks the point the view should centre on -- one player, the other, or the
// midpoint -- and then moves the camera **one pixel** towards it per axis. The
// delta is computed in full and then only its sign is used, by an `ASL A` whose
// result is discarded and whose carry is the answer. That is why the view
// drifts after the players rather than snapping to them.
//
// A `PHD` routine, so N and Z are the caller's direct page, and every one of
// its exits is `PLD : SEC : RTL`, so **carry is set on all four and says
// nothing**. The three register outputs are worth stating because two of them
// are leftovers: A is the Y delta, or zero on the exits that never compute one;
// X is that delta unless a scroll routine overwrote it; and Y is `$0006`, the
// index the record read left behind, unless one did.
//
// The guard is the four scroll routines' arena guard asked once for both of
// them: a call can reach one X scroll and one Y scroll, the second buys its
// strip out of what the first left, so the question has to be asked about the
// sum rather than about either.
static bool guard_camera_follow(Wram* scratch, const Rom* rom,
                                const CosimRegs* in) {
  (void)rom;
  (void)in;
  if (camera_follow_supported(scratch)) return true;
  cosim_census_note("tilemap arena exhausted", CAMERA_FOLLOW_ENTRY);
  return false;
}

static void shim_camera_follow(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  CameraFollowRegs r;
  camera_follow(w, rom, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = (in->d & 0x8000u) != 0;  // the closing `PLD`
  out->z = in->d == 0;
  out->c = true;  // ...and the `SEC` under it, on every exit
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B379  actor_aligned -- X, Y = a point; A = which way something is
// ---------------------------------------------------------------------------
//
// `actor_nearest`'s sibling, called by the same enemy body and answering the
// other half of its question: not *who is closest* but *is anything lined up
// with me right now*. It returns a doubled direction index, or zero.
//
// A `PHD` routine like `camera_follow`, so N and Z are the caller's direct page
// and neither means anything about the search -- the ROM's own caller does
// `TAX : BEQ` to get the answer's Z back. Carry is the leftover of whichever
// `SBC` picked the direction, and is clear on the no-match exit because a `CPX`
// ended the loop there. X is the record that matched, or `$184A`, which is the
// loop counter one stride below the table rather than a pointer to anything.
// Y is the argument, untouched.
//
// No guard: it reads 32 fixed records out of WRAM and cannot fail.
static void shim_actor_aligned(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  (void)rom;
  ActorAlignedRegs r;
  actor_aligned(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = in->y;
  out->n = (in->d & 0x8000u) != 0;  // the closing `PLD`
  out->z = in->d == 0;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:BF1B  actor_notify_box -- no arguments; everything in a rectangle told
// ---------------------------------------------------------------------------
//
// The blast radius. `actor_overlap_pass` asks who is touching whom; this is one
// actor asking who is inside a box and telling all of them, and it is how every
// attack that is not a contact hit reaches its victims.
//
// It dispatches through `thread_call_handler`, so it inherits the decline that
// `actor_collide_notify` and `sprite_build_oam` already have: **false means some
// actor in the box has a handler the port does not**, and the harness gives the
// whole call back to the ROM. A decline may leave `w` partly written, because
// the records before it in the walk have been told and the ROM would have told
// them too.
//
// Every register is claimed. They are all leftovers of the last record the walk
// looked at, which is the sort of thing this project usually declines to claim
// -- but four of the five call sites read return immediately, so "it is dead"
// would be a guess about the caller's caller rather than a fact, and 9,784
// calls is enough for the harness to settle it either way.
static bool guard_actor_notify_box(Wram* scratch, const Rom* rom,
                                   const CosimRegs* in) {
  ThreadCallResult tail = {.c = in->c};
  ActorNotifyRegs r;
  return actor_notify_box(scratch, rom, in->a, in->c, &tail, &r);
}

// What each run of `$80:BF1B` costs, indexed by `ActorNotifyBlock`. The whole
// routine is 76 bytes of listing and 14 direct-page instructions, and every
// block below is a straight run between two branches of it.
//
// The seven per-record blocks are nested prefixes of one another — each is the
// one above it plus the instructions that ask the next question — which is why
// they climb 92, 132, 206, 246, 320, 360, 572 rather than being independent
// numbers. A transcription error in any one of them therefore shows up as a
// broken spacing, and the spacings are **40, 74, 40, 74, 40** and then the
// dispatch preamble's 212.
//
// The alternation is not decoration. A test that can reuse what is already in A
// costs `CMP` + the branch's six = 40; one that has to fetch a fresh word from
// the record first costs `LDA $xx,X` on top of that = 74. So the id test and
// the two *upper* bounds are 40, and the two *lower* bounds — which open each
// axis — are 74. Any table where those five numbers are not in that order has
// an instruction in the wrong block.
//
// `NOTIFY_BLK_HIT` carries `JSL $808480`'s own 54 and not a cycle of what it
// dispatches into: that is `thread_call_cycles`, summed per hit below, exactly
// as `OVL_HIT` carries `JSR $BE8F` and leaves the rest to `notify_cycles`.
static const CosimRun NOTIFY_BOX_COST[NOTIFY_BLOCK_COUNT] = {
    [NOTIFY_BLK_PROLOGUE] = {28 + 34 + 34 + 18, 8},
    [NOTIFY_BLK_BOUND_KEPT] = {34 + 18 + 12 + 12, 6, 1},
    [NOTIFY_BLK_BOUND_CLAMPED] = {34 + 12 + 34 + 12 + 12, 8, 2},
    [NOTIFY_BLK_BOUND_NEXT] = {18, 2},
    [NOTIFY_BLK_BOUND_DONE] = {12, 2},
    // `LDY $9C : BEQ` taken, and the shared `PLD : RTL` at `$80:BF65`.
    [NOTIFY_BLK_NO_ACTORS] = {28 + 18 + 34 + 42, 6, 1},
    // ...not taken, then `DEY DEY : BEQ` taken into the same two instructions.
    [NOTIFY_BLK_ONE_ACTOR] = {28 + 12 + 12 + 12 + 18 + 34 + 42, 10, 1},
    // ...and not taken either, which is the walk.
    [NOTIFY_BLK_WALK] = {28 + 12 + 12 + 12 + 12, 8, 1},
    // $80:BF35 LDX $137E,Y : LDA $0E,X : BEQ taken.
    [NOTIFY_BLK_NO_ID] = {40 + 34 + 18, 7, 1},
    // ...not taken, then `CMP $40 : BEQ` taken.
    [NOTIFY_BLK_SELF_ID] = {40 + 34 + 12 + 28 + 18, 11, 2},
    // ...not taken, then `LDA $02,X : CMP $38 : BCC` taken.
    [NOTIFY_BLK_LEFT_OF] = {40 + 34 + 12 + 28 + 12 + 34 + 28 + 18, 17, 4},
    // ...not taken, then `CMP $3A : BCS` taken.
    [NOTIFY_BLK_RIGHT_OF] = {40 + 34 + 12 + 28 + 12 + 34 + 28 + 12 + 28 + 18,
                             21, 5},
    // ...not taken, then `LDA $06,X : CMP $3C : BCC` taken.
    [NOTIFY_BLK_ABOVE] = {40 + 34 + 12 + 28 + 12 + 34 + 28 + 12 + 28 + 12 + 34 +
                              28 + 18,
                          27, 7},
    // ...not taken, then `CMP $3E : BCS` taken.
    [NOTIFY_BLK_BELOW] = {40 + 34 + 12 + 28 + 12 + 34 + 28 + 12 + 28 + 12 + 34 +
                              28 + 12 + 28 + 18,
                          31, 8},
    // ...not taken: inside the box. `PHY : LDA $0C,X : STX $78 : TAX :
    // LDY $40 : JSL $808480 : PLY`.
    [NOTIFY_BLK_HIT] = {40 + 34 + 12 + 28 + 12 + 34 + 28 + 12 + 28 + 12 + 34 +
                            28 + 12 + 28 + 12 + 28 + 34 + 28 + 12 + 28 + 54 +
                            34,
                        44, 11},
    // $80:BF61 DEY DEY : BPL taken.
    [NOTIFY_BLK_LOOP_NEXT] = {12 + 12 + 18, 4},
    // ...not taken, and the `PLD : RTL` the early exits share.
    [NOTIFY_BLK_LOOP_DONE] = {12 + 12 + 12 + 34 + 42, 6},
};

// False when a hit in this box entered a handler with no cost table, or when
// there were more hits than the array can describe — the same all-or-nothing
// `overlap_cycles` makes, for the same reason.
static bool notify_box_cycles(const ActorNotifyWork* k, const CosimRegs* in,
                              int* out) {
  bool fast = in->fastrom;
  // The routine forces its own page to zero at `$80:BF1D PEA $0000 : PLD`
  // before it touches a direct-page word, so the caller's `D` cannot make any
  // of the fourteen cost an extra idle — which is why, alone among the models
  // here, this one takes no `dp_unaligned` and its shims check no `in->d`.
  int cycles = 0;
  for (int i = 0; i < NOTIFY_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&NOTIFY_BOX_COST[i], fast);

  if (k->hits > NOTIFY_BOX_MAX_PRICED_HITS) return false;
  for (int i = 0; i < k->hits; i++) {
    int call;
    if (!thread_call_cycles(&k->call[i], in, &call)) return false;
    cycles += call;
  }
  *out = cycles;
  return true;
}

static void shim_actor_notify_box(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  ThreadCallResult tail = {.c = in->c};
  ActorNotifyRegs r;
  ActorNotifyWork work;
  actor_notify_box_counted(w, rom, in->a, in->c, &tail, &r, &work);

  if (R36_COSTS_NEEDED()) {
    int cycles;
    if (notify_box_cycles(&work, in, &cycles)) R36_COSIM_COST(cycles);
  }

  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.c;
  out->n = (in->d & 0x8000u) != 0;  // the closing `PLD`
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:B3F1  actor_snap_to -- X, Y = two records; X moved onto Y if it is close
// ---------------------------------------------------------------------------
//
// `actor_aligned` finds something lined up to within a tile; this closes the
// last pixel of it, per axis, and it is the one routine in the registry with no
// `PHD` whose flags are therefore its own. The X axis runs first and everything
// it leaves is overwritten by the Y axis, so what the caller gets back describes
// Y alone: carry **set** means Y did not snap.
static void shim_actor_snap_to(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  (void)rom;
  ActorSnapRegs r;
  actor_snap_to(w, in->x, in->y, &r);
  out->a = r.a;
  out->x = in->x;  // both are indices; neither is written
  out->y = in->y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $82:8014 / $82:8069  boss_bg_queue -- no arguments; twenty DMA jobs
// ---------------------------------------------------------------------------
//
// The big-figure blitter, and a pair in the same shape as the four scroll
// routines: two entries that differ in one thing, here whether the figure is
// mirrored on the way to VRAM.
//
// Neither takes a register argument -- everything comes out of direct page,
// which they force to zero themselves -- and neither *produces* one either.
// Both end on `LDA #$81C9 : LDY #$0082 : JSL $8083AE : PLD : RTL`, so all
// three registers and the carry belong to `vbl_queue_a_add` and say only
// whether the vblank queue had room, and N and Z are the caller's direct page
// off the closing `PLD`. That leaves the entire result of a 2,746-instruction
// routine in WRAM, which is the easiest kind of routine to check and the
// reason these two passed first run.
//
// The guards differ, because what the two routines read differs. The plain one
// only ever reads the four header bytes; the mirrored one reads all 560 and
// stages a flipped copy, so its guard has to ask about the whole figure.
static bool guard_boss_bg_queue(Wram* scratch, const Rom* rom,
                                const CosimRegs* in) {
  (void)in;
  if (boss_bg_queue_supported(scratch, rom)) return true;
  cosim_census_note("figure header unreadable", BOSS_BG_QUEUE_ENTRY);
  return false;
}

static bool guard_boss_bg_queue_flip(Wram* scratch, const Rom* rom,
                                     const CosimRegs* in) {
  (void)in;
  if (boss_bg_queue_flip_supported(scratch, rom)) return true;
  cosim_census_note("figure unreadable", BOSS_BG_QUEUE_FLIP_ENTRY);
  return false;
}

static void boss_bg_out(const BossBgRegs* r, const CosimRegs* in,
                        CosimRegs* out) {
  out->a = r->a;
  out->x = r->x;
  out->y = r->y;
  out->n = (in->d & 0x8000u) != 0;  // the closing `PLD`
  out->z = in->d == 0;
  out->c = r->c;  // ...and `vbl_queue_a_add`'s own `CPY #$0010`, untouched
                  // by everything between it and the `RTL`
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_boss_bg_queue(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  BossBgRegs r;
  boss_bg_queue(w, rom, &r);
  boss_bg_out(&r, in, out);
}

static void shim_boss_bg_queue_flip(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  BossBgRegs r;
  boss_bg_queue_flip(w, rom, &r);
  boss_bg_out(&r, in, out);
}

// ---------------------------------------------------------------------------
// $82:8F93  boss_step — the direction in `$16`, `#$6969` in A for double speed
// ---------------------------------------------------------------------------
//
// A `JSR` from the boss thread, so the direct page is the caller's and the shim
// hands it over rather than assuming — even though every call site reaches the
// figure's position at a fixed `$1E62` and the thread's `D` has been zero every
// time the harness has looked.
//
// N and Z are the exit compare's and A is not: `CMP` does not write the
// accumulator, so the routine returns the coordinate it loaded while the flags
// describe the difference. X and Y are `terrain_blocked_wide`'s leftovers from
// the last probe the pass loop ran, and there is always one — the loop tests
// its counter at the bottom.
//
// **Thirteen blocks of arithmetic wrapped round somebody else's table.** The
// probes are `terrain_blocked_wide`'s and are priced by `wide_cycles` above;
// what is here is the two table lookups, the pass loop and the exit compare.
// The `JSR`s are in `PASS_HEAD` and `LEAD_CLEAR` and the `RTS`s that match them
// are in the leaf's exit blocks, so the seam is paid for exactly once from each
// side.
//
// This routine does **not** force `D` — no `PHD`, no `TCD`, fifteen direct-page
// instructions on whatever page the boss thread was running. So unlike the leaf
// it calls, the `dp` column here is a real cost and not a cross-check, and the
// shim passes `in->d` for both the arithmetic and the answer.
static const CosimRun BOSS_STEP_COST[BOSS_STEP_BLOCK_COUNT] = {
    // `LDX $16 : CMP #$6969 : BNE` taken.
    [BOSS_BLK_SINGLE] = {28 + 18 + 18, 7, 1},  // 64
    // ...or not taken, and `DEC $2C` is a direct-page read-modify-write at 50,
    // which is most of what the double step costs over the single one.
    [BOSS_BLK_DOUBLE] = {28 + 18 + 12 + 12 + 12 + 18 + 12 + 50, 15, 2},  // 162
    // $82:8FA2-$82:8FD4. Four long reads out of `$82:906F` at 36 apiece, the
    // two absolute `$1E62`/`$1E64` loads at 34, and eight direct-page stores.
    [BOSS_BLK_SETUP] = {34 + 28 + 34 + 28 + 36 + 12 + 34 + 28 + 36 + 12 + 34 +
                            28 + 36 + 28 + 36 + 28 + 28 + 12 + 12 + 12 + 36 +
                            28,
                        62, 8},  // 600
    // $82:8FD6-$82:8FEC, ending on the `JSR`: the probe index, both offsets
    // added to the candidate, and the call.
    [BOSS_BLK_PASS_HEAD] = {28 + 12 + 28 + 28 + 12 + 36 + 12 + 28 + 12 + 36 +
                                12 + 28 + 12 + 40,
                            29, 5},  // 324
    [BOSS_BLK_LEAD_BLOCKED] = {18, 2, 0},
    // The same shape again for the trailing probe, under a `BCS` not taken.
    [BOSS_BLK_LEAD_CLEAR] = {12 + 28 + 36 + 12 + 28 + 12 + 36 + 12 + 28 + 12 +
                                 40,
                             25, 3},  // 256
    [BOSS_BLK_TRAIL_BLOCKED] = {18, 2, 0},
    // `BCS` not taken, `LDA $0A : AND #$0008 : BEQ` not taken, the store, and
    // the `BRA` over the other half.
    [BOSS_BLK_COMMIT_X] = {12 + 28 + 18 + 12 + 28 + 34 + 18, 16, 2},  // 150
    // ...or the `BEQ` taken, which is cheaper by exactly the `BRA` it lands
    // past: 150 - 138 = 12, and 12 is a branch not taken.
    [BOSS_BLK_COMMIT_Y] = {12 + 28 + 18 + 18 + 28 + 34, 14, 2},  // 138
    // `LDA $18 : SEC : SBC #$0008 : STA $18 : BPL`.
    [BOSS_BLK_LOOP_NEXT] = {28 + 12 + 18 + 28 + 18, 10, 2},  // 104
    [BOSS_BLK_LOOP_DONE] = {28 + 12 + 18 + 28 + 12, 10, 2},  // 98
    // `LDA $1E62 : CMP $10 : BNE` taken, then `CLC : RTS`.
    [BOSS_BLK_EXIT_X] = {34 + 28 + 18 + 12 + 40, 9, 1},  // 132
    [BOSS_BLK_EXIT_Y] = {34 + 28 + 12 + 34 + 28 + 18 + 12 + 40, 16, 2},  // 206
    // ...and neither, which is the same run of instructions with `SEC` in
    // place of `CLC` and the second `BNE` not taken: 206 - 200 = 6.
    [BOSS_BLK_EXIT_STUCK] = {34 + 28 + 12 + 34 + 28 + 12 + 12 + 40, 16, 2},
};

// **2,076 and 15,616 are this routine's registry bounds, and the table
// reproduces both — but only the second one the way it was expected to.**
//
// The ceiling is a diagonal that runs both passes and finds all four probes
// clear: 162 + 600 + 2x(324 + 256) + 150 + 138 + 104 + 98 + 132 = 2,544 of
// arithmetic, plus four ceiling probes at 3,148 = 15,136. The measured 15,616
// is 480 more, which is twelve refreshes over a call eleven scanlines long.
//
// The floor is the interesting one. The cheapest call the table can build is a
// *single* step whose first probe is already in terrain — 64 + 600 + 324 + 634
// + 18 + 98 + 200 = 1,938 — and 2,076 is 138 more, which is not a multiple of
// 40 and so cannot be refresh alone. Swap `SINGLE` for `DOUBLE` and it is
// 2,036, and 2,076 - 2,036 = 40 exactly.
//
// **So the cheapest call ever measured is a double step, and it is not because
// single steps are rare.** `boss_step_single` is taken 247 times in
// `level25-boss.zmv` alone. What has never happened is the two cheap things at
// once: a single step whose very first probe is refused on the tile number.
// Each half is ordinary, the combination is not, and a model built out of
// blocks says so where a fitted constant could not — 1,938 is a price this
// table can quote for a call the game has never made.
static int boss_step_cycles(const BossStepWork* k, bool fast,
                            bool dp_unaligned) {
  int cycles = wide_cycles(&k->probes, fast);
  for (int i = 0; i < BOSS_STEP_BLOCK_COUNT; i++)
    cycles +=
        k->blocks[i] * cosim_run_cycles_dp(&BOSS_STEP_COST[i], fast,
                                           dp_unaligned);
  return cycles;
}

static void shim_boss_step(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  BossStepRegs r;
  BossStepWork work;
  boss_step_counted(w, rom, in->d, in->a, &r, &work);
  R36_COSIM_COST(boss_step_cycles(&work, in->fastrom, (in->d & 0xff) != 0));
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $82:9265, $82:92D6  boss_place_parts, boss_stomp — the same two words again
// ---------------------------------------------------------------------------
//
// `boss_step` moves `$1E62`/`$1E64`; these two are what the same thread does
// with them on the next two instructions. Neither takes a register argument.
//
// `boss_place_parts` reads four record pointers off the direct page and one
// mirror flag, and writes eight words. Its V is that of the `ADC` its flags
// come from and is not claimed — the caller's next instruction is another
// `JSR`, and no exit of this routine has ever had a reader for it.
static void shim_boss_place_parts(Wram* w, const Rom* rom, const CosimRegs* in,
                                  CosimRegs* out) {
  BossPartsRegs r;
  boss_place_parts(w, rom, in->d, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// `boss_stomp` is five stores and a `JSL`, so it inherits `actor_notify_box`'s
// decline whole — including the part where a declined call may already have
// told some of the actors in the box. The guard has to build the box before it
// can ask, because what the walk finds is what decides the answer; it does that
// to the harness's scratch copy, which is thrown away either way.
//
// **Neither `in->a` nor `in->c` is passed through**, and that is the whole of
// what this shim gets wrong if it is written the obvious way: the `JSL` hands
// `actor_notify_box` an A and a carry that `$82:92D6` made itself, three and
// two instructions earlier. Passing the caller's A instead fails on call 1 of
// `level25-lane` with `A: ROM $000A, port $021D` — `$000A` being the id the
// routine had just loaded. See `port/boss.h`.
static bool guard_boss_stomp(Wram* scratch, const Rom* rom,
                             const CosimRegs* in) {
  (void)in;
  return boss_stomp_supported(scratch, rom);
}

// Everything `$82:92D6` does that is not `actor_notify_box`: `$82:92D6`
// through `$82:92FB STA $0040` is 376 over 40 bytes, then `JSL $80BF1B` at 54
// and the `RTS` at 40. **All five stores are absolute** — `STA $0038` and not
// `STA $38` — because the routine that reads them forces `D` to zero itself, so
// there is no direct-page column here and none is owed.
//
// **470 + `actor_notify_box`'s own floor of 642 is 1,112, and 1,112 is the
// number already written in this routine's registry entry below** — recorded as
// its measured minimum over 6,549 calls, rounds before either half of that sum
// existed. Neither figure was fitted to the other: one is the corpus reporting
// the cheapest call it ever saw, the other is two listings added up. The
// cheapest call is a boss standing on a board holding one visible actor, and it
// costs 1,112 cycles with no refresh in it at all.
static const CosimRun BOSS_STOMP_COST = {376 + 54 + 40, 45};

static void shim_boss_stomp(Wram* w, const Rom* rom, const CosimRegs* in,
                            CosimRegs* out) {
  BossStompRegs r;
  ActorNotifyWork work;
  boss_stomp_counted(w, rom, &r, &work);

  int cycles;
  if (notify_box_cycles(&work, in, &cycles))
    R36_COSIM_COST(cycles + cosim_run_cycles(&BOSS_STOMP_COST, in->fastrom));

  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.c;
  // The `PLD` inside `actor_notify_box`, restoring the page this routine was
  // called on — so two of the three flags describe the boss thread and not the
  // box.
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// $80:ACF6  blockmap_cell_ptr — X = column, Y = row
// ---------------------------------------------------------------------------
//
// No `PHD`, so the direct page is the caller's and the shim hands it over. Ten
// call sites in four banks reach it and this registry has one of their callers,
// so it will be a both-sides row for a while yet.
static void shim_blockmap_cell_ptr(Wram* w, const Rom* rom, const CosimRegs* in,
                                   CosimRegs* out) {
  (void)rom;
  BlockCellRegs r;
  blockmap_cell_ptr(w, in->d, in->x, in->y, &r);
  out->a = r.a;
  out->x = r.x;
  out->y = in->y;  // never touched
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static bool guard_blockmap_cell_ptr(Wram* scratch, const Rom* rom,
                                    const CosimRegs* in) {
  (void)rom;
  return blockmap_cell_ptr_supported(scratch, in->d);
}

// ---------------------------------------------------------------------------
// $80:AD2B  blockmap_expand — ten frames, and the second routine `verify`
// structurally cannot check
// ---------------------------------------------------------------------------
//
// This entry stood as a comment saying the routine could not be registered, for
// the same reason `$80:C05A` did: **one call is about ten frames long, so
// `verify` reported one call, one interruption and nothing checked** on every
// movie tried — `level1`, `level1-rescue`, `level9`, `level25-boss` and
// `level53` alike. There was no movie on which a call completed inside a frame
// and there cannot be.
//
// That is a statement about rewind-and-replay and not about substitution, which
// is the distinction `CosimRoutine::run_only` exists to make. Under `run` the
// ROM never executes any of it, so there is no call window for an NMI to land
// inside; what checks the port is `run`'s comparison of all 128 KB at the next
// scheduler pass, and every pass after it for the rest of the movie. The map
// this builds is the map the whole level is played on, so a port that got one
// cell wrong would be visible in the very next frame the camera drew.
//
// **It is also what the parked burn was built for.** Ten frames is ten NMIs, and
// the core latches one — see `CosimBurn` in `cosim.c`. `$80:C05A` fits inside a
// frame and so never needed it; this does not, and `run` reports the parks.
//
// ## Why the cost is a count and not a mean
//
// `run_only` requires it — a mean is something `verify` watched, and `verify`
// has never seen this routine finish. It is available here because **no branch
// in the routine depends on the data it reads.** The nest is
// `$B0` rows x `$AE >> 1` cells x 8 block rows x 8 words, and every trip count
// falls out of the level record. So the port counts the four loops and the two
// bank questions (`BlockExpandWork`) and this prices them, exactly as a hot
// dispatcher's branch outcomes are priced — the difference being that here the
// answer is exact rather than an average.
//
// Level 1 is 22 x 13 = **286 cells, which is exactly the 286 calls `verify`
// measures `blockmap_cell_ptr` taking** on `level1.zmv`: the shape of this model
// is confirmed by a count the harness makes independently of it. And that
// routine's own measured floor, 334 master cycles, is to the cycle what
// `tools/cycles816.py` prices its 21 bytes at — so the same tool that priced the
// rest of this table is checked against a real measurement inside this very
// call. Level 1 comes out at 3,604,294 cycles, 10.09 frames.
//
// ## ...and it was checked against the ROM anyway
//
// `verify` cannot score this call, but that is a limit of rewind-and-replay and
// not of the clock, so the ROM's own execution was timed directly: the cycles
// spent with the program counter inside `$80:ACF6..$80:AD91` on `level1.zmv`,
// with the eleven interrupts that land in the middle attributed to the handler
// where they belong. **3,713,172 cycles**, less the 2,722 DRAM refreshes the
// core adds at 40 apiece, is **3,604,292** — two cycles under this model, on a
// figure of three and a half million.
//
// Two is not zero, and the difference is the probe's rather than the model's: a
// `snes_runCycle` granule is two master cycles, so attributing a step to one
// side or the other of the boundary is worth exactly this much. It is stated as
// a measurement and not as the refresh-exact residual the per-call models are
// held to, because that standard needs `verify`, and `verify` is the thing this
// routine cannot have.
//
// ## The blocks
//
// Two of them come in pairs because the block map's bank is level data — `$A8`
// out of the level record, WRAM on some levels and cartridge on others — and a
// cartridge operand byte costs 6 rather than 8 and moves into the FastROM
// column. The block *library* is `$7E` at every call site the ROM has, but the
// port checks rather than trusting it, so the copy loop carries the same pair;
// `words_rom` is expected to stay zero and the report will say so if it does not.
static const CosimRun BLOCK_PROLOGUE = {208, 13};   // $AD2B..$AD37, once
static const CosimRun BLOCK_ROW_HEAD = {68, 5};     // $AD38..$AD3C, per row
static const CosimRun BLOCK_ROW_TAIL = {140, 8};    // $AD88..$AD8F, BNE not taken
static const CosimRun BLOCK_CELL_WRAM = {514, 40};  // $AD3D..$AD64
static const CosimRun BLOCK_CELL_ROM = {510, 42};   // ...map in the cartridge
static const CosimRun BLOCK_TILE_ADDR = {250, 15};  // $80:AD1C, called per cell
static const CosimRun BLOCK_CELL_PTR = {334, 21};   // $80:ACF6, likewise
static const CosimRun BLOCK_LIB_PTR = {250, 17};    // $80:AD0B, likewise
static const CosimRun BLOCK_ROW_START = {18, 3};    // $AD65..$AD67, per block row
static const CosimRun BLOCK_WORD_WRAM = {140, 8};   // $AD68..$AD6F, BPL not taken
// The same eight bytes with the library in the cartridge: the two operand bytes
// of `LDA [$28],Y` cost 6 rather than 8, and they join the byte column. Derived
// rather than measured, and the pair above is the check on the derivation —
// `cycles816.py` prices the cell head at exactly this difference, -4 and +2.
static const CosimRun BLOCK_WORD_ROM = {136, 10};
static const CosimRun BLOCK_ROW_ADVANCE = {206, 18};  // $AD70..$AD81, BNE not taken
static const CosimRun BLOCK_CELL_TAIL = {112, 6};     // $AD82..$AD87, BNE not taken
static const CosimRun BLOCK_EPILOGUE = {76, 2};       // $AD90..$AD91, the RTL too

// A branch this routine takes rather than falls through. Every one of the four
// is a `DEC`/`DEY` loop closing, so how often each is taken follows from the
// trip counts and is not counted separately — see `BlockExpandWork`.
#define BLOCK_TAKEN 6

static int blockmap_cycles(const BlockExpandWork* k, bool fast) {
  const uint32_t cells_wram = k->cells - k->cells_rom;
  const uint32_t words_wram = k->words - k->words_rom;
  long cycles = cosim_run_cycles(&BLOCK_PROLOGUE, fast) +
                cosim_run_cycles(&BLOCK_EPILOGUE, fast);

  cycles += (long)k->rows * (cosim_run_cycles(&BLOCK_ROW_HEAD, fast) +
                             cosim_run_cycles(&BLOCK_ROW_TAIL, fast));
  cycles += (long)k->cells * (cosim_run_cycles(&BLOCK_TILE_ADDR, fast) +
                              cosim_run_cycles(&BLOCK_CELL_PTR, fast) +
                              cosim_run_cycles(&BLOCK_LIB_PTR, fast) +
                              cosim_run_cycles(&BLOCK_CELL_TAIL, fast));
  cycles += (long)cells_wram * cosim_run_cycles(&BLOCK_CELL_WRAM, fast);
  cycles += (long)k->cells_rom * cosim_run_cycles(&BLOCK_CELL_ROM, fast);
  cycles += (long)k->block_rows * (cosim_run_cycles(&BLOCK_ROW_START, fast) +
                                   cosim_run_cycles(&BLOCK_ROW_ADVANCE, fast));
  cycles += (long)words_wram * cosim_run_cycles(&BLOCK_WORD_WRAM, fast);
  cycles += (long)k->words_rom * cosim_run_cycles(&BLOCK_WORD_ROM, fast);

  // ...and the four loop-backs. Each loop takes its branch on every trip but
  // the one that ends it, so the count is the difference between the trips and
  // the trips of the loop outside it — and the outermost is `rows - 1`.
  cycles += (long)BLOCK_TAKEN *
            ((long)(k->words - k->block_rows) +
             (long)(k->block_rows - k->cells) + (long)(k->cells - k->rows) +
             (long)(k->rows ? k->rows - 1 : 0));
  return (int)cycles;
}

static void shim_blockmap_expand(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  BlockExpandRegs r;
  BlockExpandWork work;
  blockmap_expand_counted(w, rom, &r, &work);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->c = r.c;
  // The closing `PLD` restores the caller's page, so N and Z describe *that*
  // and not the count — the same trap every `PHD` routine in this file sets.
  out->n = (in->d & 0x8000u) != 0;
  out->z = in->d == 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
  R36_COSIM_COST(blockmap_cycles(&work, in->fastrom));
}

static bool guard_blockmap_expand(Wram* scratch, const Rom* rom,
                                  const CosimRegs* in) {
  (void)in;
  return blockmap_expand_supported(scratch, rom);
}

// ---------------------------------------------------------------------------
// $80:C07F, $80:C0A3, $80:C139  the status panel
// ---------------------------------------------------------------------------

// Three entries for a tree of twenty routines, and the reason there are three
// rather than one is that the two panels have callers `$80:C07F` does not.
// `$80:C1D1`, `$80:C1F1`, `$80:C1F5` and `$80:C215` — the screen transitions —
// reach them directly, so registering only the top would leave those four sites
// running the ROM and checking nothing.
//
// All three take the caller's direct page, because `$1E`, `$20`, `$22` and `$24`
// are scratch on whatever thread's page is current. `$80:C07F` is a `JSL`
// target from three banks; the two panels are `JSR`ed from bank `$80` only.
//
// Carry is claimed on all three, and it is the flag that took the work. Nothing
// in the cluster returns anything in it, but there is no path through a panel
// that leaves it alone: a `CMP` against a shadow sets it six times over, and
// below that so do the shifts that build a table index and the `ROR $1E` inside
// every printed digit. See `port/hud.h`.
// What each straight-line run of the HUD tree costs the 65816, indexed by
// `HudBlock`. Every number came out of `tools/cycles816.py`, which prices the
// listing rather than being told what it costs; the whole table is checked
// against the ROM on every call, and the cost-model report says so.
//
// The tree is sixteen routines but only five shapes of cost: the refresh's own
// two branches, the panel's six comparisons, three adapters that are three
// instructions each, four drawing routines whose loops are all counted rather
// than terminated, and one digit — which is where the variation actually lives,
// eleven of them per full redraw at three different prices.
static const CosimRun HUD_BLOCK_COST[HUD_BLOCK_COUNT] = {
    // $80:C07F. P1 costs the extra `BRA $C090` on the way back from the panel;
    // P2 costs the taken `BNE` instead. Both include the `JSR`.
    [HUD_BLK_REFRESH_P1] = {28 + 18 + 28 + 12 + 40 + 18, 14},
    [HUD_BLK_REFRESH_P2] = {28 + 18 + 28 + 18 + 40, 12},
    [HUD_BLK_REFRESH_IDLE] = {34 + 18 + 42, 6},
    [HUD_BLK_REFRESH_QUEUE] = {34 + 12 + 34 + 18 + 18 + 24, 18},
    // $80:83AE, tail-jumped into. `ACCEPTED` is the prologue, the compare that
    // found a free slot and the whole of the store; `BUSY` is one turn of the
    // search that did not; `FELL` is all fifteen turns and no free slot, which
    // ends on a `BNE` that falls through rather than a `BEQ` that is taken.
    [HUD_BLK_QUEUE_FULL] = {28 + 34 + 18 + 18 + 34 + 42, 11},
    [HUD_BLK_QUEUE_ACCEPTED] = {110 + 58 + 248, 12 + 5 + 14},
    [HUD_BLK_QUEUE_BUSY] = {40 + 12 + 48 + 18, 11},
    [HUD_BLK_QUEUE_FELL] = {110 + 13 * 118 + 112 + 248, 12 + 13 * 11 + 11 + 14},
    // $80:C0A3 and $80:C139, which are the same code and so the same price.
    [HUD_BLK_PANEL_OFF] = {34 + 12 + 40, 6},
    [HUD_BLK_PANEL_ON] = {34 + 18, 5},
    [HUD_BLK_PANEL_RTS] = {40, 1},
    // `LDA : CMP long : BEQ`, then the store, the `JSR` and the `INC $1E7A`.
    [HUD_BLK_FIELD_SAME] = {34 + 40 + 18, 9},
    [HUD_BLK_FIELD_CHANGED] = {34 + 40 + 12 + 40 + 40 + 56, 19},
    // ...the same with `LDA : ASL : TAX : LDA table,X` in front of the compare.
    [HUD_BLK_COUNT_SAME] = {34 + 12 + 12 + 40 + 40 + 18, 14},
    [HUD_BLK_COUNT_CHANGED] = {34 + 12 + 12 + 40 + 40 + 12 + 40 + 40 + 56, 24},
    // The score: two compares, and three ways out of them.
    [HUD_BLK_SCORE_SAME] = {34 + 40 + 12 + 34 + 40 + 18, 18},
    [HUD_BLK_SCORE_LOW] = {34 + 40 + 18, 9},
    [HUD_BLK_SCORE_HIGH] = {34 + 40 + 12 + 34 + 40 + 12, 18},
    [HUD_BLK_SCORE_TAIL] = {40 + 34 + 40 + 34 + 40 + 56, 20},
    // $80:C6E4 .. $80:C7AB.
    [HUD_BLK_ADAPT_SCORE] = {18 + 18 + 18, 9},
    [HUD_BLK_ADAPT_HEALTH] = {18 + 18, 6},
    [HUD_BLK_ADAPT_COUNT_NONE] = {18 + 34 + 18 + 18, 11},
    [HUD_BLK_ADAPT_COUNT_OVER] = {18 + 34 + 12 + 18 + 18 + 18, 16},
    [HUD_BLK_ADAPT_COUNT_SHOWN] =
        {18 + 34 + 12 + 18 + 12 + 12 + 12 + 18 + 12 + 18, 22},
    [HUD_BLK_ADAPT_ICON_SHOWN] = {34 + 12 + 18 + 18, 11},
    // The blank path is the long one: it clears the icon *and* the count beside
    // it, so it pays for two `hud_blank`s as well as these six instructions.
    [HUD_BLK_ADAPT_ICON_NONE] = {34 + 18 + 18 + 40 + 18 + 18, 17},
    // $80:C580: one `LDA #$0000` and six stores.
    [HUD_BLK_BLANK] = {18 + 6 * 40 + 40, 28},
    // $80:C59C and the $80:C379 it jumps into — ten tiles out of a table.
    [HUD_BLK_HEALTH] = {326 + 1248, 26 + 101},
    // $80:C5C2 and $80:C666: four tiles each, and one `LDY $20` apart.
    [HUD_BLK_ICON_WEAPON] = {308 + 340, 24 + 28},
    [HUD_BLK_ICON_ITEM] = {280 + 340, 22 + 28},
    // $80:C4EC. The common tail — store, `INX INX`, `RTS` — is 104 over 7
    // bytes, and the print's `CLC : ADC : SEC : ROR $1E` is 92 over 7.
    [HUD_BLK_DIGIT_NONZERO] = {18 + 92 + 104, 2 + 7 + 7},
    [HUD_BLK_DIGIT_LEAD] = {12 + 28 + 18 + 92 + 104, 6 + 7 + 7},
    [HUD_BLK_DIGIT_BLANK] = {12 + 28 + 12 + 18 + 18 + 104, 11 + 7},
    // The `BIT $1E` both renderers end on, and the forced zero when nothing
    // printed.
    [HUD_BLK_DIGITS_END_PRINTED] = {28 + 18, 4},
    [HUD_BLK_DIGITS_END_ZERO] = {28 + 12 + 18 + 40, 11},
    // $80:C519 and $80:C553 with their digits taken out. The score's includes
    // the two dead instructions at `$80:C526` — the port does not have them
    // because they leave nothing behind, but the machine still pays for them.
    [HUD_BLK_DIGITS8] = {128 + 80 + 1290 + 40, 13 + 6 + 4 * 27 + 1},
    [HUD_BLK_DIGITS3] = {410, 34},
};

static int hud_cycles(const HudWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < HUD_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&HUD_BLOCK_COST[i], fast);
  return cycles;
}

// Two things about the caller decide prices the table above does not carry, so
// a call that presents either goes unpriced and falls back to the declared mean
// — visibly, as a `priced` below `checked` in the cost-model report.
//
// **The direct page has to be page-aligned.** Every routine in the cluster
// scratches `$1E`, `$20`, `$22` and `$24` on the caller's, and a direct page
// with a low byte costs one extra internal cycle on each of those.
//
// **The data bank has to be a slow one, and this is the assumption that was
// wrong first time round.** A LoROM cartridge appears twice and only the `$80`+
// copy is fast, so `$80:C5A3 LDA $C5B6,X` — an instruction in bank $80 reading a
// table through the *data bank* — costs 40 cycles through bank $00 and 36
// through bank $80. Every caller in the corpus leaves a low bank there, the
// three-instruction difference is real, and it showed up as exactly three calls
// out of 233 whose model was short by 12.
static void hud_report_cost(const CosimRegs* in, const HudWork* work) {
  if ((in->d & 0xff) == 0 && in->db < 0x80)
    R36_COSIM_COST(hud_cycles(work, in->fastrom));
}

static void shim_hud_panel1(Wram* w, const Rom* rom, const CosimRegs* in,
                            CosimRegs* out) {
  HudWork work = {0};
  HudPanelRegs r = {.a = in->a, .x = in->x, .y = in->y,
                    .n = in->n, .z = in->z, .c = in->c, .work = &work};
  hud_panel(w, rom, in->d, HUD_SIDE_P1, &r);
  hud_report_cost(in, &work);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_hud_panel2(Wram* w, const Rom* rom, const CosimRegs* in,
                            CosimRegs* out) {
  HudWork work = {0};
  HudPanelRegs r = {.a = in->a, .x = in->x, .y = in->y,
                    .n = in->n, .z = in->z, .c = in->c, .work = &work};
  hud_panel(w, rom, in->d, HUD_SIDE_P2, &r);
  hud_report_cost(in, &work);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

static void shim_hud_refresh(Wram* w, const Rom* rom, const CosimRegs* in,
                             CosimRegs* out) {
  // A is not an input: `LDA $24` overwrites it before anything reads it. X, Y
  // and carry are, because the quiet paths hand all three straight back.
  HudWork work = {0};
  HudRefreshRegs r = {.x = in->x, .y = in->y, .c = in->c, .work = &work};
  hud_refresh(w, rom, in->d, &r);
  hud_report_cost(in, &work);
  out->a = r.a;
  out->x = r.x;
  out->y = r.y;
  out->n = r.n;
  out->z = r.z;
  out->c = r.c;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C;
}

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

// `cycles` is the mean cost of the ROM's own instructions and `stack_bytes` the
// deepest its stack pointer went, both as `zamn_cosim verify` measured them over
// **movies/level1-rescue.zmv** — the longer of the two movies, and the only one
// that produces a collision at all. `cycles` is what a substituted call burns in
// native mode so the rest of the machine still sees a call that took about as
// long as it used to.
//
// The whole column moved when the collision handlers were ported, and not
// because the ROM changed: a routine's mean is taken over the calls the port
// *serves*, and the 1,226 passes containing a collision used to be declined.
// They are the expensive ones. Re-measure and update these whenever a guard's
// answer changes; `verify` prints the range it saw alongside the mean.
// The stretches of WRAM these routines publish to the NMI, held back until
// their budget is spent. See `CosimRoutine::commit`; the census that says which
// routines need this, and which of them the corpus actually interrupts, is in
// docs/cosim.md.
//
// Each span is where the ROM's *publishing* write lands, which is not always
// where it looks. Getting that wrong is not a small error: the first cut of
// this named `$0C` for the vblank queues, and `$80:83E0` dispatches on the slot
// rather than the count, so it held back a word nobody was waiting on and
// changed nothing at all.

// Queue A and queue B are dispatched off their slots -- `$80:83E9` and
// `$80:8446` read the slot and skip it when it is zero -- so the slot table is
// what publishes a job. The count comes too, because `$80:840B` decrements it
// while we are holding it and only the delta survives that.
static const CosimCommitSpan COMMIT_VBL_A[] = {
    {W_VBL_QUEUE_A, W_VBL_QUEUE_A_SLOTS * 4, false},
    {W_VBL_QUEUE_A_COUNT, 2, true},
};
static const CosimCommitSpan COMMIT_VBL_B[] = {
    {W_VBL_QUEUE_B, W_VBL_QUEUE_B_SLOTS * 4, false},
    {W_VBL_QUEUE_B_COUNT, 2, true},
};

// The VRAM queue is the other shape: `$80:9EB9` bounds the drain with
// `CPX $CE`, so nothing in the five arrays is reachable until the count says
// so, and the count is the whole commit. It is a store rather than an increment
// because the routine reads `$CE` before it writes the payload and stores an
// absolute value after -- so it really does overwrite the `STZ $CE` at
// `$80:9EBF`, and that quirk is the ROM's.
static const CosimCommitSpan COMMIT_VRAM_QUEUE[] = {{W_VRAM_QUEUE_COUNT, 2, false}};

// Bit 6 gates the flush at `$80:9E7D`, and the routine writes the word rather
// than setting a bit in it.
static const CosimCommitSpan COMMIT_RENDER_FLAGS[] = {{W_RENDER_FLAGS, 2, false}};

// The widest commit in the registry: the row loop's cursor, absolute for the
// same reason `$CE` is, and then the vblank job queued on top of it.
static const CosimCommitSpan COMMIT_BOSS_BG[] = {
    {W_BG_DMA_CURSOR, 2, false},
    {W_VBL_QUEUE_A, W_VBL_QUEUE_A_SLOTS * 4, false},
    {W_VBL_QUEUE_A_COUNT, 2, true},
};

// ---------------------------------------------------------------------------
// The scheduler and the vblank dispatchers — routines that leave by a jump
// ---------------------------------------------------------------------------
//
// See `port/sched.h`. The shim's whole job here is to move the register set
// in and out; the port decides everything, the exit included.

static void cpu_from(const CosimRegs* in, PortCpu* c) {
  c->a = in->a;
  c->x = in->x;
  c->y = in->y;
  c->s = in->s;
  c->d = in->d;
  c->db = in->db;
  c->p = in->p;
  c->pc = in->pc;
}

// Code fetched through bank `$00` is slow whatever `$420D` says, and the
// scheduler runs there after a thread ends -- see `port/sched.h`. Nothing in
// these runs reads ROM as data except `nmi_input`, which is always in `$80`.
static bool fetch_fast(const CosimRegs* in) {
  return in->fastrom && (in->pc >> 16) >= 0x80;
}

static void cpu_to(const PortCpu* c, CosimRegs* out) {
  out->a = c->a;
  out->x = c->x;
  out->y = c->y;
  out->s = c->s;
  out->d = c->d;
  out->db = c->db;
  out->p = c->p;
  out->pc = c->pc;
  out->n = (c->p & PORT_P_N) != 0;
  out->v = (c->p & PORT_P_V) != 0;
  out->z = (c->p & PORT_P_Z) != 0;
  out->c = (c->p & PORT_P_C) != 0;
  out->flags = COSIM_FLAG_N | COSIM_FLAG_Z | COSIM_FLAG_C | COSIM_FLAG_V;
  out->regs = COSIM_REG_ALL;
}

// Each run's price, from `tools/cycles816.py` over the listing, with branches
// taken or not as the run's name says. Nothing here depends on the direct page:
// every one of these runs on page zero, which the guards insist on where the
// ROM does not install it itself.
static const CosimRun SCHED_COST[SCHED_BLOCK_COUNT] = {
    [SCHED_PARK] = {338, 23, 0},
    [SCHED_EXIT] = {292, 21, 0},
    [SCHED_STEP] = {60, 7, 0},
    [SCHED_WRAP] = {54, 7, 0},
    [SCHED_EMPTY] = {58, 5, 0},
    [SCHED_ASLEEP] = {82, 8, 0},
    [SCHED_RESUME] = {242, 17, 0},
    [SCHED_WAKE] = {98, 8, 0},
    [SCHED_WAKE_CARRY] = {142, 10, 0},
    [SCHED_TICK_HEAD] = {58, 6, 0},
    [SCHED_TICK_EMPTY] = {58, 5, 0},
    [SCHED_TICK_DONE] = {88, 10, 0},
    [SCHED_TICK_STEP] = {134, 14, 0},
    [SCHED_TICK_NEXT] = {42, 4, 0},
    [SCHED_TICK_TAIL] = {94, 8, 0},
};

static int sched_cycles(const SchedWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < SCHED_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&SCHED_COST[i], fast);
  return cycles;
}

// The same two dispatchers, so one table.
static const CosimRun VBL_RUN_COST[VBL_RUN_BLOCK_COUNT] = {
    [VBL_RUN_EMPTY_QUEUE] = {46, 4, 0},
    [VBL_RUN_HEAD] = {86, 9, 0},
    [VBL_RUN_FREE_SLOT] = {58, 5, 0},
    [VBL_RUN_JOB] = {290, 23, 0},
    [VBL_RUN_KEPT] = {46, 4, 0},
    [VBL_RUN_DROPPED] = {198, 14, 0},
    [VBL_RUN_DROPPED_OUT] = {18, 2, 0},
    [VBL_RUN_DROPPED_ON] = {12, 2, 0},
    [VBL_RUN_NEXT] = {66, 6, 0},
    [VBL_RUN_END] = {60, 6, 0},
};

static int vbl_run_cycles(const VblRunWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < VBL_RUN_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&VBL_RUN_COST[i], fast);
  return cycles;
}

// What every one of these assumes and the ROM does not check, because on its
// own paths it is always so: a stack in low WRAM, and the bank the tables are
// read through showing low WRAM too — `$00-$3F` and `$80-$BF` mirror it, and
// `$7E` is it.
static bool low_stack(const CosimRegs* in) {
  return in->s >= 0x0010 && in->s < 0x2000;
}

static bool bank_sees_low_wram(uint8_t db) {
  return (db & 0x40) == 0 || db == 0x7e;
}

// ...and the slot the scheduler is on, which it indexes two tables with.
static bool cur_task_ok(const Wram* w) {
  const uint16_t x = wram_r16(w, W_SCHED_CUR_TASK);
  return x < WRAM_THREAD_SLOTS * 2 && (x & 1) == 0;
}

static bool wide(const CosimRegs* in) {
  return (in->p & (PORT_P_M | PORT_P_X)) == 0;
}

// $80:8353  thread_yield — A = ticks to sleep. `PHB : PHP : REP #$30` comes
// before anything width-dependent, so any caller's widths will do.
static bool accepts_thread_yield(const Wram* w, const CosimRegs* in) {
  return low_stack(in) && cur_task_ok(w);
}

static void shim_thread_yield(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  (void)rom;
  PortCpu c;
  SchedWork k = {0};
  cpu_from(in, &c);
  thread_yield_port(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(sched_cycles(&k, fetch_fast(in)));
}

// $80:833E  thread_exit — where a thread body's own `RTL` goes.
static bool accepts_thread_exit(const Wram* w, const CosimRegs* in) {
  return wide(in) && low_stack(in) && cur_task_ok(w);
}

static void shim_thread_exit(Wram* w, const Rom* rom, const CosimRegs* in,
                             CosimRegs* out) {
  (void)rom;
  PortCpu c;
  SchedWork k = {0};
  cpu_from(in, &c);
  thread_exit_port(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(sched_cycles(&k, fetch_fast(in)));
}

// $80:8372 and $80:8380 — after the `WAI`, and after `sprite_build_oam`.
static bool accepts_sched_wake(const Wram* w, const CosimRegs* in) {
  (void)w;
  return wide(in) && in->d == 0;
}

static void shim_sched_wake(Wram* w, const Rom* rom, const CosimRegs* in,
                            CosimRegs* out) {
  (void)rom;
  PortCpu c;
  SchedWork k = {0};
  cpu_from(in, &c);
  sched_wake(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(sched_cycles(&k, fetch_fast(in)));
}

static bool accepts_sched_rescan(const Wram* w, const CosimRegs* in) {
  (void)w;
  return wide(in) && in->d == 0 && low_stack(in) && bank_sees_low_wram(in->db);
}

static void shim_sched_rescan(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  (void)rom;
  PortCpu c;
  SchedWork k = {0};
  cpu_from(in, &c);
  sched_rescan(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(sched_cycles(&k, fetch_fast(in)));
}

// $80:83E0 / $80:843D, and the two addresses a job returns to. A job that came
// back 8-bit, or on another page, would be read wrongly by the ROM too; the
// port declines it rather than agree.
static bool accepts_vbl_run(const Wram* w, const CosimRegs* in) {
  (void)w;
  return wide(in) && in->d == 0 && low_stack(in) && bank_sees_low_wram(in->db);
}

static void vbl_run_shim(Wram* w, const VblQueueDesc* q, bool resumed,
                         const CosimRegs* in, CosimRegs* out) {
  PortCpu c;
  VblRunWork k = {0};
  cpu_from(in, &c);
  vbl_queue_run(w, q, resumed, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(vbl_run_cycles(&k, in->fastrom));
}

static void shim_vbl_queue_a_run(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  vbl_run_shim(w, &VBL_QUEUE_A_DESC, false, in, out);
}

static void shim_vbl_queue_a_resume(Wram* w, const Rom* rom, const CosimRegs* in,
                                    CosimRegs* out) {
  (void)rom;
  vbl_run_shim(w, &VBL_QUEUE_A_DESC, true, in, out);
}

static void shim_vbl_queue_b_run(Wram* w, const Rom* rom, const CosimRegs* in,
                                 CosimRegs* out) {
  (void)rom;
  vbl_run_shim(w, &VBL_QUEUE_B_DESC, false, in, out);
}

static void shim_vbl_queue_b_resume(Wram* w, const Rom* rom, const CosimRegs* in,
                                    CosimRegs* out) {
  (void)rom;
  vbl_run_shim(w, &VBL_QUEUE_B_DESC, true, in, out);
}

// $80:8179 and on — the NMI handler, between its hardware accesses.
//
// `nmi_input`'s two table reads are ROM words through bank $80, so they cost
// what program bytes do. `tools/cycles816.py` already counts them: its 42 is
// 38 program bytes and those 4. The first version added the 4 again and was 8
// cycles long on every call made with `$420D` clear, 6,270 of them over the
// corpus. The `$4218`/`$421A` reads are I/O at 6 cycles whatever `$420D` says.
static const CosimRun NMI_COST[NMI_BLOCK_COUNT] = {
    [NMI_ENTER] = {356, 22, 0},
    [NMI_ENTER_BUSY] = {524, 27, 0},
    [NMI_STACK_RUN] = {88, 9, 0},
    [NMI_INPUT_RUN] = {394, 42, 0},
    [NMI_LEAVE] = {322, 18, 0},
    [NMI_LEAVE_TICK] = {366, 20, 0},
};

static int nmi_cycles(const NmiWork* k, bool fast) {
  int cycles = 0;
  for (int i = 0; i < NMI_BLOCK_COUNT; i++)
    cycles += k->blocks[i] * cosim_run_cycles(&NMI_COST[i], fast);
  return cycles;
}

// The first stretch pushes onto whatever stack the interrupt found, and the
// rest run on page zero, which the first installs.
static bool accepts_nmi_enter(const Wram* w, const CosimRegs* in) {
  (void)w;
  return low_stack(in);
}

static bool accepts_nmi(const Wram* w, const CosimRegs* in) {
  (void)w;
  return in->d == 0 && low_stack(in) && bank_sees_low_wram(in->db);
}

// `nmi_leave` pulls five registers at 16 bits, which the dispatcher before it
// leaves set.
static bool accepts_nmi_leave(const Wram* w, const CosimRegs* in) {
  return wide(in) && accepts_nmi(w, in);
}

static void shim_nmi_enter(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  (void)rom;
  PortCpu c;
  NmiWork k = {0};
  cpu_from(in, &c);
  nmi_enter(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(nmi_cycles(&k, in->fastrom));
}

static void shim_nmi_stack(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  (void)rom;
  PortCpu c;
  NmiWork k = {0};
  cpu_from(in, &c);
  nmi_stack(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(nmi_cycles(&k, in->fastrom));
}

static void shim_nmi_input(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  PortCpu c;
  NmiWork k = {0};
  cpu_from(in, &c);
  nmi_input(w, rom, &c, in->joy[0], in->joy[1], &k);
  cpu_to(&c, out);
  R36_COSIM_COST(nmi_cycles(&k, in->fastrom));
}

static void shim_nmi_leave(Wram* w, const Rom* rom, const CosimRegs* in,
                           CosimRegs* out) {
  (void)rom;
  PortCpu c;
  NmiWork k = {0};
  cpu_from(in, &c);
  nmi_leave(w, &c, &k);
  cpu_to(&c, out);
  R36_COSIM_COST(nmi_cycles(&k, in->fastrom));
}

// $80:80C1  the reset's WRAM clear. Each byte `MVN` moves is the instruction
// again, three program bytes and all, so it is priced a byte at a time.
static const CosimRun RESET_COST[RESET_BLOCK_COUNT] = {
    [RESET_HEAD] = {128, 16, 0},
    [RESET_CHECK] = {70, 9, 0},
    [RESET_TAKEN] = {6, 0, 0},
    [RESET_SETUP] = {54, 9, 0},
    [RESET_WARM] = {112, 16, 0},
    [RESET_BRA] = {18, 2, 0},
    [RESET_MAGIC] = {232, 28, 0},
    [RESET_TAIL] = {262, 26, 2},
};
static const CosimRun RESET_MOVE_BYTE = {46, 3, 0};

// `init_ppu_regs` leaves page zero and 16-bit registers, and the stack at
// `$01FF` from the `TXS` before it.
static bool accepts_reset_clear(const Wram* w, const CosimRegs* in) {
  (void)w;
  return wide(in) && in->d == 0 && low_stack(in);
}

static void shim_reset_clear(Wram* w, const Rom* rom, const CosimRegs* in,
                             CosimRegs* out) {
  (void)rom;
  PortCpu c;
  ResetWork k = {0};
  cpu_from(in, &c);
  reset_clear(w, &c, &k);
  cpu_to(&c, out);
  const bool fast = fetch_fast(in);
  int cycles = (int)k.moved * cosim_run_cycles(&RESET_MOVE_BYTE, fast);
  for (int i = 0; i < RESET_BLOCK_COUNT; i++)
    cycles += k.blocks[i] * cosim_run_cycles(&RESET_COST[i], fast);
  R36_COSIM_COST(cycles);
}

static const uint32_t RESET_EXITS[] = {RESET_NMI_ON_PC};

static const uint32_t NMI_ENTER_EXITS[] = {NMI_BLANK_PC, NMI_RETURN_PC};
static const uint32_t NMI_STACK_EXITS[] = {NMI_FLUSH_PC};
static const uint32_t NMI_INPUT_EXITS[] = {NMI_QUEUE_B_PC};
static const uint32_t NMI_LEAVE_EXITS[] = {NMI_RETURN_PC};

static const uint32_t SCHED_EXITS[] = {SCHED_RESUME_PC, SCHED_WAI_PC};
static const uint32_t SCHED_WAKE_EXITS[] = {SCHED_OAM_PC};
// ...and the same three exits through bank `$00`, where a thread's end leaves
// the scheduler running. See `port/sched.h`.
static const uint32_t SCHED_EXITS_00[] = {SCHED_RESUME_PC & 0xffffu,
                                          SCHED_WAI_PC & 0xffffu};
static const uint32_t SCHED_WAKE_EXITS_00[] = {SCHED_OAM_PC & 0xffffu};
static const uint32_t VBL_A_EXITS[] = {0x808400u, 0x808417u};
static const uint32_t VBL_B_EXITS[] = {0x80845du, 0x808474u};

static const uint32_t ZOMBIE_MOVE_NATIVE_EXITS[] = {ZOMBIE_MOVE_RTS_PC, ZOMBIE_ROTATE_RTS_PC};
static const uint32_t ZOMBIE_MOVE_SETUP_EXITS[] = {ZOMBIE_MOVE_TERRAIN_CALL_PC};
static const uint32_t ZOMBIE_AFTER_TERRAIN_EXITS[] = {ZOMBIE_MOVE_ROTATE_PC, ZOMBIE_MOVE_ACTOR_CALL_PC};
static const uint32_t ZOMBIE_AFTER_ACTOR_EXITS[] = {ZOMBIE_MOVE_RTS_PC};
static const uint32_t ZOMBIE_ROTATE_EXITS[] = {ZOMBIE_ROTATE_RTS_PC};
static const uint32_t ZOMBIE_SEEK_NATIVE_EXITS[] = {0x8186adu, ZOMBIE_SEEK_RTS_PC};
static const uint32_t ZOMBIE_SEEK_EXITS[] = {ZOMBIE_NEAREST_CALL_PC};
static const uint32_t ZOMBIE_AFTER_NEAREST_EXITS[] = {0x8186adu, ZOMBIE_BEARING_PREP_PC};
static const uint32_t ZOMBIE_BEARING_PREP_EXITS[] = {ZOMBIE_BEARING_CALL_PC};
static const uint32_t ZOMBIE_AFTER_BEARING_EXITS[] = {ZOMBIE_SEEK_RTS_PC};
static const uint32_t ZOMBIE_ANIM_EXITS[] = {ZOMBIE_ANIM_RTS_A_PC, ZOMBIE_ANIM_RTS_B_PC};
static const uint32_t ZOMBIE_HANDLER_EXITS[] = {ZOMBIE_HANDLER_RTS_PC};
static const uint32_t ZOMBIE_POST_ANIM_EXITS[] = {ZOMBIE_YIELD_SETUP_PC, ZOMBIE_END_PC, ZOMBIE_SPECIAL_CALL_PC};


#define COSIM_EXITS(tbl) \
  .exits = (tbl), .exit_count = (int)(sizeof(tbl) / sizeof((tbl)[0]))

// ---------------------------------------------------------------------------
// Thread bodies — see `port/bodies.h`
// ---------------------------------------------------------------------------
//
// Stretches like the scheduler's, stopping at every yield and every call. The
// prices are `tools/cycles816.py`'s with `--db 9F --ind=9F:8000`, branches not
// taken, and a taken one is the block `*_TAKEN` on top. `LDA ($0C),Y` is
// priced for a data byte from fast ROM, which the level lists in `$9F` are;
// the port counts the bytes it actually read, and one that was not fast ROM
// costs 2 more whatever `$420D` says.

static const CosimRun VICTIMS_COST[VICTIMS_BLOCK_COUNT] = {
    [VICTIMS_START] = {126, 7, 3},
    [VICTIMS_STZ] = {28, 2, 1},
    [VICTIMS_TICKS] = {18, 3, 0},
    [VICTIMS_HEAD] = {184, 18, 2},
    [VICTIMS_FLAG] = {98, 11, 1},
    [VICTIMS_INDEX] = {186, 14, 3},
    [VICTIMS_DX] = {80, 7, 2},
    [VICTIMS_DY] = {150, 11, 3},
    [VICTIMS_NEG] = {30, 4, 0},
    [VICTIMS_CMP] = {30, 5, 0},
    [VICTIMS_NEXT] = {68, 4, 1},
    [VICTIMS_NEXT_JMP] = {68, 5, 1},
    [VICTIMS_STOP] = {168, 21, 0},
    [VICTIMS_TAKEN] = {6, 0, 0},
};

static const CosimRun OBJECT_COST[OBJECT_BLOCK_COUNT] = {
    [OBJECT_POLL] = {40, 4, 1},
    [OBJECT_TICK] = {64, 8, 0},
    [OBJECT_TICKS] = {18, 3, 0},
    [OBJECT_STZ] = {28, 2, 1},
    [OBJECT_HEAD] = {184, 18, 2},
    [OBJECT_SCAN] = {80, 7, 1},
    [OBJECT_LIVE] = {12, 2, 0},
    [OBJECT_D] = {92, 9, 1},
    [OBJECT_NEG] = {30, 4, 0},
    [OBJECT_CMP] = {30, 5, 0},
    [OBJECT_STATE] = {52, 5, 0},
    [OBJECT_STEP] = {118, 6, 2},
    [OBJECT_BRA] = {18, 2, 0},
    [OBJECT_TAKEN] = {6, 0, 0},
};

static const CosimRun ACTORS_COST[ACTORS_BLOCK_COUNT] = {
    [ACTORS_CHECK] = {12, 2, 0},
    [ACTORS_TICKS] = {18, 3, 0},
    [ACTORS_COUNT] = {98, 11, 1},
    [ACTORS_TICK] = {98, 11, 0},
    [ACTORS_NEXT] = {68, 4, 1},
    [ACTORS_INDEX] = {144, 11, 3},
    [ACTORS_TYPE] = {76, 7, 1},
    [ACTORS_XY] = {184, 11, 4},
    [ACTORS_BEST] = {40, 4, 1},
    [ACTORS_NEW_BEST] = {84, 6, 3},
    [ACTORS_END] = {58, 7, 1},
    [ACTORS_RESET] = {74, 7, 2},
    [ACTORS_PICK] = {156, 12, 3},
    [ACTORS_ARM] = {132, 13, 1},
    [ACTORS_BACK] = {18, 2, 0},
    [ACTORS_TAKEN] = {6, 0, 0},
};

static const CosimRun TANIM_COST[TANIM_BLOCK_COUNT] = {
    [TANIM_HEAD] = {40, 4, 1},
    [TANIM_START] = {18, 3, 0},
    [TANIM_SLOT] = {70, 6, 1},
    [TANIM_IDLE] = {30, 5, 0},
    [TANIM_BRA] = {18, 2, 0},
    [TANIM_COUNT] = {68, 4, 1},
    // Six of the run's bytes are data -- the two sequence words and the bit
    // table -- and `BodyWork` counts them, so they are not in this column.
    [TANIM_FRAME] = {530, 41, 5},
    [TANIM_WRAP] = {30, 5, 0},
    [TANIM_RESTART] = {66, 5, 1},
    [TANIM_NEXT] = {110, 8, 2},
    [TANIM_END] = {102, 6, 2},
    [TANIM_DONE] = {46, 5, 0},
    [TANIM_QUEUE] = {36, 6, 0},
    [TANIM_TICKS] = {18, 3, 0},
    [TANIM_TAKEN] = {6, 0, 0},
};


// R25 level-1 zombie body slices.  These are intentionally small stretches;
// the expensive terrain/actor searches and scheduler calls remain their own
// already-verified native routines.  Costs are master-cycle models for only the
// straight-line instructions replaced here, with external ROM-table bytes
// accounted through BodyWork's data counters.
static const CosimRun ZBODY_COST[ZBODY_BLOCK_COUNT] = {
    [ZBODY_MOVE_SETUP]     = {402, 37, 4},
    [ZBODY_AFTER_TERRAIN]  = {70,   7, 2},
    [ZBODY_AFTER_ACTOR]    = {18,   2, 0},
    [ZBODY_MOVE_COMMIT]    = {150, 13, 4},
    [ZBODY_ROTATE]         = {226, 25, 4},
    [ZBODY_SEEK_PREP]      = {54,   4, 2},
    [ZBODY_AFTER_NEAREST]  = {58,   8, 0},
    [ZBODY_BEARING_PREP]   = {72,   7, 2},
    [ZBODY_AFTER_BEARING]  = {62,   4, 1},
    [ZBODY_ANIM_EARLY]     = {36,   4, 1},
    [ZBODY_ANIM_FULL]      = {330, 32, 7},
    [ZBODY_HANDLER_CALL]   = {92,   7, 1},
    [ZBODY_POST_ZERO]      = {40,   4, 1},
    [ZBODY_POST_NORMAL]    = {58,   7, 1},
    [ZBODY_POST_SPECIAL]   = {126, 14, 3},
    [ZBODY_TAKEN]          = {6,    0, 0},
};

// The player's frame, `$80:CDF4`, and the movement handler at `$80:E4BA`.
// Nothing here reads outside the page but `$1D52` and `$1CB8,X`, low WRAM.
static const CosimRun PBODY_COST[PBODY_BLOCK_COUNT] = {
    [PBODY_TICKS] = {18, 3, 0},
    [PBODY_STATE] = {102, 7, 1},
    [PBODY_MOVE] = {40, 4, 1},
    [PBODY_MOVE_CALL] = {74, 5, 0},
    [PBODY_BUTTONS] = {56, 4, 2},
    [PBODY_BRA] = {18, 2, 0},
    [PBODY_BRANCH] = {28, 2, 1},
    [PBODY_EVENT] = {40, 4, 1},
    [PBODY_SKIP] = {40, 4, 1},
    [PBODY_RECOVER] = {62, 4, 1},
    [PBODY_RECOVERED] = {46, 5, 1},
    [PBODY_WON] = {46, 5, 0},
    [PBODY_DEAD] = {80, 7, 1},
    [PBODY_TAKEN] = {6, 0, 0},
};

static int body_cycles(const BodyWork* k, const CosimRun* cost, int count,
                       const CosimRegs* in) {
  const bool fast = fetch_fast(in);
  const bool unaligned = (in->d & 0x00ffu) != 0;
  int cycles = 0;
  for (int i = 0; i < count; i++)
    cycles += k->blocks[i] * cosim_run_cycles_dp(&cost[i], fast, unaligned);
  // The lists' own bytes: priced fast in the blocks, 2 more for each that was
  // not, and 2 more for the ones that were while `$420D` is clear.
  cycles += 2 * k->slow_data + (in->fastrom ? 0 : 2 * k->fast_data);
  return cycles;
}

// What all of them assume: 16-bit registers, binary mode, a thread's own page
// in low WRAM, and a stack there.
static bool body_ok(const CosimRegs* in) {
  return wide(in) && (in->p & PORT_P_D) == 0 && in->d < 0x1f00 &&
         low_stack(in);
}

// ...and the ones that read `$1B6A` and the like through the data bank.
static bool accepts_body_low(const Wram* w, const CosimRegs* in) {
  (void)w;
  return body_ok(in) && bank_sees_low_wram(in->db);
}

static bool accepts_body(const Wram* w, const CosimRegs* in) {
  (void)w;
  return body_ok(in);
}

// ...and the two that walk a level list through `($0C),Y`, which is in ROM
// through a bank that also shows low WRAM. Nothing else has been seen there.
static bool accepts_body_list(const Wram* w, const CosimRegs* in) {
  return accepts_body_low(w, in) && in->db >= 0x80 && in->db < 0xc0 &&
         wram_r16(w, (uint16_t)(in->d + 0x0c)) >= 0x8000;
}

// R49 measured player-state residuals; pure guards reuse the body convention.
// Reference costs include actual branch paths, FastROM and unaligned-DP costs.
#define R49_PLAYER_SHIM(name) \
  static void shim_##name(Wram* w, const Rom* rom, const CosimRegs* in, CosimRegs* out) { \
    (void)rom; PortCpu c; PlayerResumeWork k = {0}; cpu_from(in, &c); \
    name(w, &c, R36_COSTS_NEEDED() ? &k : NULL); cpu_to(&c, out); \
    R36_COSIM_COST(k.fast_cycles + (in->fastrom ? 0 : 2*k.rom_bytes) + \
                   ((in->d & 255u) ? 6*k.dp_accesses : 0)); \
  }
R49_PLAYER_SHIM(player_idle_resume)
static const uint32_t player_idle_resume_exits[] = {0x80d4e9u, 0x80d4f4u, 0x80d554u, 0x80d557u};
R49_PLAYER_SHIM(player_walk_resume)
static const uint32_t player_walk_resume_exits[] = {0x80d4e9u, 0x80d700u, 0x80d6b2u};
R49_PLAYER_SHIM(player_walk_fire_resume)
static const uint32_t player_walk_fire_resume_exits[] = {0x80d4e9u, 0x80d700u, 0x80d6c8u, 0x80d6d5u};
R49_PLAYER_SHIM(player_walk_after_fire)
static const uint32_t player_walk_after_fire_exits[] = {0x80d6d5u};
R49_PLAYER_SHIM(player_walk_animation)
static const uint32_t player_walk_animation_exits[] = {0x80f300u};
#undef R49_PLAYER_SHIM

#if defined(XBOX_PORT) && defined(ZAMN_R50_NATIVE_HOTPATHS)
static bool accepts_overlap_scan(const Wram* w, const CosimRegs* in) {
  PortCpu c; cpu_from(in,&c);
  return wide(in) && actor_overlap_reach==OVERLAP_REACH_STOCK && native_overlap_supported(w,&c);
}
static bool guard_sprite_emit_native(Wram* w, const Rom* rom, const CosimRegs* in) {
  PortCpu c; cpu_from(in,&c);
  return wide(in) && native_sprite_supported(w,rom,&c);
}
static void shim_overlap_scan(Wram* w,const Rom* rom,const CosimRegs* in,CosimRegs* out) {
  (void)rom; PortCpu c; NativeHotWork k={0}; cpu_from(in,&c);
  native_overlap_scan(w,&c,R36_COSTS_NEEDED()?&k:NULL); cpu_to(&c,out);
  R36_COSIM_COST(k.fast_cycles+(in->fastrom?0:2*k.rom_bytes));
}
static void shim_sprite_emit_native(Wram* w,const Rom* rom,const CosimRegs* in,CosimRegs* out) {
  PortCpu c; NativeHotWork k={0}; cpu_from(in,&c);
  native_sprite_emit(w,rom,&c,R36_COSTS_NEEDED()?&k:NULL); cpu_to(&c,out);
  R36_COSIM_COST(k.fast_cycles+(in->fastrom?0:2*k.rom_bytes));
}
static const uint32_t overlap_scan_exits[]={0x80bf0d,0x80bf1a};
static const uint32_t sprite_emit_native_exits[]={0x80ba98,0x80bab9};
#if defined(ZAMN_R51_NATIVE_HOTPATHS)
static const uint32_t sprite_emit_flipx_exits[]={0x80bb08,0x80bb2f};
static const uint32_t sprite_emit_flipy_exits[]={0x80bb7e,0x80bba5};
static const uint32_t sprite_emit_flipxy_exits[]={0x80bbfb,0x80bc22};
#if defined(ZAMN_RELEASE_NO_DIAGNOSTICS) || defined(ZAMN_R51_PROFILE_FULL_WALKER) || defined(ZAMN_R70_DIAGNOSTIC_OAM_PARITY)
// R51 release-only OAM walker slices. Earlier diagnostics deliberately kept
// these three in ROM for historical comparison. R70 can explicitly register
// them without activating the expensive full R51 walker profiler, providing
// release/diagnostic parity (393 native entries).
static bool guard_oam_walk_native(Wram* w, const Rom* rom, const CosimRegs* in) {
  PortCpu c; cpu_from(in,&c);
  return wide(in) && native_oam_walk_supported(w,rom,&c);
}
static void shim_oam_walk_native(Wram* w,const Rom* rom,const CosimRegs* in,CosimRegs* out) {
  PortCpu c; cpu_from(in,&c); native_oam_walk(w,rom,&c,NULL); cpu_to(&c,out);
}
static const uint32_t oam_walk_native_exits[]={0x80bdb4,0x80bdcc};
static const uint32_t oam_tail_native_exits[]={0x80bde2};
#endif
#endif
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R53_NATIVE_MOVEMENT)
static bool guard_r53_movement(Wram* w,const Rom* rom,const CosimRegs* in) {
  PortCpu c; cpu_from(in,&c);
  return wide(in) && native_r53_supported(w,rom,&c);
}
static void shim_r53_movement(Wram* w,const Rom* rom,const CosimRegs* in,CosimRegs* out) {
  PortCpu c; cpu_from(in,&c);native_r53_movement(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r53_exit_9715[]={0x82973au};
static const uint32_t r53_exit_973e[]={0x829750u,0x829746u};
static const uint32_t r53_exit_974a[]={0x829750u};
static const uint32_t r53_exit_9751[]={0x829755u};
static const uint32_t r53_exit_9759[]={0x829761u,0x82976fu};
static const uint32_t r53_exit_9765[]={0x82976fu};
static const uint32_t r53_exit_9773[]={0x829785u,0x82977bu};
static const uint32_t r53_exit_977f[]={0x829785u};
static const uint32_t r53_exit_97be[]={0x82980eu};
static const uint32_t r53_exit_97d4[]={0x8297e0u};
static const uint32_t r53_exit_97e0[]={0x8297e5u};
static const uint32_t r53_exit_97e9[]={0x8297fdu};
static const uint32_t r53_exit_9800[]={0x82980au,0x82980bu};
static const uint32_t r53_exit_980e[]={0x829813u};
static const uint32_t r53_exit_981a[]={0x82982eu};
static const uint32_t r53_exit_9831[]={0x82983bu,0x82983cu};
static const uint32_t r53_exit_983f[]={0x829844u};
static const uint32_t r53_exit_985b[]={0x829860u,0x829863u};
static const uint32_t r53_exit_9863[]={0x829867u};
static const uint32_t r53_exit_986b[]={0x829880u};
static const uint32_t r53_exit_98ea[]={0x8298fau};
static const uint32_t r53_exit_98fe[]={0x82990au,0x829904u};
static const uint32_t r53_exit_9994[]={0x8299cbu,0x8299d8u};
static const uint32_t r53_exit_99cb[]={0x8299d7u};

#endif

#if defined(XBOX_PORT) && defined(ZAMN_R54_NATIVE_D9_CLUSTER)
static bool guard_r54_cluster(Wram* w,const Rom* rom,const CosimRegs* in) {
  PortCpu c;cpu_from(in,&c);return wide(in)&&native_r54_supported(w,rom,&c);
}
static void shim_r54_cluster(Wram* w,const Rom* rom,const CosimRegs* in,CosimRegs* out) {
  PortCpu c;cpu_from(in,&c);native_r54_d9_cluster(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r54_exit_d8db[]={0x82d8efu,0x82d8f3u};
static const uint32_t r54_exit_d8fd[]={0x82d907u,0x82d9b4u};
static const uint32_t r54_exit_d907[]={0x82d910u,0x82d913u};
static const uint32_t r54_exit_d913[]={0x82d91du,0x82d92au};
static const uint32_t r54_exit_d91d[]={0x82d8f6u};
static const uint32_t r54_exit_d92a[]={0x82d938u};
static const uint32_t r54_exit_d938[]={0x82d919u,0x82d945u};
static const uint32_t r54_exit_d945[]={0x82d94du};
static const uint32_t r54_exit_d94d[]={0x82d919u,0x82d96eu};
static const uint32_t r54_exit_d96e[]={0x82d919u,0x82d989u};
static const uint32_t r54_exit_d989[]={0x82d999u};
static const uint32_t r54_exit_d999[]={0x82d9aeu};
static const uint32_t r54_exit_d9ae[]={0x82d8f6u};
static const uint32_t r54_exit_d9b4[]={0x82d9bfu};
static const uint32_t r54_exit_d9c3[]={0x82d9d1u};
static const uint32_t r54_exit_d9d5[]={0x82d9d8u};
static const uint32_t r54_exit_d9dc[]={0x82d9e6u,0x82d9ebu};
static const uint32_t r54_exit_d9eb[]={0x82d9f1u};
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R55_NATIVE_ACTOR_FRAGMENTS)
static bool guard_r55_fragments(Wram*w,const Rom*rom,const CosimRegs*in) {
  PortCpu c;cpu_from(in,&c);return wide(in)&&native_r55_supported(w,rom,&c);
}
static void shim_r55_fragments(Wram*w,const Rom*rom,const CosimRegs*in,CosimRegs*out) {
  PortCpu c;cpu_from(in,&c);native_r55_actor_fragments(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r55_exit_839843[]={0x839848u,0x839865u};
static const uint32_t r55_exit_83984f[]={0x83985du,0x839864u};
static const uint32_t r55_exit_83985d[]={0x839848u,0x839865u};
static const uint32_t r55_exit_839865[]={0x83987du};
static const uint32_t r55_exit_839880[]={0x839897u};
static const uint32_t r55_exit_83989b[]={0x8398aau,0x839916u};
static const uint32_t r55_exit_8398aa[]={0x8398d2u,0x839894u};
static const uint32_t r55_exit_8398d2[]={0x8398e6u};
static const uint32_t r55_exit_8398f1[]={0x839894u,0x839916u,0x839908u};
static const uint32_t r55_exit_82988d[]={0x829890u};
static const uint32_t r55_exit_829890[]={0x8298b0u};
static const uint32_t r55_exit_8298b0[]={0x8298ceu};
static const uint32_t r55_exit_8298ce[]={0x8298e0u};
static const uint32_t r55_exit_82991a[]={0x829924u};
static const uint32_t r55_exit_829927[]={0x82992cu};
static const uint32_t r55_exit_82993b[]={0x829927u,0x82993fu};
static const uint32_t r55_exit_82993f[]={0x829969u,0x829946u};
#endif


#if defined(XBOX_PORT) && defined(ZAMN_R59_NATIVE_COLLISION_FRAGMENTS)
static bool guard_r59_fragments(Wram*w,const Rom*rom,const CosimRegs*in) {
  PortCpu c;cpu_from(in,&c);return wide(in)&&native_r59_supported(w,rom,&c);
}
static void shim_r59_fragments(Wram*w,const Rom*rom,const CosimRegs*in,CosimRegs*out) {
  PortCpu c;cpu_from(in,&c);native_r59_collision_fragments(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r59_exit_be9e[]={0x80beb4u};
static const uint32_t r59_exit_bea0[]={0x80beb4u};
static const uint32_t r59_exit_bea8[]={0x80beb4u};
static const uint32_t r59_exit_beb8[]={0x80bec4u};
static const uint32_t r59_exit_8483[]={0x808488u,0x8084b0u};
static const uint32_t r59_exit_8486[]={0x808488u,0x8084b0u};
static const uint32_t r59_exit_f25d[]={0x81f275u,0x81f27du};
static const uint32_t r59_exit_f276[]={0x81f27du};
static const uint32_t r59_exit_f27a[]={0x81f27du};
static const uint32_t r59_exit_9845[]={0x829849u};
static const uint32_t r59_exit_984d[]={0x829857u,0x8297e0u};
static const uint32_t r59_exit_984f[]={0x829857u,0x8297e0u};
static const uint32_t r59_exit_9852[]={0x829857u,0x8297e0u};
static const uint32_t r59_exit_9854[]={0x8297e0u};
#endif


#if defined(XBOX_PORT) && defined(ZAMN_R60_CONNECTED_NATIVE)
static bool guard_r60(Wram*w,const Rom*rom,const CosimRegs*in){
  PortCpu c;cpu_from(in,&c);return wide(in)&&native_r60_supported(w,rom,&c);
}
static void shim_r60(Wram*w,const Rom*rom,const CosimRegs*in,CosimRegs*out){
  PortCpu c;cpu_from(in,&c);native_r60_connected_blocks(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r60_exit_be91[]={0x80be9eu};
static const uint32_t r60_exit_be93[]={0x80be9eu};
static const uint32_t r60_exit_be95[]={0x80be9eu};
static const uint32_t r60_exit_be97[]={0x80be9eu};
static const uint32_t r60_exit_be99[]={0x80be9eu};
static const uint32_t r60_exit_be9b[]={0x80be9eu};
static const uint32_t r60_exit_f1cd[]={0x81f1d3u};
static const uint32_t r60_exit_f1d0[]={0x81f1d3u};
static const uint32_t r60_exit_993d[]={0x829927u,0x829946u,0x829969u};
static const uint32_t r60_exit_9942[]={0x829946u,0x829969u};
static const uint32_t r60_exit_9944[]={0x829946u,0x829969u};
static const uint32_t r60_exit_994a[]={0x82995eu};
static const uint32_t r60_exit_994d[]={0x82995eu};
static const uint32_t r60_exit_994f[]={0x82995eu};
static const uint32_t r60_exit_9952[]={0x82995eu};
static const uint32_t r60_exit_9955[]={0x82995eu};
static const uint32_t r60_exit_9958[]={0x82995eu};
static const uint32_t r60_exit_995b[]={0x82995eu};
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R61_THREAD_ACTOR_NATIVE)
static bool guard_r61(Wram*w,const Rom*rom,const CosimRegs*in){
  PortCpu c;cpu_from(in,&c);return wide(in)&&native_r61_supported(w,rom,&c);
}
static void shim_r61(Wram*w,const Rom*rom,const CosimRegs*in,CosimRegs*out){
  PortCpu c;cpu_from(in,&c);native_r61_thread_actor(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r61_exit_808488[]={0x80848bu};
static const uint32_t r61_exit_808489[]={0x80848bu};
static const uint32_t r61_exit_80848a[]={0x80848bu};
static const uint32_t r61_exit_8084a3[]={0x8084a4u};
static const uint32_t r61_exit_8084a5[]={0x8084a8u,0x8084aeu};
static const uint32_t r61_exit_8084a6[]={0x8084a8u,0x8084aeu};
static const uint32_t r61_exit_8084a8[]={0x8084aeu};
static const uint32_t r61_exit_8084ab[]={0x8084aeu};
static const uint32_t r61_exit_8084ae[]={0x8084b0u};
static const uint32_t r61_exit_8084af[]={0x8084b0u};
static const uint32_t r61_exit_81f1d3[]={0x81f166u,0x81f1e2u};
static const uint32_t r61_exit_81f1d5[]={0x81f166u,0x81f1e2u};
static const uint32_t r61_exit_81f1d7[]={0x81f166u,0x81f1e2u};
static const uint32_t r61_exit_81f1d9[]={0x81f166u,0x81f1e2u};
static const uint32_t r61_exit_81f1db[]={0x81f1e2u};
static const uint32_t r61_exit_81f1de[]={0x81f1e2u};
static const uint32_t r61_exit_81f1e1[]={0x81f1e2u};
static const uint32_t r61_exit_829887[]={0x829889u,0x8298e5u};
static const uint32_t r61_exit_829930[]={0x829937u};
static const uint32_t r61_exit_829933[]={0x829937u};
static const uint32_t r61_exit_829935[]={0x829937u};
static const uint32_t r61_exit_829936[]={0x829937u};
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R62_ACTOR_CONTROL_NATIVE)
static bool guard_r62(Wram*w,const Rom*rom,const CosimRegs*in){
 PortCpu c;cpu_from(in,&c);return wide(in)&&native_r62_supported(w,rom,&c);
}
static void shim_r62(Wram*w,const Rom*rom,const CosimRegs*in,CosimRegs*out){
 PortCpu c;cpu_from(in,&c);native_r62_actor_control(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r62_exit_81f175[]={0x81f1dbu,0x81f1b5u,0x81f1a9u};
static const uint32_t r62_exit_81f178[]={0x81f1dbu,0x81f1b5u,0x81f1a9u};
static const uint32_t r62_exit_81f17a[]={0x81f1dbu,0x81f1b5u,0x81f1a9u};
static const uint32_t r62_exit_81f17d[]={0x81f1b5u,0x81f1a9u};
static const uint32_t r62_exit_81f17f[]={0x81f1a9u};
static const uint32_t r62_exit_81f182[]={0x81f1a9u};
static const uint32_t r62_exit_81f184[]={0x81f1a9u};
static const uint32_t r62_exit_81f186[]={0x81f1a9u};
static const uint32_t r62_exit_81f188[]={0x81f1a9u};
static const uint32_t r62_exit_81f18a[]={0x81f1a9u};
static const uint32_t r62_exit_81f18d[]={0x81f1a9u};
static const uint32_t r62_exit_81f18f[]={0x81f1a9u};
static const uint32_t r62_exit_81f191[]={0x81f1a9u};
static const uint32_t r62_exit_81f193[]={0x81f1a9u};
static const uint32_t r62_exit_81f195[]={0x81f1a9u};
static const uint32_t r62_exit_81f198[]={0x81f1a9u};
static const uint32_t r62_exit_81f19a[]={0x81f1a9u};
static const uint32_t r62_exit_81f19c[]={0x81f1a9u};
static const uint32_t r62_exit_81f19e[]={0x81f1a9u};
static const uint32_t r62_exit_81f1a0[]={0x81f1a9u};
static const uint32_t r62_exit_81f1a3[]={0x81f1a9u};
static const uint32_t r62_exit_81f1a5[]={0x81f1a9u};
static const uint32_t r62_exit_81f1a7[]={0x81f1a9u};
static const uint32_t r62_exit_81f1a9[]={0x81f1b5u};
static const uint32_t r62_exit_81f1ab[]={0x81f1b5u};
static const uint32_t r62_exit_81f1ad[]={0x81f1b5u};
static const uint32_t r62_exit_81f1b0[]={0x81f1b5u};
static const uint32_t r62_exit_81f1b2[]={0x81f1b5u};
static const uint32_t r62_exit_81f1b5[]={0x81f1c7u,0x81f1c0u};
static const uint32_t r62_exit_81f1b7[]={0x81f1c7u,0x81f1c0u};
static const uint32_t r62_exit_81f1b8[]={0x81f1c7u,0x81f1c0u};
static const uint32_t r62_exit_81f1bb[]={0x81f1c7u,0x81f1c0u};
static const uint32_t r62_exit_81f1bd[]={0x81f1c7u,0x81f1c0u};
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R63_POSTCALL_NATIVE)
static bool guard_r63(Wram*w,const Rom*rom,const CosimRegs*in) {
 PortCpu c;cpu_from(in,&c);return wide(in)&&native_r63_supported(w,rom,&c);
}
static void shim_r63(Wram*w,const Rom*rom,const CosimRegs*in,CosimRegs*out) {
 PortCpu c;cpu_from(in,&c);native_r63_postcall(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r63_exit_81f1c4[]={0x81f1cdu};
static const uint32_t r63_exit_81f1c7[]={0x81f1cdu};
static const uint32_t r63_exit_81f1c9[]={0x81f1cdu};
static const uint32_t r63_exit_81f1ca[]={0x81f1cdu};
static const uint32_t r63_exit_81f1cb[]={0x81f1cdu};
static const uint32_t r63_exit_81f1e6[]={0x81f1e9u};
static const uint32_t r63_exit_81f1ed[]={0x81f1f7u};
static const uint32_t r63_exit_81f1ee[]={0x81f1f7u};
static const uint32_t r63_exit_81f1f1[]={0x81f1f7u};
static const uint32_t r63_exit_81f1f4[]={0x81f1f7u};
static const uint32_t r63_exit_81f1f9[]={0x81f1fbu};
static const uint32_t r63_exit_829962[]={0x829965u};
static const uint32_t r63_exit_829969[]={0x829973u};
static const uint32_t r63_exit_82996a[]={0x829973u};
static const uint32_t r63_exit_82996d[]={0x829973u};
static const uint32_t r63_exit_829970[]={0x829973u};
static const uint32_t r63_exit_829975[]={0x829977u};
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R64_ACTOR_RECORD_NATIVE)
static bool guard_r64(Wram *w,const Rom *rom,const CosimRegs *in) {
  PortCpu c;cpu_from(in,&c);
  return wide(in)&&native_r64_supported(w,rom,&c);
}
static void shim_r64(Wram *w,const Rom *rom,const CosimRegs *in,CosimRegs *out) {
  PortCpu c;cpu_from(in,&c);
  native_r64_actor_record(w,rom,&c,NULL);cpu_to(&c,out);
}
static const uint32_t r64_exit[] = {0x81f24bu};
#endif

#define BODY_SHIM(name, call, table)                                        \
  static void shim_##name(Wram* w, const Rom* rom, const CosimRegs* in,     \
                          CosimRegs* out) {                                 \
    (void)rom;                                                              \
    PortCpu c;                                                              \
    BodyWork k = {0};                                                       \
    cpu_from(in, &c);                                                       \
    call;                                                                   \
    cpu_to(&c, out);                                                        \
    R36_COSIM_COST(body_cycles(&k, table,                                       \
                           (int)(sizeof(table) / sizeof((table)[0])), in)); \
  }

BODY_SHIM(victims_start, victims_start(w, &c, &k), VICTIMS_COST)
BODY_SHIM(victims_resume, victims_resume(w, rom, &c, &k), VICTIMS_COST)
BODY_SHIM(victims_started, victims_started(w, &c, &k), VICTIMS_COST)
BODY_SHIM(victims_stopped, victims_stopped(w, &c, &k), VICTIMS_COST)
BODY_SHIM(object_resume, object_resume(w, &c, &k), OBJECT_COST)
BODY_SHIM(object_polled, object_polled(w, &c, &k), OBJECT_COST)
BODY_SHIM(object_acted, object_acted(w, &c, &k), OBJECT_COST)
BODY_SHIM(actors_checked, actors_checked(w, rom, &c, &k), ACTORS_COST)
BODY_SHIM(actors_measured, actors_measured(w, &c, &k), ACTORS_COST)
BODY_SHIM(actors_started, actors_started(w, &c, &k), ACTORS_COST)
BODY_SHIM(tile_anim_resume, tile_anim_resume(w, rom, &c, &k), TANIM_COST)
BODY_SHIM(tile_anim_queued, tile_anim_queued(w, &c, &k), TANIM_COST)
BODY_SHIM(player_ticks, player_ticks(&c, &k), PBODY_COST)
BODY_SHIM(player_state, player_state(w, &c, &k), PBODY_COST)
BODY_SHIM(player_move, player_move(w, &c, &k), PBODY_COST)
BODY_SHIM(player_buttons, player_buttons(w, &c, &k), PBODY_COST)
BODY_SHIM(player_loop, player_loop(&c, &k), PBODY_COST)
BODY_SHIM(player_branch, player_branch(w, &c, &k), PBODY_COST)
BODY_SHIM(player_hurt, player_hurt(w, &c, &k), PBODY_COST)
BODY_SHIM(player_won, player_won(w, &c, &k), PBODY_COST)
BODY_SHIM(player_dead, player_dead(w, &c, &k), PBODY_COST)

// R25 zombie hot stretches.  The two table-reading stretches require DB=$81,
// which is how this body runs in the ROM; the rest only touch the thread page
// and low WRAM.
static bool accepts_zombie_tables(const Wram* w, const CosimRegs* in) {
  return accepts_body(w, in) && in->db == 0x81;
}

BODY_SHIM(zombie_move_setup, zombie_move_setup(w, rom, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_move_after_terrain, zombie_move_after_terrain(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_move_after_actor, zombie_move_after_actor(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_move_rotate, zombie_move_rotate(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_seek, zombie_seek(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_after_nearest, zombie_after_nearest(&c, &k), ZBODY_COST)
BODY_SHIM(zombie_bearing_prep, zombie_bearing_prep(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_after_bearing, zombie_after_bearing(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_anim, zombie_anim(w, rom, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_handler_call, zombie_handler_call(w, &c, &k), ZBODY_COST)
BODY_SHIM(zombie_post_anim, zombie_post_anim(w, &c, &k), ZBODY_COST)


// ---------------------------------------------------------------------------
// $80:E4BA  player_walk -- see `port/walk.h`
// ---------------------------------------------------------------------------
//
// The walk is readable C: it calls the four tests and `step_propose` as C
// functions, and none of the ROM's instructions between `$E4BA` and the `RTS`
// run. So its price is everything the ROM would have run in that window,
// built from what the walk's log says it asked.
//
// Around each question the ROM loads the point, calls, returns and branches
// on the answer. Those are priced here from `tools/cycles816.py`, with the
// branch not taken. `WALK_BRANCH_ON_YES` says which answer takes it, for 6
// more. The tests themselves cost what their own registry entries charge, so
// a walk costs what the fifteen stretches it replaced and the calls between
// them did.
static const CosimRun WALK_AROUND[WALK_ASK_COUNT] = {
    // LDX $34 : LDY $32, JSL $80AE14, the RTL, BCC.
    [WALK_ASK_GROUND] = {56 + 54 + 42 + 12, 4 + 4 + 1 + 2, 2},
    // JSR $E739; ASL : AND #$CB38, six CMP : BNE taken, SEC, RTS; BCS.
    [WALK_ASK_REACTION] = {40 + 60 + 5 * 30 + 6 * 6 + 12 + 40 + 12,
                           3 + 9 + 5 * 5 + 1 + 1 + 2, 0},
    // LDX : LDY, JSL $80A8B3, the RTL, BCS.
    [WALK_ASK_TETHER] = {56 + 54 + 42 + 12, 4 + 4 + 1 + 2, 2},
    // LDA $08 : LDX : LDY, JSL $80BFC8, the RTL, BCC.
    [WALK_ASK_THERE] = {84 + 54 + 42 + 12, 6 + 4 + 1 + 2, 3},
    [WALK_ASK_HERE] = {84 + 54 + 42 + 12, 6 + 4 + 1 + 2, 3},
    // LDX : LDY, JSL $80B422, the RTL, BCS.
    [WALK_ASK_MAP] = {56 + 54 + 42 + 12, 4 + 4 + 1 + 2, 2},
};
static const bool WALK_BRANCH_ON_YES[WALK_ASK_COUNT] = {
    [WALK_ASK_REACTION] = true, [WALK_ASK_TETHER] = true,
    [WALK_ASK_MAP] = true};
static const CosimRun WALK_TAKEN_BRANCH = {6, 0, 0};
// BIT $54 : BPL taken, JSR $E450, and its RTS.
static const CosimRun WALK_OPENING = {40 + 6 + 40 + 40, 4 + 3 + 1, 1};
// LDA $34 : STA $30, or LDA $36 : STA $32.
static const CosimRun WALK_TAKE = {56, 4, 2};

// What a test's own registry entry charges a substituted call.
static int registry_cycles(const char* name, int* cache) {
  if (*cache < 0) {
    const CosimRoutine* r = cosim_find(name);
    *cache = r ? r->cycles : 0;
  }
  return *cache;
}

// ---------------------------------------------------------------------------
// R26: fused native Level-1 zombie paths
// ---------------------------------------------------------------------------
//
// R25 native-ported the straight-line pieces but deliberately stopped at each
// JSL. That proved the translations, but it still returned to the 65816 core
// for every helper call. R26 keeps the exact same verified C helpers and calls
// them directly from the body shim, like player_walk and monster_chase already
// do. The core sees one larger native region rather than a C -> 65816 -> C
// round-trip around each terrain/search query.
//
// The JSL itself is still part of the emulated time budget even though it no
// longer executes. 42 is the FastROM master-cycle price used by the existing
// walk/chase models; its four fetched bytes get the normal +2/byte when
// FastROM is off.
static const CosimRun ZOMBIE_NATIVE_JSL = {42, 4, 0};

static uint64_t s_r26_move_calls;
static uint64_t s_r26_move_blocked;
static uint64_t s_r26_move_actor_tests;
static uint64_t s_r26_move_occupied;
static uint64_t s_r26_move_commits;
static uint64_t s_r26_seek_calls;
static uint64_t s_r26_seek_nearest;
static uint64_t s_r26_seek_bearing;

void cosim_r26ZombieStats(uint64_t* move_calls, uint64_t* move_blocked,
                          uint64_t* move_actor_tests, uint64_t* move_occupied,
                          uint64_t* move_commits, uint64_t* seek_calls,
                          uint64_t* seek_nearest, uint64_t* seek_bearing) {
  if (move_calls) *move_calls = s_r26_move_calls;
  if (move_blocked) *move_blocked = s_r26_move_blocked;
  if (move_actor_tests) *move_actor_tests = s_r26_move_actor_tests;
  if (move_occupied) *move_occupied = s_r26_move_occupied;
  if (move_commits) *move_commits = s_r26_move_commits;
  if (seek_calls) *seek_calls = s_r26_seek_calls;
  if (seek_nearest) *seek_nearest = s_r26_seek_nearest;
  if (seek_bearing) *seek_bearing = s_r26_seek_bearing;
}

void cosim_r26ZombieStatsReset(void) {
  s_r26_move_calls = 0;
  s_r26_move_blocked = 0;
  s_r26_move_actor_tests = 0;
  s_r26_move_occupied = 0;
  s_r26_move_commits = 0;
  s_r26_seek_calls = 0;
  s_r26_seek_nearest = 0;
  s_r26_seek_bearing = 0;
}

static void shim_zombie_move_native(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  static int terrain_cost = -1;
  PortCpu c;
  BodyWork body = {0};
  int calls = 0;
  cpu_from(in, &c);
  ++s_r26_move_calls;

  // $81:85EF..$81:8617: form the proposed point.
  zombie_move_setup(w, rom, &c, &body);

  // $81:8618 JSL $80:AE97 terrain_blocked_enemy, directly in C.
  TerrainRegs ground;
  terrain_blocked_enemy(w, c.x, c.y, &ground);
  c.a = ground.a;
  c.x = ground.x;
  c.y = ground.y;
  set_nz16(&c, c.d);  // the callee's final PLD
  set_c(&c, ground.blocked);
  calls += cosim_run_cycles(&ZOMBIE_NATIVE_JSL, fetch_fast(in));
  calls += registry_cycles("terrain_blocked_enemy", &terrain_cost);

  // $81:861C... If the ground refused the step, finish the rotation natively.
  zombie_move_after_terrain(w, &c, &body);
  if (c.pc == ZOMBIE_MOVE_ROTATE_PC) {
    ++s_r26_move_blocked;
    zombie_move_rotate(w, &c, &body);
  } else {
    // $81:8627 JSL $80:BF67 actor_at_point, directly in C. This routine has a
    // board-dependent cost model, so keep its counted work rather than using
    // the registry mean.
    AtPointRegs hit;
    AtPointWork hit_work;
    actor_at_point_counted(w, c.a, c.x, c.y, &hit, &hit_work);
    ++s_r26_move_actor_tests;
    c.a = hit.a;
    c.x = hit.x;
    c.y = hit.y;
    set_nz16(&c, c.d);  // its final PLD
    set_c(&c, hit.found);
    calls += cosim_run_cycles(&ZOMBIE_NATIVE_JSL, fetch_fast(in));
    calls += at_point_cycles(&hit_work, in->fastrom);

    zombie_move_after_actor(w, &c, &body);
    if (hit.found) ++s_r26_move_occupied;
    else ++s_r26_move_commits;
  }

  cpu_to(&c, out);
  R36_COSIM_COST(body_cycles(&body, ZBODY_COST, ZBODY_BLOCK_COUNT, in) + calls);
}

static void shim_zombie_seek_native(Wram* w, const Rom* rom,
                                    const CosimRegs* in, CosimRegs* out) {
  static int bearing_cost = -1;
  PortCpu c;
  BodyWork body = {0};
  int calls = 0;
  cpu_from(in, &c);
  ++s_r26_seek_calls;

  // $81:8706 setup followed by $81:870A JSL $80:B123 actor_nearest.
  zombie_seek(w, &c, &body);
  const uint16_t point_y = c.y;
  uint16_t dist = 0;
  ActorNearestWork near_work;
  const uint16_t found = actor_nearest_counted(w, c.x, c.y, &dist, &near_work);
  ++s_r26_seek_nearest;
  c.a = dist;
  c.x = found;
  c.y = point_y;
  set_nz16(&c, c.d);  // actor_nearest's final PLD
  set_c(&c, false);
  calls += cosim_run_cycles(&ZOMBIE_NATIVE_JSL, fetch_fast(in));
  calls += nearest_cycles(&near_work, in->fastrom);

  zombie_after_nearest(&c, &body);
  if (c.pc == ZOMBIE_BEARING_PREP_PC) {
    // $81:871D JSL $80:B2A5 player_bearing, directly in C.
    zombie_bearing_prep(w, &c, &body);
    PlayerPickRegs pick;
    player_bearing(w, rom, c.a, c.x, c.y, &pick);
    ++s_r26_seek_bearing;
    c.a = pick.a;
    c.x = pick.x;
    c.y = pick.y;
    set_nz16(&c, c.d);  // player_bearing's final PLD
    set_c(&c, pick.c);
    calls += cosim_run_cycles(&ZOMBIE_NATIVE_JSL, fetch_fast(in));
    calls += registry_cycles("player_bearing", &bearing_cost);
    zombie_after_bearing(w, &c, &body);
  }

  cpu_to(&c, out);
  R36_COSIM_COST(body_cycles(&body, ZBODY_COST, ZBODY_BLOCK_COUNT, in) + calls);
}


// ---------------------------------------------------------------------------
// R27: whole zombie tick in native C
// ---------------------------------------------------------------------------
//
// R26 proved that calling the verified helpers directly is both safe and a
// little faster, but the log also proved the fusion was too small: the 65816
// still ran the dynamic behavior dispatch, the behavior body, animation and
// post-decision around every zombie tick.  R27 takes over at $81:8834 (the
// instruction after thread_yield returns) and, for the three behavior states
// reached by the Level-1 zombie, runs all the way back to the next yield setup
// in C.  Unknown behavior values deliberately fall back at $81:8837.
//
// The helpers below reproduce the *caller's* JSR/JSL/branch work as a cycle
// budget while invoking the already-verified native implementations directly.
// That keeps PPU/APU timing moving at the ROM's rate without spending host CPU
// time interpreting the skipped 65816 instructions.
#define R27_ZOMBIE_FRAME_PC 0x818834u
static const uint32_t R27_ZOMBIE_FRAME_EXITS[] = {
    ZOMBIE_YIELD_SETUP_PC + 3u,  // $81:8830 JSL thread_yield
    ZOMBIE_HANDLER_CALL_PC,      // unknown handler: old dispatch path
    ZOMBIE_SPECIAL_CALL_PC,      // rare F5F5 event
    ZOMBIE_END_PC,               // normal end/cleanup
};

static const CosimRun R27_JSR              = {40, 3, 0};
static const CosimRun R27_JSL              = {54, 4, 0};
static const CosimRun R27_RTS              = {40, 1, 0};
static const CosimRun R27_BRANCH           = {12, 2, 0};
static const CosimRun R27_TAKEN            = { 6, 0, 0};
static const CosimRun R27_JMP              = {18, 3, 0};
static const CosimRun R27_TRY_XY           = {56, 4, 2};
static const CosimRun R27_TRY_SELF_XY      = {84, 6, 3};
static const CosimRun R27_H8600_HEAD       = {316, 28, 7};
static const CosimRun R27_COMMIT           = {220, 16, 5};
static const CosimRun R27_ROTATE           = {216, 23, 3};
static const CosimRun R27_H8656_HEAD       = {384, 37, 6};
static const CosimRun R27_H8656_ADOPT      = {56, 4, 2};
static const CosimRun R27_H8656_SECOND     = {260, 24, 5};
static const CosimRun R27_H86B3_HEAD       = {56, 4, 2};
static const CosimRun R27_H86B3_CMP        = {70, 8, 1};
static const CosimRun R27_H86B3_TARGET     = {40, 3, 1};
static const CosimRun R27_TAX              = {12, 1, 0};
static const CosimRun R27_DIR_STORES       = {80, 6, 2};
static const CosimRun R27_CHASE_STEP_HEAD  = {264, 24, 6};
static const CosimRun R27_RANDOM_SETUP     = {158, 17, 2};
static const CosimRun R27_SEEK_HEAD        = {56, 4, 2};
static const CosimRun R27_CMP_IMM          = {18, 3, 0};
static const CosimRun R27_SET_CHASE        = {86, 6, 1};
static const CosimRun R27_BEARING_PREP     = {74, 7, 2};
static const CosimRun R27_DEC_DP           = {50, 2, 1};
static const CosimRun R27_HANDLER_DISPATCH = {102, 7, 1};
static const CosimRun R27_LDA_YIELD_TICKS  = {18, 3, 0};

static uint64_t s_r27_frame_calls;
static uint64_t s_r27_frame_fused;
static uint64_t s_r27_frame_to_yield;
static uint64_t s_r27_frame_unknown;
static uint64_t s_r27_h8600;
static uint64_t s_r27_h8656;
static uint64_t s_r27_h86b3;
static uint64_t s_r27_random;

void cosim_r27ZombieFrameStats(uint64_t* calls, uint64_t* fused,
                               uint64_t* to_yield, uint64_t* unknown,
                               uint64_t* h8600, uint64_t* h8656,
                               uint64_t* h86b3, uint64_t* random) {
  if (calls) *calls = s_r27_frame_calls;
  if (fused) *fused = s_r27_frame_fused;
  if (to_yield) *to_yield = s_r27_frame_to_yield;
  if (unknown) *unknown = s_r27_frame_unknown;
  if (h8600) *h8600 = s_r27_h8600;
  if (h8656) *h8656 = s_r27_h8656;
  if (h86b3) *h86b3 = s_r27_h86b3;
  if (random) *random = s_r27_random;
}

void cosim_r27ZombieFrameStatsReset(void) {
  s_r27_frame_calls = s_r27_frame_fused = s_r27_frame_to_yield = 0;
  s_r27_frame_unknown = 0;
  s_r27_h8600 = s_r27_h8656 = s_r27_h86b3 = s_r27_random = 0;
}

static int r27_cost(const CosimRun* r, const CosimRegs* in) {
  return cosim_run_cycles_dp(r, fetch_fast(in), (in->d & 0x00ffu) != 0);
}

static uint16_t r27_dp_r16(const Wram* w, const PortCpu* c, uint8_t off) {
  return wram_r16(w, (uint16_t)(c->d + off));
}
static void r27_dp_w16(Wram* w, const PortCpu* c, uint8_t off, uint16_t v) {
  wram_w16(w, (uint16_t)(c->d + off), v);
}
static void r27_lda(PortCpu* c, uint16_t v) {
  c->a = v;
  set_nz16(c, v);
}

static int r27_call_terrain(Wram* w, PortCpu* c, const CosimRegs* in) {
  static int helper = -1;
  TerrainRegs r;
  terrain_blocked_enemy(w, c->x, c->y, &r);
  c->a = r.a; c->x = r.x; c->y = r.y;
  set_nz16(c, c->d);
  set_c(c, r.blocked);
  return r27_cost(&R27_JSL, in) +
         registry_cycles("terrain_blocked_enemy", &helper);
}

static int r27_call_at_point(Wram* w, PortCpu* c, const CosimRegs* in) {
  AtPointRegs r;
  AtPointWork work;
  actor_at_point_counted(w, c->a, c->x, c->y, &r, &work);
  c->a = r.a; c->x = r.x; c->y = r.y;
  set_nz16(c, c->d);
  set_c(c, r.found);
  return r27_cost(&R27_JSL, in) + at_point_cycles(&work, in->fastrom);
}

static int r27_call_nearest(Wram* w, PortCpu* c, const CosimRegs* in) {
  uint16_t dist = 0;
  ActorNearestWork work;
  const uint16_t point_y = c->y;
  const uint16_t found = actor_nearest_counted(w, c->x, c->y, &dist, &work);
  c->a = dist; c->x = found; c->y = point_y;
  set_nz16(c, c->d);
  set_c(c, false);
  return r27_cost(&R27_JSL, in) + nearest_cycles(&work, in->fastrom);
}

static int r27_call_player_bearing(Wram* w, const Rom* rom, PortCpu* c,
                                   const CosimRegs* in) {
  static int helper = -1;
  PlayerPickRegs r;
  player_bearing(w, rom, c->a, c->x, c->y, &r);
  c->a = r.a; c->x = r.x; c->y = r.y;
  set_nz16(c, c->d);
  set_c(c, r.c);
  return r27_cost(&R27_JSL, in) + registry_cycles("player_bearing", &helper);
}

static int r27_call_snap(Wram* w, PortCpu* c, const CosimRegs* in) {
  static int helper = -1;
  ActorSnapRegs r;
  actor_snap_to(w, c->x, c->y, &r);
  c->a = r.a;
  c->p = (uint8_t)(c->p & ~(PORT_P_N | PORT_P_Z | PORT_P_C));
  if (r.n) c->p |= PORT_P_N;
  if (r.z) c->p |= PORT_P_Z;
  if (r.c) c->p |= PORT_P_C;
  return r27_cost(&R27_JSL, in) + registry_cycles("actor_snap_to", &helper);
}

static int r27_call_actor_bearing(Wram* w, const Rom* rom, PortCpu* c,
                                  const CosimRegs* in) {
  static int helper = -1;
  ActorBearingRegs r;
  const uint16_t to = c->y;
  actor_bearing(w, rom, c->x, c->y, &r);
  c->a = r.a; c->x = r.x; c->y = to;
  set_nz16(c, c->d);
  set_c(c, r.c);
  return r27_cost(&R27_JSL, in) + registry_cycles("actor_bearing", &helper);
}

static int r27_call_rng(Wram* w, PortCpu* c, const CosimRegs* in) {
  static int helper = -1;
  RngResult r;
  rng_next(w, flag(c, PORT_P_C), &r);
  c->a = r.a;
  c->p = (uint8_t)(c->p & ~(PORT_P_N | PORT_P_Z | PORT_P_C | PORT_P_V));
  if (r.n) c->p |= PORT_P_N;
  if (r.z) c->p |= PORT_P_Z;
  if (r.c) c->p |= PORT_P_C;
  if (r.v) c->p |= PORT_P_V;
  return r27_cost(&R27_JSL, in) + registry_cycles("rng_next", &helper);
}

// $81:85D3: test the proposed point.  This includes the local RTS, so callers
// only add the JSR that entered it.
static int r27_try_position(Wram* w, PortCpu* c, const CosimRegs* in) {
  int cycles = r27_cost(&R27_TRY_XY, in);
  c->x = r27_dp_r16(w, c, 0x1a); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x1c); set_nz16(c, c->y);
  cycles += r27_call_terrain(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C)) {
    cycles += r27_cost(&R27_TAKEN, in) + r27_cost(&R27_RTS, in);
    return cycles;
  }

  cycles += r27_cost(&R27_TRY_SELF_XY, in);
  r27_lda(c, r27_dp_r16(w, c, 0x08));
  c->x = r27_dp_r16(w, c, 0x1a); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x1c); set_nz16(c, c->y);
  cycles += r27_call_at_point(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C)) cycles += r27_cost(&R27_TAKEN, in);
  cycles += r27_cost(&R27_RTS, in);
  return cycles;
}

static int r27_rotate(Wram* w, PortCpu* c, const CosimRegs* in) {
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->a = (uint16_t)(c->a - 1u); set_nz16(c, c->a);
  c->a = (uint16_t)(c->a - 1u); set_nz16(c, c->a);
  set_c(c, false);
  c->a = adc16(c, c->a, 0x0004u);
  c->a &= 0x000fu; set_nz16(c, c->a);
  c->a = (uint16_t)(c->a + 1u); set_nz16(c, c->a);
  c->a = (uint16_t)(c->a + 1u); set_nz16(c, c->a);
  r27_dp_w16(w, c, 0x0e, c->a);
  r27_lda(c, 0x8656u);
  r27_dp_w16(w, c, 0x14, c->a);
  return r27_cost(&R27_ROTATE, in) + r27_cost(&R27_RTS, in);
}

static int r27_commit_point(Wram* w, PortCpu* c, const CosimRegs* in) {
  c->y = r27_dp_r16(w, c, 0x08); set_nz16(c, c->y);
  r27_lda(c, r27_dp_r16(w, c, 0x1a));
  r27_dp_w16(w, c, 0x16, c->a);
  wram_w16(w, (uint16_t)(c->y + 0x0002u), c->a);
  r27_lda(c, r27_dp_r16(w, c, 0x1c));
  r27_dp_w16(w, c, 0x18, c->a);
  wram_w16(w, (uint16_t)(c->y + 0x0006u), c->a);
  return r27_cost(&R27_COMMIT, in) + r27_cost(&R27_RTS, in);
}

static int r27_handler_8600(Wram* w, const Rom* rom, PortCpu* c,
                            const CosimRegs* in) {
  int cycles = r27_cost(&R27_H8600_HEAD, in);
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->a = asl16(c, c->a);
  c->x = c->a; set_nz16(c, c->x);
  r27_lda(c, r27_dp_r16(w, c, 0x16));
  set_c(c, false);
  c->a = adc16(c, c->a, bus_r16(w, rom, ZOMBIE_DELTA_X_TABLE + c->x));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, r27_dp_r16(w, c, 0x18));
  set_c(c, false);
  c->a = adc16(c, c->a, bus_r16(w, rom, ZOMBIE_DELTA_Y_TABLE + c->x));
  r27_dp_w16(w, c, 0x1c, c->a);
  c->x = r27_dp_r16(w, c, 0x1a); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x1c); set_nz16(c, c->y);

  cycles += r27_call_terrain(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in); // BCC $8621
  if (flag(c, PORT_P_C)) {
    cycles += r27_cost(&R27_JMP, in);  // JMP $863E
    return cycles + r27_rotate(w, c, in);
  }
  cycles += r27_cost(&R27_TAKEN, in);

  cycles += r27_cost(&R27_TRY_SELF_XY, in);
  r27_lda(c, r27_dp_r16(w, c, 0x08));
  c->x = r27_dp_r16(w, c, 0x1a); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x1c); set_nz16(c, c->y);
  cycles += r27_call_at_point(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in); // BCS $863D
  if (flag(c, PORT_P_C))
    return cycles + r27_cost(&R27_TAKEN, in) + r27_cost(&R27_RTS, in);
  return cycles + r27_commit_point(w, c, in);
}

static int r27_handler_8656(Wram* w, const Rom* rom, PortCpu* c,
                            const CosimRegs* in) {
  int cycles = r27_cost(&R27_H8656_HEAD, in);
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->a = (uint16_t)(c->a - 1u); set_nz16(c, c->a);
  c->a = (uint16_t)(c->a - 1u); set_nz16(c, c->a);
  set_c(c, true);
  c->a = sbc16(c, c->a, 0x0004u);
  c->a &= 0x000fu; set_nz16(c, c->a);
  c->a = (uint16_t)(c->a + 1u); set_nz16(c, c->a);
  c->a = (uint16_t)(c->a + 1u); set_nz16(c, c->a);
  r27_dp_w16(w, c, 0x10, c->a);
  c->a = asl16(c, c->a);
  c->x = c->a; set_nz16(c, c->x);
  r27_lda(c, bus_r16(w, rom, ZOMBIE_DELTA_X_TABLE + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x16));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, bus_r16(w, rom, ZOMBIE_DELTA_Y_TABLE + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x18));
  r27_dp_w16(w, c, 0x1c, c->a);

  cycles += r27_cost(&R27_JSR, in) + r27_try_position(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in); // BCS $8680
  if (!flag(c, PORT_P_C)) {
    cycles += r27_cost(&R27_H8656_ADOPT, in);
    r27_lda(c, r27_dp_r16(w, c, 0x10));
    r27_dp_w16(w, c, 0x0e, c->a);
  } else {
    cycles += r27_cost(&R27_TAKEN, in);
  }

  cycles += r27_cost(&R27_H8656_SECOND, in);
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->a = asl16(c, c->a);
  c->x = c->a; set_nz16(c, c->x);
  r27_lda(c, bus_r16(w, rom, ZOMBIE_DELTA_X_TABLE + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x16));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, bus_r16(w, rom, ZOMBIE_DELTA_Y_TABLE + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x18));
  r27_dp_w16(w, c, 0x1c, c->a);

  cycles += r27_cost(&R27_JSR, in) + r27_try_position(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in); // BCS $86AA
  if (flag(c, PORT_P_C)) {
    cycles += r27_cost(&R27_TAKEN, in) + r27_cost(&R27_JMP, in);
    return cycles + r27_rotate(w, c, in);
  }
  return cycles + r27_commit_point(w, c, in);
}

static int r27_chase_step(Wram* w, const Rom* rom, PortCpu* c,
                          const CosimRegs* in) {
  int cycles = r27_cost(&R27_CHASE_STEP_HEAD, in);
  c->x = r27_dp_r16(w, c, 0x20); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x08); set_nz16(c, c->y);
  r27_lda(c, bus_r16(w, rom, ZOMBIE_DELTA_X_TABLE + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x16));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, bus_r16(w, rom, ZOMBIE_DELTA_Y_TABLE + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x18));
  r27_dp_w16(w, c, 0x1c, c->a);

  cycles += r27_cost(&R27_JSR, in) + r27_try_position(w, c, in);
  cycles += r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C))
    return cycles + r27_cost(&R27_TAKEN, in) + r27_cost(&R27_RTS, in);
  return cycles + r27_commit_point(w, c, in);
}

static int r27_random_move(Wram* w, const Rom* rom, PortCpu* c,
                           const CosimRegs* in) {
  ++s_r27_random;
  int cycles = r27_call_rng(w, c, in) + r27_cost(&R27_RANDOM_SETUP, in);
  c->a &= 0x0003u; set_nz16(c, c->a);
  c->a = asl16(c, c->a);
  c->a = asl16(c, c->a);
  c->a = (uint16_t)(c->a + 1u); set_nz16(c, c->a);
  c->a = (uint16_t)(c->a + 1u); set_nz16(c, c->a);
  r27_dp_w16(w, c, 0x0e, c->a);
  r27_lda(c, 0x8600u);
  r27_dp_w16(w, c, 0x14, c->a);
  return cycles + r27_handler_8600(w, rom, c, in);
}

static int r27_handler_86b3(Wram* w, const Rom* rom, PortCpu* c,
                            const CosimRegs* in) {
  int cycles = r27_cost(&R27_H86B3_HEAD, in);
  c->x = r27_dp_r16(w, c, 0x16); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x18); set_nz16(c, c->y);
  cycles += r27_call_nearest(w, c, in);

  r27_dp_w16(w, c, 0x2e, c->x);
  cmp16(c, c->a, 0x0046u);
  cycles += r27_cost(&R27_H86B3_CMP, in);
  if (flag(c, PORT_P_C)) {
    cycles += r27_cost(&R27_TAKEN, in) + r27_cost(&R27_JMP, in);
    return cycles + r27_random_move(w, rom, c, in);
  }

  cycles += r27_cost(&R27_H86B3_TARGET, in);
  c->y = c->x; set_nz16(c, c->y);
  c->x = r27_dp_r16(w, c, 0x08); set_nz16(c, c->x);
  cycles += r27_call_snap(w, c, in);
  cycles += r27_call_actor_bearing(w, rom, c, in);

  cycles += r27_cost(&R27_TAX, in) + r27_cost(&R27_BRANCH, in);
  c->x = c->a; set_nz16(c, c->x);
  if (c->x == 0) {
    cycles += r27_cost(&R27_TAKEN, in) + r27_cost(&R27_JMP, in);
    return cycles + r27_random_move(w, rom, c, in);
  }

  cycles += r27_cost(&R27_DIR_STORES, in);
  c->a = asl16(c, c->a);
  r27_dp_w16(w, c, 0x0e, c->a);
  c->a = asl16(c, c->a);
  r27_dp_w16(w, c, 0x20, c->a);

  // JSR $86D9 deliberately returns to $86D9 once, so the movement step runs
  // twice before the handler finally returns to $883F.
  cycles += r27_cost(&R27_JSR, in);
  cycles += r27_chase_step(w, rom, c, in);
  cycles += r27_chase_step(w, rom, c, in);
  return cycles;
}

// Direct version of $81:8706 including its final RTS.
static int r27_seek_inline(Wram* w, const Rom* rom, PortCpu* c,
                           const CosimRegs* in) {
  int cycles = r27_cost(&R27_SEEK_HEAD, in);
  c->x = r27_dp_r16(w, c, 0x16); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x18); set_nz16(c, c->y);
  cycles += r27_call_nearest(w, c, in);

  cycles += r27_cost(&R27_CMP_IMM, in) + r27_cost(&R27_BRANCH, in);
  cmp16(c, c->a, 0x0041u);
  if (!flag(c, PORT_P_C)) {
    cycles += r27_cost(&R27_JMP, in) + r27_cost(&R27_SET_CHASE, in);
    r27_lda(c, 0x86b3u);
    r27_dp_w16(w, c, 0x14, c->a);
    return cycles;
  }
  cycles += r27_cost(&R27_TAKEN, in);

  cycles += r27_cost(&R27_BEARING_PREP, in);
  r27_lda(c, 0x00d0u);
  c->x = r27_dp_r16(w, c, 0x16); set_nz16(c, c->x);
  c->y = r27_dp_r16(w, c, 0x18); set_nz16(c, c->y);
  cycles += r27_call_player_bearing(w, rom, c, in);
  cycles += r27_cost(&R27_TAX, in) + r27_cost(&R27_BRANCH, in);
  c->x = c->a; set_nz16(c, c->x);
  if (c->x != 0) {
    cycles += r27_cost(&R27_TAKEN, in);
  } else {
    cycles += r27_cost(&R27_DEC_DP, in);
    const uint16_t v = (uint16_t)(r27_dp_r16(w, c, 0x12) - 1u);
    r27_dp_w16(w, c, 0x12, v);
    set_nz16(c, v);
  }
  return cycles + r27_cost(&R27_RTS, in);
}

static void shim_zombie_frame_native(Wram* w, const Rom* rom,
                                     const CosimRegs* in, CosimRegs* out) {
  PortCpu c;
  cpu_from(in, &c);
  ++s_r27_frame_calls;

  // $8834 JSR $8706 and the whole seek routine, including its RTS.
  int cycles = r27_cost(&R27_JSR, in) + r27_seek_inline(w, rom, &c, in);

  const uint16_t handler = r27_dp_r16(w, &c, 0x14);
  if (handler != 0x8600u && handler != 0x8656u && handler != 0x86b3u) {
    ++s_r27_frame_unknown;
    c.pc = ZOMBIE_HANDLER_CALL_PC;
    cpu_to(&c, out);
    R36_COSIM_COST(cycles);
    return;
  }

  ++s_r27_frame_fused;
  // $8837 PEA $883E : LDA $14 : DEC : PHA : RTS.  The temporary stack words
  // are both consumed by dispatch+handler RTS, so a direct C call leaves S at
  // exactly the same value; only A/N/Z at handler entry need reproducing.
  cycles += r27_cost(&R27_HANDLER_DISPATCH, in) + r27_cost(&R27_RTS, in);
  r27_lda(&c, handler);
  c.a = (uint16_t)(c.a - 1u); set_nz16(&c, c.a);

  if (handler == 0x8600u) {
    ++s_r27_h8600;
    cycles += r27_handler_8600(w, rom, &c, in);
  } else if (handler == 0x8656u) {
    ++s_r27_h8656;
    cycles += r27_handler_8656(w, rom, &c, in);
  } else {
    ++s_r27_h86b3;
    cycles += r27_handler_86b3(w, rom, &c, in);
  }

  // $883F JSR $8736; native animation body; its RTS.
  BodyWork anim = {0};
  cycles += r27_cost(&R27_JSR, in);
  zombie_anim(w, rom, &c, &anim);
  cycles += body_cycles(&anim, ZBODY_COST, ZBODY_BLOCK_COUNT, in);
  cycles += r27_cost(&R27_RTS, in);

  // $8842 post-decision.  Common path jumps back to $882D; absorb only its
  // LDA #$0002 and stop on the thread_yield JSL itself. Rare event/cleanup
  // paths remain ROM/native-registry fallbacks.
  BodyWork post = {0};
  zombie_post_anim(w, &c, &post);
  cycles += body_cycles(&post, ZBODY_COST, ZBODY_BLOCK_COUNT, in);
  if (c.pc == ZOMBIE_YIELD_SETUP_PC) {
    r27_lda(&c, 0x0002u);
    cycles += r27_cost(&R27_LDA_YIELD_TICKS, in);
    c.pc = 0x818830u;
    ++s_r27_frame_to_yield;
  }

  cpu_to(&c, out);
  R36_COSIM_COST(cycles);
}

// R29: $81:8A5C and the wander/turn states it installs. All nested calls are C.
// Keep their actual RTS boundaries, including the blocked-turn RTS at $8A71.
static uint32_t s_r29_chase_calls, s_r29_chase_native, s_r29_chase_fallback;
static uint32_t s_r29_actor_calls, s_r29_actor_native, s_r29_actor_fallback;

void cosim_r29ActorStats(uint32_t* n) {
  n[0] = s_r29_chase_calls; n[1] = s_r29_chase_native;
  n[2] = s_r29_chase_fallback; n[3] = s_r29_actor_calls;
  n[4] = s_r29_actor_native; n[5] = s_r29_actor_fallback;
  s_r29_chase_calls = s_r29_chase_native = s_r29_chase_fallback = 0;
  s_r29_actor_calls = s_r29_actor_native = s_r29_actor_fallback = 0;
}

static bool accepts_enemy_8a5c(const Wram* w, const CosimRegs* in) {
  const uint16_t facing = wram_r16(w, (uint16_t)(in->d + 0x0e));
  const uint16_t rec = wram_r16(w, (uint16_t)(in->d + 0x08));
  const bool ok = body_ok(in) && in->d >= 0x100 && in->db == 0x81 &&
      rec >= W_ACTOR_SLOTS && rec < W_ACTOR_SLOTS + ACTOR_SLOT_COUNT * ACTOR_SLOT_STRIDE &&
      (rec - W_ACTOR_SLOTS) % ACTOR_SLOT_STRIDE == 0 &&
      (in->pc == 0x818a5cu || (facing >= 2 && facing <= 16 && !(facing & 1)));
  if (!ok) { ++s_r29_actor_calls; ++s_r29_actor_fallback; }
  return ok;
}

// These new paths claim all flags, so publish helpers' V as well as N/Z/C.
// Existing R27 wrappers and runtime accounting are deliberately unchanged.
// Jump-style cosim compares even consumed stack bytes. Reproduce the small
// JSL/PHD residue without changing the live stack or executing a CPU opcode.
static void r29_helper_stack(Wram* w, const PortCpu* c, uint16_t ret) {
  PortCpu stack = *c;
  push8(w, &stack, 0x81); push16(w, &stack, ret); push16(w, &stack, c->d);
}

static int r29_terrain(Wram* w, PortCpu* c, const CosimRegs* in, uint16_t ret) {
  r29_helper_stack(w, c, ret);
  wram_w16(w, (uint16_t)(c->s - 6u), (uint16_t)(c->x - 9u));
  static int helper = -1;
  TerrainRegs r;
  terrain_blocked_enemy(w, c->x, c->y, &r);
  c->a = r.a; c->x = r.x; c->y = r.y;
  set_nz16(c, c->d); set_c(c, r.blocked); set_v(c, r.v);
  return r27_cost(&R27_JSL, in) + registry_cycles("terrain_blocked_enemy", &helper);
}

static int r29_at_point(Wram* w, PortCpu* c, const CosimRegs* in, uint16_t ret) {
  r29_helper_stack(w, c, ret);
  wram_w16(w, (uint16_t)(c->s - 6u), 0); // PEA $0000 before PLD
  AtPointRegs r;
  AtPointWork work;
  actor_at_point_counted(w, c->a, c->x, c->y, &r, &work);
  c->a = r.a; c->x = r.x; c->y = r.y;
  set_nz16(c, c->d); set_c(c, r.found);
  if (r.v_set) set_v(c, r.v);
  return r27_cost(&R27_JSL, in) + at_point_cycles(&work, in->fastrom);
}

static int r29_try_position(Wram* w, PortCpu* c, const CosimRegs* in) {
  int cycles = r27_cost(&R27_TRY_XY, in);
  c->x = r27_dp_r16(w, c, 0x1a); c->y = r27_dp_r16(w, c, 0x1c);
  cycles += r29_terrain(w, c, in, 0x85da) + r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C))
    return cycles + r27_cost(&R27_TAKEN, in) + r27_cost(&R27_RTS, in);
  cycles += r27_cost(&R27_TRY_SELF_XY, in);
  r27_lda(c, r27_dp_r16(w, c, 0x08));
  c->x = r27_dp_r16(w, c, 0x1a); c->y = r27_dp_r16(w, c, 0x1c);
  cycles += r29_at_point(w, c, in, 0x85e6) + r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C)) cycles += r27_cost(&R27_TAKEN, in);
  return cycles + r27_cost(&R27_RTS, in);
}

static int r29_turn(Wram* w, PortCpu* c, const CosimRegs* in) {
  static const CosimRun own = {244, 22, 4}; // $8A4B..$8A70
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->a = (uint16_t)(c->a - 2u);
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x2c));
  c->a = (uint16_t)((c->a & 15u) + 2u);
  r27_dp_w16(w, c, 0x0e, c->a);
  r27_lda(c, 0x8a72u); r27_dp_w16(w, c, 0x14, c->a);
  c->pc = 0x818a71u;
  return r27_cost(&own, in) + r27_cost(&R27_RTS, in);
}

static int r29_wander(Wram* w, const Rom* rom, PortCpu* c, const CosimRegs* in) {
  int cycles = r27_cost(&R27_H8600_HEAD, in);
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->x = c->a = asl16(c, c->a);
  r27_lda(c, r27_dp_r16(w, c, 0x16));
  set_c(c, false); c->a = adc16(c, c->a, bus_r16(w, rom, 0x8185afu + c->x));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, r27_dp_r16(w, c, 0x18));
  set_c(c, false); c->a = adc16(c, c->a, bus_r16(w, rom, 0x8185b1u + c->x));
  r27_dp_w16(w, c, 0x1c, c->a);
  c->x = r27_dp_r16(w, c, 0x1a); c->y = r27_dp_r16(w, c, 0x1c);
  cycles += r29_terrain(w, c, in, 0x89da) + r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C))
    return cycles + r27_cost(&R27_JMP, in) + r29_turn(w, c, in);
  cycles += r27_cost(&R27_TAKEN, in) + r27_cost(&R27_TRY_SELF_XY, in);
  r27_lda(c, r27_dp_r16(w, c, 0x08));
  c->x = r27_dp_r16(w, c, 0x1a); c->y = r27_dp_r16(w, c, 0x1c);
  cycles += r29_at_point(w, c, in, 0x89e9) + r27_cost(&R27_BRANCH, in);
  c->pc = 0x8189fcu;
  if (flag(c, PORT_P_C))
    return cycles + r27_cost(&R27_TAKEN, in) + r27_cost(&R27_RTS, in);
  return cycles + r27_commit_point(w, c, in);
}

static int r29_follow_wall(Wram* w, const Rom* rom, PortCpu* c, const CosimRegs* in) {
  // $8A72 differs from $8656 by DEC $24 and SBC $2C (rather than #4).
  static const CosimRun head = {444, 38, 8};
  int cycles = r27_cost(&head, in);
  r27_dp_w16(w, c, 0x24, (uint16_t)(r27_dp_r16(w, c, 0x24) - 1u));
  r27_lda(c, r27_dp_r16(w, c, 0x0e));
  c->a = (uint16_t)(c->a - 2u);
  set_c(c, true); c->a = sbc16(c, c->a, r27_dp_r16(w, c, 0x2c));
  c->a = (uint16_t)((c->a & 15u) + 2u);
  r27_dp_w16(w, c, 0x10, c->a);
  c->x = c->a = asl16(c, c->a);
  r27_lda(c, bus_r16(w, rom, 0x8185afu + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x16));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, bus_r16(w, rom, 0x8185b1u + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x18));
  r27_dp_w16(w, c, 0x1c, c->a);
  push16(w, c, 0x8a96);
  cycles += r27_cost(&R27_JSR, in) + r29_try_position(w, c, in);
  c->s = (uint16_t)(c->s + 2u);
  cycles += r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C)) cycles += r27_cost(&R27_TAKEN, in);
  else {
    cycles += r27_cost(&R27_H8656_ADOPT, in);
    r27_lda(c, r27_dp_r16(w, c, 0x10)); r27_dp_w16(w, c, 0x0e, c->a);
  }
  cycles += r27_cost(&R27_H8656_SECOND, in);
  r27_lda(c, r27_dp_r16(w, c, 0x0e)); c->x = c->a = asl16(c, c->a);
  r27_lda(c, bus_r16(w, rom, 0x8185afu + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x16));
  r27_dp_w16(w, c, 0x1a, c->a);
  r27_lda(c, bus_r16(w, rom, 0x8185b1u + c->x));
  set_c(c, false); c->a = adc16(c, c->a, r27_dp_r16(w, c, 0x18));
  r27_dp_w16(w, c, 0x1c, c->a);
  push16(w, c, 0x8ab3);
  cycles += r27_cost(&R27_JSR, in) + r29_try_position(w, c, in);
  c->s = (uint16_t)(c->s + 2u);
  cycles += r27_cost(&R27_BRANCH, in);
  if (flag(c, PORT_P_C))
    return cycles + r27_cost(&R27_TAKEN, in) + r27_cost(&R27_JMP, in) + r29_turn(w, c, in);
  c->pc = 0x818ac6u;
  return cycles + r27_commit_point(w, c, in);
}

static const uint32_t R29_ENEMY_EXITS[] = {0x8189fcu, 0x818a71u, 0x818ac6u};
static void shim_enemy_8a5c_native(Wram* w, const Rom* rom,
                                   const CosimRegs* in, CosimRegs* out) {
  PortCpu c;
  cpu_from(in, &c);
  ++s_r29_actor_calls;
  int cycles = 0;
  if (in->pc == 0x818a5cu) {
    // RNG, the random facing, JMP $89BA and installation of the wander state.
    cycles += r27_call_rng(w, &c, in) + r27_cost(&R27_RANDOM_SETUP, in);
    c.a &= 3u; c.a = asl16(&c, c.a); c.a = asl16(&c, c.a);
    c.a = (uint16_t)(c.a + 2u); r27_dp_w16(w, &c, 0x0e, c.a);
    r27_lda(&c, 0x89bfu); r27_dp_w16(w, &c, 0x14, c.a);
  }
  cycles += in->pc == 0x818a72u ? r29_follow_wall(w, rom, &c, in)
                                 : r29_wander(w, rom, &c, in);
  ++s_r29_actor_native;
  cpu_to(&c, out);
  R36_COSIM_COST(cycles - r27_cost(&R27_RTS, in));
}


static int walk_cycles(const WalkLog* log, const CosimRegs* in) {
  static int propose = -1, ground = -1, tether = -1, map = -1;
  static const char* const TEST[WALK_ASK_COUNT] = {
      [WALK_ASK_GROUND] = "terrain_blocked",
      [WALK_ASK_TETHER] = "step_tether_blocked",
      [WALK_ASK_MAP] = "terrain_out_of_bounds"};
  int* const cache[WALK_ASK_COUNT] = {
      [WALK_ASK_GROUND] = &ground, [WALK_ASK_TETHER] = &tether,
      [WALK_ASK_MAP] = &map};

  const bool fast = fetch_fast(in);
  const bool unaligned = (in->d & 0x00ffu) != 0;
  int cycles = cosim_run_cycles_dp(&WALK_OPENING, fast, unaligned) +
               registry_cycles("step_propose", &propose);
  for (int q = 0; q < WALK_ASK_COUNT; q++) {
    for (int yes = 0; yes < 2; yes++) {
      const int n = log->asked[q][yes];
      if (n == 0) continue;
      int each = cosim_run_cycles_dp(&WALK_AROUND[q], fast, unaligned);
      if (WALK_BRANCH_ON_YES[q] == (yes != 0))
        each += cosim_run_cycles(&WALK_TAKEN_BRANCH, fast);
      if (TEST[q]) each += registry_cycles(TEST[q], cache[q]);
      cycles += n * each;
    }
  }
  cycles += log->taken * cosim_run_cycles_dp(&WALK_TAKE, fast, unaligned);
  return cycles + obstacle_cycles(&log->obstacle, in->fastrom);
}

// The tests put their scratch on page zero, so a player's page there would
// have them writing its fields. A player's page is never there.
static bool accepts_player_walk(const Wram* w, const CosimRegs* in) {
  (void)w;
  return body_ok(in) && in->d >= 0x0100;
}

static bool supported_player_walk(Wram* scratch, const Rom* rom,
                                  const CosimRegs* in) {
  return walk_supported(scratch, rom, in->d);
}

static bool supported_player_walk_readonly(Wram* live, const Rom* rom,
                                           const CosimRegs* in) {
  return walk_supported_readonly((const Wram*)live, rom, in->d);
}

// Of the registers, only carry and overflow outlive the frame: nothing reads
// A, X, Y, N or Z before `thread_yield`, and its `PHP` keeps the other two in
// the thread's parked status byte. Carry is the last test's answer, overflow
// the last add's. This said overflow was always clear, until levels 19 and 25
// set it on half their walks: a tilemap row based under `$8000` whose tile
// lies past it.
static void shim_player_walk(Wram* w, const Rom* rom, const CosimRegs* in,
                             CosimRegs* out) {
  WalkLog log = {0};
  player_walk(w, rom, in->d, &log);
  R36_COSIM_COST(walk_cycles(&log, in));
  out->c = log.last_yes;
  out->v = log.overflow;
  out->flags = COSIM_FLAG_C | COSIM_FLAG_V;
  out->regs = 0;
}

// ---------------------------------------------------------------------------
// $81:BEE3  monster_chase -- see `port/chase.h`
// ---------------------------------------------------------------------------
//
// Priced the walk's way: what the ROM would have run between `$BEE3` and the
// `RTS`, from what the log says happened. Each run of the chase's own
// instructions is from `tools/cycles816.py` with the data bank at `$81`,
// branches not taken, and a taken branch adds 6. The calls cost a `JSL` here
// and their callee's own figure, which includes its `RTL`: the counted models
// for the scan and the actor test, the registry's means for the rest.
static void chase_add(CosimRun* t, int cycles, int bytes, int dp) {
  t->cycles += cycles;
  t->bytes += bytes;
  t->dp += dp;
}

static int chase_cycles(const ChaseLog* log, const CosimRegs* in) {
  static int snap = -1, bearing = -1, rng = -1, ground = -1, tile = -1;
  const int TAKEN = 6;
  CosimRun own = {0, 0, 0};
  int calls = nearest_cycles(&log->nearest, in->fastrom);

  // LDX $0A : LDY $0C : JSL actor_nearest : CMP #$00B4 : BCC.
  chase_add(&own, 140, 13, 2);
  if (log->gave_up) {
    // JMP $BF99 : JMP $BCE1, JSL rng_next : AND : ASL ASL INC INC : STA $14 :
    // JMP $BE0E, LDA #$BE14 : STA $12 : RTS.
    chase_add(&own, 18 + 18 + 166 + 86, 3 + 3 + 16 + 6, 2);
    calls += registry_cycles("rng_next", &rng);
    return calls + cosim_run_cycles_dp(&own, fetch_fast(in),
                                       (in->d & 0x00ffu) != 0);
  }
  own.cycles += TAKEN;

  // STX $24 : TXY : LDX $08 : JSL actor_snap_to : JSL actor_bearing, then
  // PHA, the record's X and Y into $0A/$0C, PLA : STA $18 : AND #1 : BNE.
  chase_add(&own, 176 + 284, 13 + 21, 2 + 4);
  calls += registry_cycles("actor_snap_to", &snap) +
           registry_cycles("actor_bearing", &bearing);
  if (log->straight) {
    own.cycles += TAKEN;
  } else {
    // Both gaps and their halves, each negated when it was negative; a gap
    // that was not skips the `EOR : INC` on a taken `BPL`.
    chase_add(&own, 408, 36, 7);
    if (!log->gap_x_negative) chase_add(&own, TAKEN - 30, -4, 0);
    if (!log->gap_y_negative) chase_add(&own, TAKEN - 30, -4, 0);
    // CMP $0E : BCC, then LDY #n : LDA $34 or $36 : BMI, and the other
    // `LDY` when the `BMI` is not taken.
    chase_add(&own, 40 + 18 + 28 + 12, 4 + 3 + 2 + 2, 2);
    const bool took_bmi = log->across ? log->gap_x_negative : log->gap_y_negative;
    if (!log->across) own.cycles += TAKEN;
    if (took_bmi) own.cycles += TAKEN;
    else chase_add(&own, 18 + (log->across ? 12 + TAKEN : 0),
                   3 + (log->across ? 2 : 0), 0);  // ...and `BRA` after #3
    chase_add(&own, 28, 2, 1);  // STY $18
  }

  // LDA $18 : ASL : STA $14 : ASL : STA $18 : JSL rng_next : AND #2 : BEQ,
  // and on a fast frame LDA #$B9B5 : STA $38.
  chase_add(&own, 162 + 30, 12 + 5, 3);
  calls += registry_cycles("rng_next", &rng);
  if (log->fast) chase_add(&own, 46, 5, 1);
  else own.cycles += TAKEN;

  // The point, the JSR to `$BC05`, and there the ground test.
  chase_add(&own, 348 + 168, 27 + 15, 8 + 3);
  calls += registry_cycles("terrain_blocked_enemy", &ground);
  if (log->asked_actors) {
    chase_add(&own, 200, 14, 4);
    calls += at_point_cycles(&log->at_point, in->fastrom);
    if (log->outcome != CHASE_STEPPED) own.cycles += TAKEN;
  } else {
    own.cycles += TAKEN;
  }
  chase_add(&own, 40 + 12, 1 + 2, 0);  // RTS, BCS
  if (log->outcome == CHASE_STEPPED) {
    // The commit and the `RTS`.
    chase_add(&own, 316, 21, 7);
  } else {
    own.cycles += TAKEN;
    chase_add(&own, 40, 4, 1);  // LDA $32 : BNE
    if (log->outcome == CHASE_MET_SOMEONE) {
      own.cycles += TAKEN;
    } else {
      // JSR $BC3D, a probe or two, and the landing if one had bit 13.
      chase_add(&own, 40 + 368, 3 + 35, 5);
      calls += registry_cycles("tile_attrs_at_pixel", &tile);
      if (log->leap_probes == 2) {
        chase_add(&own, 392, 37, 5);
        calls += registry_cycles("tile_attrs_at_pixel", &tile);
      }
      if (log->leap_found) {
        own.cycles += TAKEN;
        chase_add(&own, 294 + (log->outcome == CHASE_LEAPT ? 0 : TAKEN), 28, 3);
        calls += registry_cycles("terrain_blocked_enemy", &ground);
      }
      if (log->outcome == CHASE_LEAPT) {
        // CLC : RTS, untaken BCS, JMP $BCF1; leave leap setup to the ROM.
        chase_add(&own, 52 + 12 + 18, 2 + 2 + 3, 0);
        return calls + cosim_run_cycles_dp(&own, fetch_fast(in),
                                           (in->d & 0x00ffu) != 0);
      }
      chase_add(&own, 52 + 12 + TAKEN, 2 + 2, 0);  // SEC : RTS, BCS taken
    }
    chase_add(&own, 96, 5, 2);  // LDA $3A : STA $38 : RTS
  }
  return calls + cosim_run_cycles_dp(&own, fetch_fast(in),
                                     (in->d & 0x00ffu) != 0);
}

// The tests put their scratch on page zero, as for the walk, and the tables
// are read through the data bank, which is the thread's own.
static bool accepts_monster_chase(const Wram* w, const CosimRegs* in) {
  const uint16_t steps = wram_r16(w, (uint16_t)(in->d + CHASE_DP_STEPS));
  const uint16_t usual = wram_r16(w, (uint16_t)(in->d + CHASE_DP_USUAL_STEPS));
  const uint16_t rec = wram_r16(w, (uint16_t)(in->d + MONSTER_DP_RECORD));
  const bool ok = body_ok(in) && in->s >= 18 && in->d >= 0x0100 && in->db == CHASE_BANK &&
      steps >= 0x8000u && steps <= 0xffdcu && usual >= 0x8000u && usual <= 0xffdcu &&
      rec >= W_ACTOR_SLOTS && rec < W_ACTOR_SLOTS + ACTOR_SLOT_COUNT * ACTOR_SLOT_STRIDE &&
      (rec - W_ACTOR_SLOTS) % ACTOR_SLOT_STRIDE == 0;
  if (!ok) { ++s_r29_chase_calls; ++s_r29_chase_fallback; }
  return ok;
}

// Complete the chase once. A successful leap probe hands its exact machine
// state to the ROM before it changes state or calls the yielding animation.
static void r29_chase_stack(Wram* w, const Rom* rom, const CosimRegs* in, const ChaseLog* log) {
  PortCpu stack;
  cpu_from(in, &stack);
  if (log->gave_up) {
    // RNG overwrites the preceding nearest call's JSL, leaving its PHD.
    r29_helper_stack(w, &stack, 0xbce4);
    return;
  }
  if (!log->leap_probes) {
    push16(w, &stack, 0xbf81);
    r29_helper_stack(w, &stack, log->asked_actors ? 0xbc1f : 0xbc11);
    wram_w16(w, (uint16_t)(stack.s - 6u), 0); // actor test PEA $0000
    return;
  }
  // Last tile probe, including tile_attrs_at_pixel's nested tilemap call.
  const uint16_t x = wram_r16(w, (uint16_t)(in->d + CHASE_DP_NEXT_X));
  const uint16_t y = wram_r16(w, (uint16_t)(in->d + CHASE_DP_NEXT_Y));
  push16(w, &stack, 0xbfa2);
  const uint16_t probe_sp = stack.s;
  push8(w, &stack, 0x81);
  push16(w, &stack, log->leap_probes == 2 ? 0xbc77 : 0xbc56);
  push8(w, &stack, in->db); push16(w, &stack, in->d);
  push16(w, &stack, x); push16(w, &stack, y);
  push16(w, &stack, 0x007f); ++stack.s; // PLB consumes $7F
  push8(w, &stack, 0x80); push16(w, &stack, 0xade1);
  push16(w, &stack, (uint16_t)((x >> 3) * 2));
  if (log->leap_found) {
    stack.s = probe_sp;
    r29_helper_stack(w, &stack, 0xbc94);
    const uint16_t index = wram_r16(w, (uint16_t)(in->d + MONSTER_DP_FACING)) * 2u;
    const uint16_t landing_x = (uint16_t)(x + rom_word(rom, 0x81bcbdu + index));
    wram_w16(w, (uint16_t)(stack.s - 6u), (uint16_t)(landing_x - 9u));
  }
}

static const uint32_t R29_CHASE_EXITS[] = {MONSTER_CHASE_RTS_PC, 0x81be13u, 0x81bcf1u};
static void shim_monster_chase(Wram* w, const Rom* rom, const CosimRegs* in,
                               CosimRegs* out) {
  ChaseLog log = {0};
  PortCpu c;
  cpu_from(in, &c);
  monster_chase(w, rom, in->d, &log);
  r29_chase_stack(w, rom, in, &log);
  const bool leap = !log.gave_up && log.outcome == CHASE_LEAPT;
  c.a = log.a; c.x = log.x; c.y = log.y;
  set_nz16(&c, leap ? c.d : c.a);
  set_c(&c, log.carry); set_v(&c, log.overflow);
  c.pc = leap ? 0x81bcf1u : log.gave_up ? 0x81be13u : MONSTER_CHASE_RTS_PC;
  ++s_r29_chase_calls;
  if (leap) ++s_r29_chase_fallback;
  else ++s_r29_chase_native;
  cpu_to(&c, out);
  // Normal cost included the final RTS; the core still executes that opcode.
  R36_COSIM_COST(chase_cycles(&log, in) - (leap ? 0 : r27_cost(&R27_RTS, in)));
}

static const uint32_t VICTIMS_YIELD_EXITS[] = {VICTIMS_YIELD_PC};
static const uint32_t VICTIMS_RESUME_EXITS[] = {
    VICTIMS_YIELD_PC, VICTIMS_START_CALL_PC, VICTIMS_STOP_CALL_PC};
static const uint32_t OBJECT_YIELD_EXITS[] = {OBJECT_YIELD_PC};
// `object_resume` runs on into `object_polled`, so its exits are those and one.
static const uint32_t OBJECT_POLLED_EXITS[] = {
    OBJECT_YIELD_PC, OBJECT_GIVE_CALL_PC, OBJECT_FREE_CALL_PC};
static const uint32_t OBJECT_RESUME_EXITS[] = {
    OBJECT_YIELD_PC, OBJECT_GIVE_CALL_PC, OBJECT_FREE_CALL_PC,
    OBJECT_POLL_CALL_PC};
static const uint32_t ACTORS_YIELD_EXITS[] = {ACTORS_YIELD_PC};
static const uint32_t ACTORS_CHECKED_EXITS[] = {
    ACTORS_YIELD_PC, ACTORS_MEASURE_CALL_PC, ACTORS_START_CALL_PC};
static const uint32_t TILE_ANIM_YIELD_EXITS[] = {TILE_ANIM_YIELD_PC};
static const uint32_t TILE_ANIM_RESUME_EXITS[] = {
    TILE_ANIM_YIELD_PC, TILE_ANIM_QUEUE_CALL_PC, TILE_ANIM_END_PC};

// `$80:CE72` indexes `$1CB8` by the page's `$0E`, which is 0 or 2; anything
// that would reach past low WRAM is not a player's page.
static bool accepts_player_dead(const Wram* w, const CosimRegs* in) {
  return accepts_body_low(w, in) &&
         wram_r16(w, (uint16_t)(in->d + 0x0e)) < 0x2000 - W_PLAYER_HEALTH - 1;
}

static const uint32_t PLAYER_TICKS_EXITS[] = {PLAYER_YIELD_PC};
static const uint32_t PLAYER_STATE_EXITS[] = {PLAYER_STATE_CALL_PC};
static const uint32_t PLAYER_MOVE_EXITS[] = {
    PLAYER_MOVE_CALL_PC, PLAYER_PUBLISH_PC};
static const uint32_t PLAYER_BUTTONS_EXITS[] = {PLAYER_WON_CALL_PC};
static const uint32_t PLAYER_LOOP_EXITS[] = {PLAYER_YIELD_PC};
static const uint32_t PLAYER_BRANCH_EXITS[] = {PLAYER_BRANCH_JMP_PC};
static const uint32_t PLAYER_HURT_EXITS[] = {
    PLAYER_HURT_EVENT_PC, PLAYER_HURT_RTS_PC, PLAYER_HURT_RESET_RTS_PC};
static const uint32_t PLAYER_WON_EXITS[] = {
    PLAYER_WON_RTS_PC, PLAYER_WON_END_PC};
static const uint32_t PLAYER_DEAD_EXITS[] = {
    PLAYER_DEAD_RTS_PC, PLAYER_DEAD_END_PC};

// ---------------------------------------------------------------------------
// Vblank jobs — see `port/vblank.h`
// ---------------------------------------------------------------------------
//
// `tools/cycles816.py`'s prices, each run less the writes it ends on: an
// absolute store is three fetches, and then 6 for each byte it writes to a
// register, which `cosim_hw` adds. The tables are in low WRAM, 8 a byte
// whatever the data bank. Every job runs on page zero.
static const CosimRun VBL_COST[VBL_BLOCK_COUNT] = {
    [VBL_STORE] = {18, 3, 0},
    [VBL_TAKEN] = {6, 0, 0},
    [VBL_LOAD] = {58, 6, 0},
    [VBL_GO] = {48, 7, 0},
    [VBL_NEXT] = {82, 8, 1},
    [VQ_HEAD] = {40, 4, 1},
    [VQ_BUSY] = {60, 2, 0},
    [VQ_EMPTY] = {112, 7, 1},
    [VQ_START] = {76, 10, 1},
    [VQ_FIRST] = {18, 3, 0},
    [VQ_VMAIN] = {68, 8, 0},
    [VQ_SIZE] = {76, 8, 0},
    [VQ_DONE] = {214, 17, 4},
    [SU_HEAD] = {76, 10, 1},
    [SU_EMPTY] = {46, 4, 1},
    [SU_VMAIN] = {48, 7, 0},
    [SU_INIT] = {54, 8, 0},
    [SU_VADDR2] = {94, 11, 0},
    [SU_CLEAR] = {28, 2, 1},
    [SU_OAM_IMM] = {36, 6, 0},
    [SU_OAM_LAST] = {54, 8, 0},
    [SU_TAIL] = {54, 2, 0},
    [B2_H] = {100, 10, 0},
    [B2_V] = {118, 12, 0},
    [B2_BASE] = {142, 16, 0},
    [B2_TAIL] = {60, 3, 0},
    [CS_DX] = {110, 12, 0},
    [CS_NEG] = {30, 5, 0},
    [CS_WRITE] = {66, 7, 0},
    [CS_DY] = {128, 14, 0},
    [CS_PARK] = {48, 7, 0},
    [CS_TAIL] = {72, 4, 0},
    [SS_FIRST] = {62, 8, 0},
    [SS_NEXT] = {44, 6, 0},
    [SS_TAIL] = {72, 4, 0},
    [BB_HEAD] = {48, 7, 0},
    [BB_MODE] = {54, 8, 0},
    [BB_COUNT] = {46, 5, 0},
    [BB_EMPTY] = {54, 2, 0},
    [BB_FIRST] = {24, 2, 0},
    [BB_NEXT] = {54, 6, 0},
    [BB_DONE] = {88, 5, 0},
};

// The dispatcher and the NMI both leave 16-bit registers, page zero and a data
// bank that sees low WRAM.
static bool accepts_vbl_job(const Wram* w, const CosimRegs* in) {
  (void)w;
  return wide(in) && in->d == 0 && bank_sees_low_wram(in->db);
}

// Both queue walks run until the index equals the count, so an odd count would
// never end, and one past the tables would read the next table as this one.
static bool accepts_vram_queue_flush(const Wram* w, const CosimRegs* in) {
  const uint16_t n = wram_r16(w, W_VRAM_QUEUE_COUNT);
  return accepts_vbl_job(w, in) && (n & 1) == 0 && n <= 0x30;
}

static bool accepts_sprite_upload_flush(const Wram* w, const CosimRegs* in) {
  const uint16_t n = wram_r16(w, W_SPRITE_UPLOAD_COUNT);
  return accepts_vbl_job(w, in) && (n & 1) == 0 && n <= 0x80;
}

// This one walks down to zero with `BPL`, so an odd count would start it on
// the wrong word of every table rather than never ending, which is as wrong.
static bool accepts_boss_bg_dma(const Wram* w, const CosimRegs* in) {
  const uint16_t n = wram_r16(w, W_BG_DMA_CURSOR);
  return accepts_vbl_job(w, in) && (n & 1) == 0 && n <= 0x40;
}

// One trace at a time, and 24 KB, so not on the stack.
static HwStep g_vbl_steps[HW_TRACE_MAX];
static HwTrace g_vbl_trace = {0, HW_TRACE_MAX, false, g_vbl_steps};

#define VBL_SHIM(name)                                                      \
  static void shim_##name(Wram* w, const Rom* rom, const CosimRegs* in,    \
                          CosimRegs* out) {                                \
    (void)rom;                                                             \
    PortCpu c;                                                             \
    cpu_from(in, &c);                                                      \
    g_vbl_trace.n = 0;                                                     \
    g_vbl_trace.full = false;                                              \
    name(w, &c, &g_vbl_trace);                                             \
    cpu_to(&c, out);                                                       \
    cosim_hw(&g_vbl_trace, VBL_COST, fetch_fast(in));                      \
  }

VBL_SHIM(vram_queue_flush)
VBL_SHIM(sprite_upload_flush)
VBL_SHIM(bg2_scroll_job)
VBL_SHIM(camera_scroll_job)
VBL_SHIM(scroll_shadow_job)
VBL_SHIM(boss_bg_dma)

// ---------------------------------------------------------------------------
// The sound routines — `apu_send`, `apu_load_set` and `apu_boot`
// ---------------------------------------------------------------------------
//
// See `port/apu.h`, "The uploads, traced". Each run's price, from
// `tools/cycles816.py` over the listing, branches as the run's name says. The
// data an `LDA [$18]` reads is priced by where it is: the IPL image is in WRAM
// at `$7F:0000`, and every data set is in fast ROM.
//
// And each run an instruction at a time (`CosimInsn`), because these calls
// have the NMI land in them and the SPC700 is listening. Taken at the end of
// a 12-cycle slice rather than of an instruction, it moved the next command:
// on `level1.zmv` call 16,265 of the set upload starts 30 cycles before
// vblank, and from there on every call ended one spin early or late. Stepped
// this way all 23,834 start and end on the ROM's cycle. An instruction's last
// bus cycle is what the core polls in front of:
// an idle (`I`), low WRAM or the stack (`W`), a FastROM program or data byte
// (`F`), or nothing, for a store whose write is its own event (`C`).
#define I(c, b) {c, b, 6, false}
#define W(c, b) {c, b, 8, false}
#define F(c, b) {c, b, 6, true}
#define C(c, b) {c, b, 0, false}
static const CosimInsn AS_HEAD_I[] = {I(18, 2), W(20, 2)};        // SEP : LDY dp
static const CosimInsn AS_STORE_I[] = {C(18, 3)};                 // STx abs
static const CosimInsn AS_INY_I[] = {I(12, 1), C(18, 3)};         // INY : STY abs
static const CosimInsn AS_TAIL_I[] = {W(20, 2)};                  // STY dp
static const CosimInsn APU_JSR_I[] = {W(40, 3)};
static const CosimInsn APU_RTS_I[] = {I(40, 1)};
static const CosimInsn NB_READ_FAST_I[] = {F(42, 3)};             // LDA [dp]
static const CosimInsn NB_READ_SLOW_I[] = {W(44, 2)};
static const CosimInsn NB_STEP_I[] = {W(34, 2), I(18, 2)};        // INC : BNE
static const CosimInsn NB_CARRY_I[] = {W(34, 2), F(12, 2), W(34, 2)};
static const CosimInsn LS_HEAD_I[] = {
    I(18, 2), F(18, 3), I(12, 1), I(12, 1), I(12, 1),  // REP AND ASL ASL TAX
    F(36, 6), W(28, 2), F(36, 6), W(28, 2), I(18, 2),  // LDA STA LDA STA SEP
};
static const CosimInsn LS_LO_I[] = {W(20, 2)};
static const CosimInsn LS_HI_I[] = {W(20, 2), W(20, 2)};
static const CosimInsn LS_END_I[] = {F(12, 2)};
static const CosimInsn LS_BLOCK_I[] = {I(18, 2), F(12, 2)};
static const CosimInsn LS_BYTE_I[] = {F(12, 2)};
static const CosimInsn LS_COUNT_I[] = {W(20, 2), I(18, 2)};
static const CosimInsn LS_BORROW_I[] = {W(20, 2), F(12, 2), W(34, 2)};
static const CosimInsn LS_DEC_I[] = {W(34, 2), W(20, 2), W(20, 2)};
static const CosimInsn LS_MORE_I[] = {I(18, 2)};
static const CosimInsn LS_NEXT_I[] = {F(12, 2), F(18, 3)};
static const CosimInsn AB_HEAD_I[] = {I(18, 2), W(20, 1), F(18, 3), F(18, 3),
                                      F(18, 3)};
static const CosimInsn AB_MVN_I[] = {I(44, 4)};
static const CosimInsn AB_MID_I[] = {F(18, 3), F(18, 3)};
static const CosimInsn AB_POINT_I[] = {
    W(26, 1), I(18, 2), I(18, 2), F(12, 2), W(20, 2),  // PLB SEP REP LDA STA
    F(30, 5), W(20, 2), F(30, 5), W(20, 2),            // LDA STA LDA STA
    F(30, 5), W(20, 2), W(40, 3),                      // LDA STA JSR
};
static const CosimInsn AB_ZERO_I[] = {F(12, 2), C(18, 3)};
static const CosimInsn AB_REP_I[] = {I(18, 2)};
static const CosimInsn IP_HEAD_I[] = {W(20, 1), I(18, 2), I(18, 2), F(18, 3),
                                      F(18, 3)};
static const CosimInsn IP_START_I[] = {I(18, 2), F(12, 2), I(18, 2)};
static const CosimInsn IP_HDR_I[] = {
    W(20, 1), I(18, 2), W(52, 2), I(12, 1), I(12, 1),  // PHA REP LDA INY INY
    I(12, 1), W(52, 2), I(12, 1), I(12, 1), C(18, 3),  // TAX LDA INY INY STA
};
static const CosimInsn IP_FLAG_I[] = {I(18, 2), F(18, 3), F(12, 2), I(12, 1),
                                      C(18, 3)};
static const CosimInsn IP_KICK_I[] = {F(12, 2), W(26, 1), C(18, 3)};
static const CosimInsn IP_FIRST_I[] = {
    I(18, 2), W(44, 2), I(12, 1), I(18, 1),  // BVS LDA INY XBA
    F(12, 2), I(18, 2), I(18, 2), C(18, 3),  // LDA BRA REP STA
};
static const CosimInsn IP_NEXT_I[] = {I(18, 2), I(12, 1), I(18, 2), I(18, 1),
                                      W(44, 2), I(12, 1), I(18, 1)};
static const CosimInsn IP_SEND_I[] = {I(12, 1), I(18, 2), C(18, 3)};
static const CosimInsn IP_ENDB_I[] = {I(18, 2), I(12, 1), F(12, 2)};
static const CosimInsn IP_ADC_I[] = {F(12, 2)};
static const CosimInsn IP_BEQ_T_I[] = {I(18, 2)};
static const CosimInsn IP_BEQ_N_I[] = {F(12, 2)};
static const CosimInsn IP_DONE_I[] = {F(12, 2), W(26, 1), I(40, 1)};
#undef I
#undef W
#undef F
#undef C

#define APU_RUN(cycles, bytes, dp, ins) \
  {cycles, bytes, dp, ins, (int)(sizeof ins / sizeof ins[0])}
static const CosimRun APU_COST[APU_BLOCK_COUNT] = {
    [AS_HEAD] = APU_RUN(38, 4, 1, AS_HEAD_I),
    [AS_STORE] = APU_RUN(18, 3, 0, AS_STORE_I),
    [AS_INY] = APU_RUN(30, 4, 0, AS_INY_I),
    [AS_TAIL] = APU_RUN(20, 2, 1, AS_TAIL_I),
    [APU_JSR] = APU_RUN(40, 3, 0, APU_JSR_I),
    [APU_RTS] = APU_RUN(40, 1, 0, APU_RTS_I),
    [NB_READ_FAST] = APU_RUN(42, 3, 1, NB_READ_FAST_I),
    [NB_READ_SLOW] = APU_RUN(44, 2, 1, NB_READ_SLOW_I),
    [NB_STEP] = APU_RUN(52, 4, 1, NB_STEP_I),
    [NB_CARRY] = APU_RUN(80, 6, 2, NB_CARRY_I),
    [LS_HEAD] = APU_RUN(218, 26, 2, LS_HEAD_I),
    [LS_LO] = APU_RUN(20, 2, 1, LS_LO_I),
    [LS_HI] = APU_RUN(40, 4, 2, LS_HI_I),
    [LS_END] = APU_RUN(12, 2, 0, LS_END_I),
    [LS_BLOCK] = APU_RUN(30, 4, 0, LS_BLOCK_I),
    [LS_BYTE] = APU_RUN(12, 2, 0, LS_BYTE_I),
    [LS_COUNT] = APU_RUN(38, 4, 1, LS_COUNT_I),
    [LS_BORROW] = APU_RUN(66, 6, 2, LS_BORROW_I),
    [LS_DEC] = APU_RUN(74, 6, 3, LS_DEC_I),
    [LS_MORE] = APU_RUN(18, 2, 0, LS_MORE_I),
    [LS_NEXT] = APU_RUN(30, 5, 0, LS_NEXT_I),
    [AB_HEAD] = APU_RUN(92, 12, 0, AB_HEAD_I),
    // Three program bytes fetched again for every byte moved, the source byte
    // from fast ROM in `$91` or `$95`, the destination in WRAM, two idles.
    [AB_MVN] = APU_RUN(44, 4, 0, AB_MVN_I),
    [AB_MID] = APU_RUN(36, 6, 0, AB_MID_I),
    [AB_POINT] = APU_RUN(284, 33, 4, AB_POINT_I),
    [AB_ZERO] = APU_RUN(30, 5, 0, AB_ZERO_I),
    [AB_REP] = APU_RUN(18, 2, 0, AB_REP_I),
    [IP_HEAD] = APU_RUN(92, 11, 0, IP_HEAD_I),
    [IP_START] = APU_RUN(48, 6, 0, IP_START_I),
    [IP_HDR] = APU_RUN(220, 15, 2, IP_HDR_I),
    [IP_FLAG] = APU_RUN(78, 11, 0, IP_FLAG_I),
    [IP_KICK] = APU_RUN(56, 6, 0, IP_KICK_I),
    [IP_FIRST] = APU_RUN(158, 15, 1, IP_FIRST_I),
    [IP_NEXT] = APU_RUN(140, 10, 1, IP_NEXT_I),
    [IP_SEND] = APU_RUN(48, 6, 0, IP_SEND_I),
    [IP_ENDB] = APU_RUN(42, 5, 0, IP_ENDB_I),
    [IP_ADC] = APU_RUN(12, 2, 0, IP_ADC_I),
    [IP_BEQ_T] = APU_RUN(18, 2, 0, IP_BEQ_T_I),
    [IP_BEQ_N] = APU_RUN(12, 2, 0, IP_BEQ_N_I),
    [IP_DONE] = APU_RUN(78, 4, 0, IP_DONE_I),
};
#undef APU_RUN

// All three read `$1E` and the cursor on direct page zero, reach the APU's
// ports through the data bank, and the two uploads push onto the stack. The
// IPL upload's `ADC`s are binary.
static bool accepts_apu(const Wram* w, const CosimRegs* in) {
  (void)w;
  return in->d == 0 && (in->db & 0x40) == 0 && !(in->p & PORT_P_D) &&
         low_stack(in);
}

// ...and a set has to fit the trace, at 23 steps a byte. The largest in the
// table is 9,280 bytes. The cursor carries into its high byte and never
// into the bank, so the walk wraps the way `apu_next_byte` does.
#define APU_SET_MAX_BYTES 10000
static bool supported_apu_load_set(Wram* scratch, const Rom* rom,
                                   const CosimRegs* in) {
  if (!accepts_apu(scratch, in)) return false;
  const uint32_t index = (in->a & 0xffu) * 4u;
  const uint32_t bank = (uint32_t)(rom_word(rom, APU_SET_TABLE + 2u + index) &
                                   0xffu) << 16;
  uint16_t at = rom_word(rom, APU_SET_TABLE + index);
  uint32_t bytes = 0;
  for (int blocks = 0;; blocks++) {
    const uint16_t n = (uint16_t)(bus_r8(scratch, rom, bank | at) |
                                  bus_r8(scratch, rom, bank | (uint16_t)(at + 1))
                                      << 8);
    if (n == 0) return true;
    bytes += n;
    if (bytes > APU_SET_MAX_BYTES || blocks > 256) return false;
    at = (uint16_t)(at + 2u + n);
  }
}

// One trace at a time, and 1.5 MB, so not on the stack.
static HwStep g_apu_steps[APU_TRACE_MAX];
static HwTrace g_apu_trace = {0, APU_TRACE_MAX, false, g_apu_steps};

#if defined(XBOX_PORT) && defined(ZAMN_R46_APU_COMPACT_TRACE)
// R46 removes the CPU-only trace construction cost at the source instead of
// building tens of thousands of HW_RUN events and deleting them later. These
// counters are intentionally outside CosimWork: the shim owns trace generation
// and has all the exact block-repeat information before cosim_hw consumes it.
static uint64_t g_r46_apu_calls;
static uint64_t g_r46_apu_run_steps;
static uint64_t g_r46_apu_run_reps;
static uint64_t g_r46_apu_cycles;
static uint64_t g_r46_apu_stack_coalesced;
static uint64_t g_r46_apu_trace_steps;

void cosim_r46ApuTraceTake(uint64_t* calls, uint64_t* run_steps,
                           uint64_t* run_reps, uint64_t* cycles,
                           uint64_t* stack_coalesced,
                           uint64_t* trace_steps) {
  if (calls) *calls = g_r46_apu_calls;
  if (run_steps) *run_steps = g_r46_apu_run_steps;
  if (run_reps) *run_reps = g_r46_apu_run_reps;
  if (cycles) *cycles = g_r46_apu_cycles;
  if (stack_coalesced) *stack_coalesced = g_r46_apu_stack_coalesced;
  if (trace_steps) *trace_steps = g_r46_apu_trace_steps;
  g_r46_apu_calls = 0;
  g_r46_apu_run_steps = 0;
  g_r46_apu_run_reps = 0;
  g_r46_apu_cycles = 0;
  g_r46_apu_stack_coalesced = 0;
  g_r46_apu_trace_steps = 0;
}
#endif

static void apu_trace_reset(bool compact) {
  g_apu_trace.n = 0;
  g_apu_trace.full = false;
  g_apu_trace.suppress_runs = compact;
  g_apu_trace.coalesce_stack = compact;
  g_apu_trace.suppressed_run_steps = 0;
  g_apu_trace.suppressed_run_reps = 0;
  memset(g_apu_trace.suppressed_run_blocks, 0,
         sizeof(g_apu_trace.suppressed_run_blocks));
  g_apu_trace.coalesced_stack_steps = 0;
}

#define APU_SHIM(name, call)                                                \
  static void shim_##name(Wram* w, const Rom* rom, const CosimRegs* in,    \
                          CosimRegs* out) {                                \
    (void)rom;                                                             \
    PortCpu c;                                                             \
    cpu_from(in, &c);                                                      \
    apu_trace_reset(false);                                                \
    call;                                                                  \
    cpu_to(&c, out);                                                       \
    cosim_hw(&g_apu_trace, APU_COST, fetch_fast(in));                      \
  }

APU_SHIM(apu_send, apu_send_traced(w, &c, &g_apu_trace))

static void shim_apu_load_set(Wram* w, const Rom* rom, const CosimRegs* in,
                              CosimRegs* out) {
  PortCpu c;
  cpu_from(in, &c);
#if defined(XBOX_PORT) && defined(ZAMN_R46_APU_COMPACT_TRACE)
  const bool compact = true;
#else
  const bool compact = false;
#endif
  apu_trace_reset(compact);
  apu_load_set_traced(w, rom, &c, &g_apu_trace);
  cpu_to(&c, out);

#if defined(XBOX_PORT) && defined(ZAMN_R46_APU_COMPACT_TRACE)
  if (compact) {
    const bool fast = fetch_fast(in);
    uint64_t removed = 0;
    for (int i = 0; i < APU_BLOCK_COUNT && i < HW_SUPPRESS_BLOCK_MAX; ++i)
      removed += g_apu_trace.suppressed_run_blocks[i] *
                 (uint64_t)cosim_run_cycles(&APU_COST[i], fast);
    g_r46_apu_calls++;
    g_r46_apu_run_steps += g_apu_trace.suppressed_run_steps;
    g_r46_apu_run_reps += g_apu_trace.suppressed_run_reps;
    g_r46_apu_cycles += removed;
    g_r46_apu_stack_coalesced += g_apu_trace.coalesced_stack_steps;
    g_r46_apu_trace_steps += (uint64_t)g_apu_trace.n;
  }
#endif

  // In R46 the trace already contains only real hardware-visible timing, so
  // cosim_hw prices precisely the compact transfer. Older builds still hand
  // it the full CPU+hardware trace and R45 may compact it later.
  cosim_hw(&g_apu_trace, APU_COST, fetch_fast(in));
}

APU_SHIM(apu_boot, apu_boot_traced(w, rom, &c, &g_apu_trace))

static const uint32_t APU_SEND_EXITS[] = {APU_SEND_EXIT};
static const uint32_t APU_LOAD_SET_EXITS[] = {APU_LOAD_SET_EXIT};
static const uint32_t APU_BOOT_EXITS[] = {APU_BOOT_EXIT};

// The count is the table's length by construction, so it cannot drift from it.
#define COSIM_COMMIT(tbl) \
  .commit = (tbl), .commit_count = (int)(sizeof(tbl) / sizeof((tbl)[0]))

static const CosimRoutine ROUTINES[] = {
    {
        .name = "sprite_frame_tile",
        .symbol = "$80:B9D6",
        .entry = 0x80b9d6,
        .ret_op = 0x80b9eb,  // the hit path's RTS; either one returns the same
        .ret_kind = COSIM_RTS,
        .run = shim_sprite_frame_tile,
        .excludes = SPRITE_TILE_EXCLUDES,
        .exclude_count = 1,
        .cycles = 302,
        .stack_bytes = 2,   // the `PHA` at $80:BA2B on the miss path
    },
    {
        .name = "sprite_cache_age",
        .symbol = "$80:B9C7",
        .entry = 0x80b9c7,
        .ret_op = 0x80b9d5,
        .ret_kind = COSIM_RTS,
        .run = shim_sprite_cache_age,
        .cycles = 10914,
        .stack_bytes = 0,   // pushes nothing
    },
    {
        .name = "thread_tick_waits",
        .symbol = "$80:8398",
        .entry = 0x808398,
        .ret_op = 0x8083ad,
        .ret_kind = COSIM_RTS,
        .run = shim_thread_tick_waits,
        .cycles = 3204,
        .stack_bytes = 0,   // pushes nothing
    },
    {
        .name = "hud_refresh",
        COSIM_COMMIT(COMMIT_VBL_A),
        .symbol = "$80:C07F",
        .entry = 0x80c07f,
        // `$C0A2`, the quiet path's `RTL`. The other exit is a `JML $8083AE`
        // and returns through *that* routine's `RTL` — which is fine, because a
        // return is detected by PC and stack pointer rather than by address,
        // and `ret_op` is only where a substituted call is teleported to.
        .ret_op = 0x80c0a2,
        .ret_kind = COSIM_RTL,
        .run = shim_hud_refresh,
        .cycles = 1367,
        .stack_bytes = 6,  // `JSR` a panel, `JSR` an adapter, `JSR` a digit
    },
    {
        .name = "hud_panel1",
        .symbol = "$80:C0A3",
        .entry = 0x80c0a3,
        .ret_op = 0x80c138,
        .ret_kind = COSIM_RTS,
        .run = shim_hud_panel1,
        .cycles = 1247,
        .stack_bytes = 4,
    },
    {
        .name = "hud_panel2",
        .symbol = "$80:C139",
        .entry = 0x80c139,
        .ret_op = 0x80c1ce,
        .ret_kind = COSIM_RTS,
        .run = shim_hud_panel2,
        // Lower than its twin's only because player 2 is usually absent, and
        // the panel-off exit is eight cycles of work. On a two-player movie the
        // two means are within one percent of each other.
        .cycles = 694,
        .stack_bytes = 4,
    },
    {
        .name = "vbl_queue_a_add",
        COSIM_COMMIT(COMMIT_VBL_A),
        .symbol = "$80:83AE",
        .entry = 0x8083ae,
        .ret_op = 0x8083d2,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_vbl_queue_a_add,
        .cycles = 743,
        .stack_bytes = 2,   // the opening `PHY`
    },
    {
        .name = "vbl_queue_b_add",
        COSIM_COMMIT(COMMIT_VBL_B),
        .symbol = "$80:8418",
        .entry = 0x808418,
        .ret_op = 0x80843a,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_vbl_queue_b_add,
        .cycles = 393,
        .stack_bytes = 2,   // the opening `PHY`
    },
    {
        .name = "actor_depth_sort",
        .symbol = "$80:BC7F",
        .entry = 0x80bc7f,
        .ret_op = 0x80bce1,
        .ret_kind = COSIM_RTS,
        .run = shim_actor_depth_sort,
        .excludes = DEPTH_SORT_EXCLUDES,
        .exclude_count = 1,
        .cycles = 1605,
        .stack_bytes = 0,  // pushes nothing
    },
    {
        .name = "actor_cull",
        .symbol = "$80:BCE2",
        .entry = 0x80bce2,
        .ret_op = 0x80bd1e,
        .ret_kind = COSIM_RTS,
        .run = shim_actor_cull,
        .cycles = 2675,
        .stack_bytes = 0,  // pushes nothing
    },
    {
        .name = "oam_buffer_clear",
        .symbol = "$80:BC23",
        .entry = 0x80bc23,
        .ret_op = 0x80bc7e,
        .ret_kind = COSIM_RTS,
        .run = shim_oam_buffer_clear,
        .cycles = 4814,
        .stack_bytes = 2,  // the opening `PHD`
    },
    {
        .name = "thread_spawn",
        .symbol = "$80:825E",
        .entry = 0x80825e,
        .ret_op = 0x8082d7,  // RTL; the full-board path has its own at $82DD
        .ret_kind = COSIM_RTL,
        .run = shim_thread_spawn,
        // 330 calls on `movies/level1-2p.zmv`, spanning 1,558..2,944 — the
        // spread is the slot search, which runs downwards and so costs more the
        // emptier the board is.
        .cycles = 2327,
        .stack_bytes = 4,  // the opening PHD and the PHA under it
    },
    {
        .name = "thread_call_handler",
        .symbol = "$80:8480",
        .entry = 0x808480,
        .ret_op = 0x8084b0,  // RTL; both exits converge on it
        .ret_kind = COSIM_RTL,
        .run = shim_thread_call_handler,
        .supported = guard_thread_call_handler,
        .fast_guard = fast_guard_thread_call_handler,
        .cycles = 958,
        // 11 until a handler could die: `enemy_die`'s `JSR` and the `JSL` to
        // `score_add` under it are six bytes deeper than anything else reaches.
        .stack_bytes = 19,
    },
    {
        .name = "player_collide",
        .symbol = "$80:F7F7",
        .entry = 0x80f7f7,
        .ret_op = 0x80f807,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_player_collide,
        .supported = guard_player_collide,
        .cycles = 515,
        // 2 until the pickup entry was ported; its `JSL apu_play_sfx` and the
        // weapon selector under it are nine bytes deeper than the hit path.
        .stack_bytes = 11,
    },
    {
        .name = "enemy_collide",
        .symbol = "$81:8888",
        .entry = 0x818888,
        // A bare `RTL`, deliberately: the death branch returns carry *set*, and
        // `native_publish` has already put it in the flags by the time the core
        // gets here. Pointing this at the `CLC` two bytes earlier would undo the
        // one output that parks the thread.
        .ret_op = 0x81888e,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_collide,
        .supported = guard_enemy_collide,
        // 84 for the ignore branch, which is 1,225 of the 1,226; 1,150 for the
        // one death, which is why the mean barely moves off the floor.
        .cycles = 88,
        .stack_bytes = 9,   // the ignore branch pushes nothing; a death, 9
    },
    {
        .name = "monster_collide",
        .symbol = "$81:C4A6",
        .entry = 0x81c4a6,
        // `$81:C50B`, a bare `RTL`, for `enemy_collide`'s reason: three of the
        // four exits set carry themselves and two of those set it, so landing on
        // a `CLC` or a `SEC` would overwrite the answer the port already
        // published. The `CLC` that shares this exit is the byte before.
        .ret_op = 0x81c50b,
        .ret_kind = COSIM_RTL,
        .run = shim_monster_collide,
        .supported = guard_monster_collide,
        // Measured 120..540, mean 135, over 186 calls on
        // `movies/level45-race.zmv`. The floor is the ignore branch, which is
        // 157 of them; the ceiling is taking an object.
        .cycles = 120,
        // 2 observed, and every call so far is an ignore, a latch or a take —
        // none of which nests. Left at the death path's depth, which is the same
        // `JSR` into `$81:BBEB` plus its `JSL score_add` that `enemy_collide`
        // budgets 9 for, because that path is transcribed and will want it.
        .stack_bytes = 9,
    },
    {
        .name = "monster_c440",
        .symbol = "$81:C440",
        .entry = 0x81c440,
        // `$81:C4A5`, this copy's own bare `RTL` — the one two bytes into
        // `$81:C4A4  CLC : RTL`. Not `$81:C50B`: the copies are separate code
        // and pointing one at the other's exit would work by luck and stop
        // working the day either moves.
        .ret_op = 0x81c4a5,
        .ret_kind = COSIM_RTL,
        .run = shim_monster_c440_collide,
        .supported = guard_monster_c440_collide,
        // Measured 120..1322, mean 267, over the 151 calls
        // `movies/level25.zmv` makes — and unlike the spider's, this sample is
        // not all ignores: 115 of them are hits, 105 survivors and 10 deaths.
        // The mean is twice `monster_collide`'s for exactly that reason.
        .cycles = 267,
        .stack_bytes = 9,  // the same death path, budgeted the same way
    },
    {
        .name = "enemy_b41c",
        .symbol = "$81:B41C",
        .entry = 0x81b41c,
        // `$81:B422`, the first of this routine's four bare `RTL`s, chosen for
        // `enemy_collide`'s reason: every exit decides carry for itself — two set
        // it and two clear it — so landing anywhere that runs a `CLC` or a `SEC`
        // would overwrite the answer the port has already published.
        .ret_op = 0x81b422,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_b41c_collide,
        .supported = guard_enemy_b41c_collide,
        // Measured 84..1312, mean 235, over the 66 calls
        // `movies/level29-fighting.zmv` makes. The floor is the ignore branch and
        // the ceiling is a survivor, whose `JML $81:8506` walks a parked stack.
        .cycles = 235,
        // 2, measured over those 66 — which included a death. This copy's death
        // path is the cheap one: no `JSR $81:8727`, because it awards nothing.
        // The `$61` branch's `JSR $B168` pushes the same 2 when something finally
        // takes it.
        .stack_bytes = 2,
    },
    {
        .name = "enemy_d7f6",
        .symbol = "$81:D7F6",
        .entry = 0x81d7f6,
        // `$81:D7FC`, the ignore path's `RTL`. The death tail sets carry and the
        // two ignore exits clear it, so the same rule as everywhere in this
        // family applies: land on a bare `RTL` and let `native_publish`'s flags
        // stand.
        .ret_op = 0x81d7fc,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_d7f6_collide,
        .supported = guard_enemy_d7f6_collide,
        // Measured 84..124, mean 86, over the 128 calls `movies/level17.zmv`
        // makes — and every one of them is the ignore branch, so this is the
        // cost of two instructions and nothing else. The damage path will be
        // dearer; it is budgeted low deliberately, because a budget that is too
        // small shows up as a `run` divergence rather than hiding.
        .cycles = 86,
        // The death tail calls nothing; a survivor leaves through `$81:8506`,
        // which pushes nothing either. Budgeted 2 rather than 0 because the one
        // id that declines is the only path with a `JML` this port does not
        // follow, and a budget that is too small fails a call.
        .stack_bytes = 2,
    },
    {
        // `$81:D7F6`'s sixty-four bytes again, one bank over — see
        // `ENEMY_9A6D_COLLIDE_ENTRY`. Same port, same guard and same shim; its
        // own entry so `verify` offers it the calls made to this address.
        .name = "enemy_9a6d",
        .symbol = "$82:9A6D",
        .entry = 0x829a6d,
        .ret_op = 0x829a73,  // the ignore path's bare `RTL`, as above
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_d7f6_collide,
        .supported = guard_enemy_d7f6_collide,
        // Measured 84..1196, mean 95, over the 829 calls a sweep of record 0
        // makes (`zamn_cosim verify --level 0`).
        .cycles = 95,
        .stack_bytes = 2,
    },
    {
        .name = "enemy_9b6b",
        .symbol = "$81:9B6B",
        .entry = 0x819b6b,
        // `$81:9B71`, the ignore path's `RTL`, for the family's usual reason.
        .ret_op = 0x819b71,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_9b6b_collide,
        .supported = guard_enemy_9b6b_collide,
        // Measured 84..124, mean 86, over the 33 calls `movies/level21.zmv`
        // makes — all of them the ignore branch, so this is two instructions.
        .cycles = 86,
        // 0 observed, for the same reason. Left at 2, which is what the damage
        // path's `JML` into `$81:8506` will want.
        .stack_bytes = 2,
    },
    {
        .name = "enemy_9063",
        .symbol = "$81:9063",
        .entry = 0x819063,
        .ret_op = 0x819069,  // the ignore path's RTL
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_9063_collide,
        .supported = guard_enemy_9063_collide,
        // Measured 84..490, mean 98, over the 32 calls `movies/level5.zmv`
        // makes. The 490 is the one call that is not an ignore.
        .cycles = 98,
        .stack_bytes = 2,
    },
    {
        .name = "enemy_ac92",
        .symbol = "$81:AC92",
        .entry = 0x81ac92,
        .ret_op = 0x81ac98,  // the ignore path's RTL, after its own CLC
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_ac92_collide,
        .supported = guard_enemy_ac92_collide,
        // Measured 98..1126, mean 109, over the 309 calls
        // `movies/level49-corner.zmv` makes. Almost all of them are ignores,
        // which is two instructions; the 1,126 is a hit that spliced.
        .cycles = 109,
        .stack_bytes = 2,
    },
    {
        .name = "enemy_e6e4",
        .symbol = "$81:E6E4",
        .entry = 0x81e6e4,
        // `$81:E6F1`, the ignore path's `RTL` after its own `CLC`. The airborne
        // guard and the no-damage path share a *different* `CLC : RTL` at
        // `$81:E72A`, and the death tail sets carry — so, as everywhere in this
        // family, land on a bare `RTL` and let `native_publish`'s flags stand.
        .ret_op = 0x81e6f1,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_e6e4_collide,
        // No guard: nothing it can be handed makes it decline. See above.
        .supported = NULL,
        // Measured 164..570, mean 192, over the 367 calls
        // `movies/level37-e6e4.zmv` makes. The floor is higher than the rest of
        // the family's — 164 against `enemy_d7f6`'s 84 — because even the ignore
        // path reads the display record first, which is `LDY $08 : LDX $0004,Y`
        // before any comparison happens. A guard costs every caller, including
        // the ones it does not refuse.
        .cycles = 192,
        .stack_bytes = 2,
    },
    {
        .name = "enemy_990b",
        .symbol = "$81:990B",
        .entry = 0x81990b,
        // `$81:9911`, the ignore path's `RTL` after its own `CLC` — the same
        // choice `enemy_e6e4` makes and for the same reason. This routine has
        // three other `RTL`s and two of them arrive with carry set.
        .ret_op = 0x819911,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_990b_collide,
        // No guard: nothing it can be handed makes it decline. See above.
        .supported = NULL,
        // Measured 84..1312, mean 152, over the 938 calls
        // `movies/level29-990b.zmv` makes. **The 92 this said before was priced
        // by 35 calls that were all ignores**, and the note under it said so —
        // "the eight branches behind the first comparison are priced by nobody."
        // They are priced now, and it cost 60 cycles a call: the ceiling went
        // from 124 to 1312, which is what a splice into a parked stack costs
        // when somebody finally shoots the thing.
        .cycles = 152,
        .stack_bytes = 2,
    },
    {
        .name = "enemy_990b_spin",
        .symbol = "$81:96E4",
        .entry = 0x8196e4,
        // `$81:96EA`, the ignore path's `RTL` after its own `CLC` — and this
        // routine's only reachable one. The `RTL` two instructions below it is
        // behind a `JML` and nothing arrives there.
        .ret_op = 0x8196ea,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_990b_spin_collide,
        // No guard: `enemy_survived_react` has been ported since the level-53
        // round, so the one branch this routine has cannot give up.
        .supported = NULL,
        // Measured 84..880, mean 271, over the 430 calls
        // `movies/level29-990b.zmv` makes. The floor is the family's bare
        // `CMP : BCS : CLC : RTL` to the cycle; the ceiling is the splice, and
        // there is nothing in between — seven instructions and no third path.
        // The mean sits high because more than half of these calls are hits:
        // the spin is short and the player was holding the button down.
        .cycles = 271,
        .stack_bytes = 2,
    },
    {
        .name = "actor_845e",
        .symbol = "$81:845E",
        .entry = 0x81845e,
        .ret_op = 0x81847b,  // the pass path's RTL, after its CLC
        .ret_kind = COSIM_RTL,
        .run = shim_actor_845e_collide,
        // Nothing to guard: it cannot decline and it writes no WRAM.
        .supported = NULL,
        // Measured 104..322, mean 115, over the 38 calls
        // `movies/level49-corner.zmv` makes.
        .cycles = 115,
        // It pushes nothing and calls nothing — the only entry in the registry
        // that touches neither the stack nor WRAM.
        .stack_bytes = 0,
    },
    {
        .name = "shot_edaa",
        .symbol = "$81:EDAA",
        .entry = 0x81edaa,
        .ret_op = 0x81edaa,  // the entry *is* the RTL
        .ret_kind = COSIM_RTL,
        .run = shim_shot_edaa_collide,
        // Nothing to guard: one instruction, no ids, no stores, no decline.
        .supported = NULL,
        // Measured 42..82, mean 43, over the 62 calls
        // `movies/level25-boss.zmv` makes — which is worth a line, because an
        // `RTL` is six cycles and this is the entry where the difference between
        // *the routine* and *reaching the routine* is the whole number. What the
        // harness measures is entry PC to return, and for a one-byte routine that
        // is almost entirely the `JSL` and the bus.
        .cycles = 43,
        .stack_bytes = 0,
    },
    {
        .name = "shot_f6a3",
        .symbol = "$81:F6A3",
        .entry = 0x81f6a3,
        // `$81:F6B7`, the store path's `RTL`. The ignore path has its own two
        // instructions earlier at `$81:F6B3`; either would do, and this is the
        // one the majority of calls do not take, which is the same choice
        // `actor_845e` made and for the same reason: pick the return the harness
        // can be sure it is watching.
        .ret_op = 0x81f6b7,
        .ret_kind = COSIM_RTL,
        .run = shim_shot_f6a3_collide,
        // Nothing to guard: three ids and an else, and neither exit declines.
        .supported = NULL,
        // Measured 118..188, mean 148, over the 273 calls
        // `movies/level25-heavy.zmv` makes.
        .cycles = 148,
        .stack_bytes = 0,
    },
    {
        .name = "actor_f4ef",
        .symbol = "$82:F4EF",
        .entry = 0x82f4ef,
        // `$82:F4FE`, the store path's `RTL`; the ignore path has its own four
        // bytes earlier at `$82:F4FA`. Same choice as `shot_f6a3`, and the first
        // entry in this registry whose bank is `$82`.
        .ret_op = 0x82f4fe,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_f4ef_collide,
        // Two ids and an else; neither exit declines.
        .supported = NULL,
        // Measured 118 exactly, on all seven calls `movies/level21-bubble.zmv`
        // makes — the only entry in the registry with no spread at all, because
        // sixteen bytes of comparisons have nothing to be slow about.
        .cycles = 118,
        .stack_bytes = 0,
    },
    {
        .name = "enemy_d301",
        .symbol = "$81:D301",
        .entry = 0x81d301,
        // `$81:D361`, the `RTL` the three carry-clearing exits share. The other
        // five set carry themselves, so the same rule as the rest of the family:
        // land on a bare `RTL` and let `native_publish`'s flags stand rather than
        // on the `CLC` at `$81:D360`.
        .ret_op = 0x81d361,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_d301_collide,
        .supported = guard_enemy_d301_collide,
        // Measured 140..140 over the three calls `movies/level9.zmv` makes —
        // all of them the `$FF` id, which is six instructions. The damage path
        // is dearer and this is budgeted low deliberately: a budget that is too
        // small shows up as a `run` divergence rather than hiding.
        .cycles = 140,
        // The `$FF` path pushes nothing. Budgeted for the survivor's tail, which
        // is a `JSL` into `$81:8506` and a `JSR` into `$81:D142`.
        .stack_bytes = 2,
    },
    {
        .name = "enemy_freeze",
        .symbol = "$81:847E",
        .entry = 0x81847e,
        // `$81:84D3`, the `RTL` after the `SEC`. Two of the three exits clear
        // carry and one sets it, so the same rule as the rest of the family:
        // land on a bare `RTL` and let `native_publish`'s flags stand.
        .ret_op = 0x8184d3,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_freeze,
        // Measured 180..1240, mean 296, over the 73 calls
        // `movies/level17-weapon.zmv` puts through the entry PC itself. The
        // 1,240 is a freeze: the counter, the guard, the slot lookup and the
        // splice. The 180 is a hit that only counted.
        .cycles = 296,
        .stack_bytes = 2,  // `JSL $80:9D6A` on the acting path; nothing else
    },
    {
        .name = "enemy_bubble",
        .symbol = "$81:83C6",
        .entry = 0x8183c6,
        // `$81:8403`, the `RTL` after the splice's `SEC`. Same rule as
        // `enemy_freeze`: land on a bare `RTL` so `native_publish`'s flags stand
        // rather than on the `CLC` at `$81:83D0`.
        .ret_op = 0x818403,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_bubble_react,
        // Measured 906..946, mean 933, over the three calls
        // `movies/level49-corner.zmv` puts through it — all of them the splice,
        // so the guard's own refusal has never been timed.
        .cycles = 933,
        // `PHA` and `PLX` inside the splice, which is all it pushes: it calls
        // nothing.
        .stack_bytes = 2,
    },
    {
        .name = "rng",
        .symbol = "$80:9D39",
        .entry = 0x809d39,
        .ret_op = 0x809d5a,  // its one `RTL`
        .ret_kind = COSIM_RTL,
        .run = shim_rng_next,
        // Measured 338..412, mean 355, and the mean is the same on every movie
        // in the corpus to within two cycles — it has one branch and it is three
        // instructions long. The spread is the `BVC` and nothing else.
        .cycles = 355,
        .stack_bytes = 0,  // it calls nothing
    },
    {
        .name = "actor_deeb",
        .symbol = "$82:DEEB",
        .entry = 0x82deeb,
        // `$82:DEF1`, the ignore path's `RTL`, and it has to be that one: the
        // other exit's `SEC` is two instructions before its `RTL`, so landing
        // there would run nothing but the return anyway — but landing on the
        // `CLC` at `$82:DEF0` would clear the carry the port just published.
        .ret_op = 0x82def1,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_deeb_collide,
        // Measured 118..124, mean 119, over the twelve calls
        // `movies/level49.zmv` makes — all of them the id it answers to.
        .cycles = 119,
        .stack_bytes = 0,
    },
    {
        .name = "actor_f1c2",
        .symbol = "$82:F1C2",
        .entry = 0x82f1c2,
        .ret_op = 0x82f1da,  // one of its two bare RTLs; neither touches carry
        .ret_kind = COSIM_RTL,
        .run = shim_actor_f1c2_collide,
        // Measured 256..256 over both calls `movies/level37.zmv` makes.
        .cycles = 256,
        .stack_bytes = 0,
    },
    {
        .name = "actor_f534",
        .symbol = "$81:F534",
        .entry = 0x81f534,
        // `$81:F54E`, the fall-through `RTL`. All three exits clear carry, so
        // as with `enemy_cdde` the choice is not load-bearing — and a bare `RTL`
        // is picked anyway so that it does not become load-bearing by accident.
        .ret_op = 0x81f54e,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_f534_collide,
        // Measured 178..266, mean 216, over the five calls
        // `movies/level21.zmv` makes — 15 marks on the unguarded latch and 10
        // on the guarded one, so both of its stores are in the sample.
        .cycles = 216,
        .stack_bytes = 0,  // it calls nothing and pushes nothing
    },
    {
        .name = "victim_a264",
        .symbol = "$83:A264",
        .entry = 0x83a264,
        // `$83:A2B3`, the ignore path's `RTL`. This one *is* load-bearing in the
        // other direction: four of the six exits set carry, and landing on the
        // `CLC` two bytes back would clear the answer the port published.
        .ret_op = 0x83a2b3,
        .ret_kind = COSIM_RTL,
        .run = shim_victim_a264_collide,
        // Measured 318..358, mean 339, over four calls — and none of them took
        // a path that calls `$81:8191`, so this is the shot-clears exit's cost
        // and nothing else's.
        .cycles = 339,
        // The `JSL $81:8191` it makes on two paths, inlined by the port but not
        // by the ROM.
        .stack_bytes = 4,
    },
    {
        .name = "enemy_cdde",
        .symbol = "$81:CDDE",
        .entry = 0x81cdde,
        // `$81:CDFA`. All four exits are `CLC : RTL`, so unlike the enemy family
        // the choice is not load-bearing here — carry is false whatever this
        // lands on. Pointed at the bare `RTL` anyway, because the day an exit
        // stops clearing carry is not the day to discover the rule was being
        // relied on by accident.
        .ret_op = 0x81cdfa,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_cdde_collide,
        // Measured 118..158, mean 120, over the 32 calls
        // `movies/level29-fighting.zmv` makes — and every one of them took the
        // same branch, so this is the ignore path's cost and nothing else's.
        .cycles = 120,
        // 0 observed, for the same reason: nothing in the corpus reaches the
        // `JSR $CC0A`. Left at 2, which is what that `JSR` will push the day
        // something does, because a budget that is too small fails a call and one
        // that is too large only waives two dead bytes.
        .stack_bytes = 2,
    },
    {
        .name = "enemy_b592",
        .symbol = "$81:B592",
        .entry = 0x81b592,
        // `$81:B59D`, the first of three bare `RTL`s. Every exit clears carry,
        // as `enemy_cdde`'s do, and the same reasoning applies to picking one.
        .ret_op = 0x81b59d,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_b592_collide,
        // Measured 114..154, mean 122, over the five calls
        // `movies/level29-firstaid.zmv` makes — all five the ignore branch.
        .cycles = 122,
        .stack_bytes = 0,  // it calls nothing and pushes nothing
    },
    {
        .name = "boss_9660",
        .symbol = "$82:9660",
        .entry = 0x829660,
        // `$82:9677`, the `RTL` the ignore path clears carry into. The other
        // exit is `$82:96D8` and it *sets* carry — the two are not
        // interchangeable in the ROM, but they are here, because
        // `native_publish` puts the port's flags in place before the core
        // executes this instruction and an `RTL` sets none of them.
        .ret_op = 0x829677,
        .ret_kind = COSIM_RTL,
        .run = shim_boss_9660_collide,
        // Measured 192..792, mean 264, over the 5,126 calls
        // `movies/level25.zmv` makes. The spread is the three ignore paths
        // against the damage path's `SBC $818561,X` — a long-addressed read out
        // of another bank — and the mean is the ignore path's, because 10,032
        // of the 10,112 marks land there.
        .cycles = 264,
        .stack_bytes = 0,  // it calls nothing and pushes nothing
    },
    {
        .name = "boss_aa2e",
        .symbol = "$82:AA2E",
        .entry = 0x82aa2e,
        // `$82:AA48`, the refusals' `CLC : RTL`, for `boss_9660`'s reason.
        .ret_op = 0x82aa48,
        .ret_kind = COSIM_RTL,
        .run = shim_boss_aa2e_collide,
        // Measured 234..868, mean 274, over 3,978 calls on records 20, 40 and
        // 47. The spread is `boss_9660`'s: refusals against the long read of
        // `ENEMY_DAMAGE_TABLE`.
        .cycles = 274,
        .stack_bytes = 0,  // it calls nothing and pushes nothing
    },
    {
        .name = "actor_f330",
        .symbol = "$82:F330",
        .entry = 0x82f330,
        .ret_op = 0x82f34f,  // the `CLC : RTL`'s bare `RTL`
        .ret_kind = COSIM_RTL,
        .run = shim_actor_f330_collide,
        .cycles = 253,  // measured 250..262 over 20 calls, records 31 and 36
        .stack_bytes = 0,
    },
    {
        .name = "actor_a638",
        .symbol = "$81:A638",
        .entry = 0x81a638,
        .ret_op = 0x81a63e,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_a638_collide,
        .cycles = 143,  // measured 140..146 over the 2 calls record 48 makes
        .stack_bytes = 0,
    },
    {
        .name = "actor_84ac",
        .symbol = "$82:84AC",
        .entry = 0x8284ac,
        .ret_op = 0x8284c0,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_84ac_collide,
        .cycles = 102,  // measured 102 over the 5 calls record 12 makes
        .stack_bytes = 0,
    },
    {
        .name = "enemy_b95f",
        .symbol = "$81:B95F",
        .entry = 0x81b95f,
        .ret_op = 0x81b965,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_b95f_collide,
        .cycles = 92,  // measured 84..124 over the 15 calls record 36 makes
        .stack_bytes = 0,  // a survivor leaves through `$81:8506`, which pushes nothing
    },
    {
        .name = "enemy_eff0",
        .symbol = "$82:EFF0",
        .entry = 0x82eff0,
        .ret_op = 0x82eff6,
        .ret_kind = COSIM_RTL,
        .run = shim_enemy_eff0_collide,
        .cycles = 109,  // measured 84..1180 over the 648 calls record 39 makes
        .stack_bytes = 0,
    },
    {
        .name = "actor_c8c3",
        .symbol = "$81:C8C3",
        .entry = 0x81c8c3,
        .ret_op = 0x81c8fa,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_c8c3_collide,
        .cycles = 295,  // measured 228..444 over 28 calls, records 12 and 47
        // `JSR $C6EC` pushes 2; id `$68`'s `JSL $80:9D6A` pushes 3.
        .stack_bytes = 3,
    },
    {
        .name = "shot_collide",
        .symbol = "$81:FE0E",
        .entry = 0x81fe0e,
        // Two `RTL`s, at $FE20 and $FE2E, and they are not interchangeable: the
        // expire path arrives with A = 1 and carry set by a `CMP`, the pass path
        // with A = the id. `native_publish` has already put the port's answer in
        // the registers, so either one returns correctly — this is the shorter.
        .ret_op = 0x81fe20,
        .ret_kind = COSIM_RTL,
        .run = shim_shot_collide,
        .cycles = 40,
        .stack_bytes = 0,  // it pushes nothing at all
    },
    {
        .name = "victim_collide",
        .symbol = "$83:A364",
        .entry = 0x83a364,
        // Six `RTL`s. `$A3C9` is the shared tail two of the exits reach — the
        // entry guard's `BNE` and the `$34` case falling through — and, like
        // `shot_collide`'s, which one is named does not affect what returns:
        // `native_publish` has already put the port's registers in place.
        .ret_op = 0x83a3c9,
        .ret_kind = COSIM_RTL,
        .run = shim_victim_collide,
        .cycles = 40,
        .stack_bytes = 0,  // it pushes nothing at all
    },
    {
        .name = "object_collide",
        .symbol = "$80:CAEE",
        .entry = 0x80caee,
        // Two `RTL`s, at $CB06 and $CB17, and as with the two handlers above
        // which one is named does not affect what returns — `native_publish`
        // has already put the port's registers in place. The shared ignore tail
        // is the shorter.
        .ret_op = 0x80cb06,
        .ret_kind = COSIM_RTL,
        .run = shim_object_collide,
        .cycles = 40,
        .stack_bytes = 0,  // it pushes nothing at all
    },
    {
        // Before `apu_play_sfx`, because it is that routine's callee: the
        // registry reads callees-first so the report reads the way the call
        // chain does.
        //
        // `verify_only` for a long time, because substituting it meant not
        // waiting for the SPC700, and a data-set upload's commands come back
        // to back. The wait is in the trace now (`HW_WAIT8`), made read by
        // read on the ROM's cycles, so it is substituted. It leaves by its
        // `RTS` so that the widths its `SEP #$30` sets go back with it: the
        // caller at `$80:CBC6` does `STZ $1E` next, eight bits wide.
        .name = "apu_send",
        .symbol = "$80:CCC8",
        .entry = 0x80ccc8,
        .run = shim_apu_send,
        .accepts = accepts_apu,
        COSIM_EXITS(APU_SEND_EXITS),
        .cycles = 178,  // a call that did not wait, to the RTS
        .hw = true,
        // An upload's commands wait long enough that the NMI lands in one
        // call in a hundred.
        .through_interrupts = true,
    },
    {
        .name = "apu_play_sfx",
        .symbol = "$80:CC3B",
        .entry = 0x80cc3b,
        .ret_op = 0x80cc4b,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_apu_play_sfx,
        // Measured, and by far the most variable routine in the registry for
        // its size: 484..85,450, because eight of its eleven instructions are a
        // spin on `$2143` and what they cost is how long the SPC700 took to
        // acknowledge the *previous* command. 85,450 is a wait of a frame and a
        // half. The mean is 507 on one movie and 2,134 on another, so no single
        // figure is right — so this is the measured **minimum**, the cost of a
        // call that did not wait, and every substituted call is one of those: a
        // lone sound effect finds the SPC caught up from sounds ago, so
        // `apu_drive`'s spin exits on its first read. Charging the mean would be
        // billing a wait that did not happen.
        .cycles = 484,
        .stack_bytes = 4,  // PHD + PEA, then the `JSR $CCC8` at the same depth
    },
    {
        // Also a callee of the uploader `apu_send` serves, and the third of the
        // three routines the data-set path is made of. Unlike the other two it
        // is ordinary: it touches no register, waits for nothing, and is
        // substituted like anything else.
        .name = "apu_next_byte",
        .symbol = "$80:CCBF",
        .entry = 0x80ccbf,
        .ret_op = 0x80ccc7,  // RTS
        .ret_kind = COSIM_RTS,
        .run = shim_apu_next_byte,
        // 134..202, and the mean is **138 on every movie measured** — the
        // flattest profile in this registry by a distance, because there is one
        // branch in it and it is taken once in 256. Cheaper entries exist (the
        // collision dispatchers bottom out at 40) but none of them is called a
        // quarter of a million times.
        .cycles = 138,
        .stack_bytes = 0,   // no pushes; five instructions and four of them are
                            // a byte-wide increment
    },
    {
        // After the two routines it calls. Reached by `JSR` from `$80:CBEE`
        // and by falling out of `$80:CC6F`, which sends command 8 first; the
        // second is not a call, and is counted as one served. A handful a
        // level.
        .name = "apu_load_set",
        .symbol = "$80:CC7C",
        .entry = 0x80cc7c,
        .run = shim_apu_load_set,
        .supported = supported_apu_load_set,
        .supported_readonly = true,
        COSIM_EXITS(APU_LOAD_SET_EXITS),
        .cycles = 100000,
        .hw = true,
        .through_interrupts = true,
    },
    {
        // `$80:CB1A`, with the IPL upload at `$80:CB61` inside it, its only
        // caller. Once a boot, from the reset at `$80:815B`.
        .name = "apu_boot",
        .symbol = "$80:CB1A",
        .entry = 0x80cb1a,
        .run = shim_apu_boot,
        .accepts = accepts_apu,
        COSIM_EXITS(APU_BOOT_EXITS),
        .cycles = 2000000,
        .hw = true,
        .through_interrupts = true,
    },
    {
        .name = "spawn_has_room",
        .symbol = "$80:9D5B",
        .entry = 0x809d5b,
        .ret_op = 0x809d69,  // RTL; both compares fall onto it
        .ret_kind = COSIM_RTL,
        .run = shim_spawn_has_room,
        // 112..198, call-weighted 150 over 15,393 calls on five movies. The
        // two ends are the two exits: 112 is a refusal on the census, which
        // is three instructions, and 198 is both compares. The mean sits
        // nearer the top because most calls get past the first ceiling.
        .cycles = 150,
        .stack_bytes = 0,
    },
    {
        .name = "sin_deg",
        .symbol = "$80:9C90",
        .entry = 0x809c90,
        .ret_op = 0x809cb1,  // RTS, after the PLX that decides N and Z
        .ret_kind = COSIM_RTS,
        .run = shim_sin_deg,
        // 240..324, mean 286 — and **2,676 calls, to the call, on every one of
        // the five movies measured**, which are different levels of different
        // lengths with different inputs. Bisecting `level1` says why: nothing
        // reaches it before frame 900 and nothing reaches it after frame 1,200,
        // so all 2,676 are one burst in the level-entry transition and none of
        // them is play. The count is a property of the ROM, not of the input,
        // which is a thing very few rows in this table can say.
        .cycles = 286,
        .stack_bytes = 2,  // the PHX, and the index register is sixteen bits
                           // because the table is 360 entries long — which the
                           // harness's measured stack column independently
                           // confirms
    },
    {
        .name = "actor_publish_pos",
        .symbol = "$80:F327",
        .entry = 0x80f327,
        .ret_op = 0x80f337,  // the single-record RTS; the other exit is $F353
        .ret_kind = COSIM_RTS,
        .run = shim_actor_publish_pos,
        // 244..614, call-weighted 353 over 17,076 calls on five movies. The
        // floor is the single-record path — six instructions — and the ceiling
        // is the stacked pair, which is eleven and does four more memory
        // accesses. The mean therefore reads as *what fraction of the board is
        // two records tall*, and it moves the most of anything in this
        // registry between movies: 251 on `level25-lane` against 413 on
        // `level1-2p`.
        .cycles = 353,
        .stack_bytes = 0,
    },
    {
        .name = "nearest_player_dist",
        .symbol = "$81:8024",
        .entry = 0x818024,
        .ret_op = 0x81807d,  // RTS
        .ret_kind = COSIM_RTS,
        .run = shim_nearest_player_dist,
        // 556..978, call-weighted 643 over 8,240 calls on five movies, and the
        // one routine in this round whose cost is a straight function of how
        // many players are on the board: four one-player movies all land
        // between 598 and 609, and `level1-2p` alone measures 918. The second
        // player is the second half of the routine, and it is the only input
        // that turns it on.
        .cycles = 643,
        .stack_bytes = 0,
    },
    {
        .name = "wave_hdma_build",
        .symbol = "$80:9570",
        .entry = 0x809570,
        .ret_op = 0x8095da,  // RTS; every path in the routine converges on it
        .ret_kind = COSIM_RTS,
        .run = shim_wave_hdma_build,
        // **The most expensive substitutable routine in the registry**, ahead
        // of `$82:8069 boss_bg_queue_flip` at 89,008 and `sprite_build_oam` at
        // 43,111 — and unlike either of those it is neither a DMA nor a pass
        // over the whole board. 68,996..164,382, call-weighted 115,606 over
        // 2,021 calls on seven movies — a third of a frame's CPU budget in one
        // call, because the loop makes ~223 far calls to `sin_deg` and does two
        // long-addressed stores per scanline.
        //
        // The distribution is two populations rather than one. Every level
        // movie measures exactly 12 calls at 163,164..163,514, the full-length
        // table built twelve times in the level-entry transition; `boot.zmv`
        // makes 1,077 at a mean of 96,406 and a floor of 1,176, which is the
        // title sequence holding the wobble and then retracting it two bytes at
        // a time until there is almost no table left to build. `level45-race`
        // sits between the two at 884 calls and 135,762.
        //
        // So `.cycles` is a number no call is ever near, and until the model
        // above existed the budget drift table measured what that was worth:
        // -47,703 cycles a call on a level movie, two orders of magnitude
        // worse per call than anything else in the registry. It is kept because
        // a declared mean is what a routine falls back to when its shim reports
        // nothing, and this one always reports — but it is the fallback now and
        // not the price.
        .cycles = 115606,
        .stack_bytes = 7,  // JSL (3) into the trampoline, its JSR (2), and the
                           // PHX inside sin_deg (2)
    },
    {
        .name = "actor_step_bearing",
        .symbol = "$81:9BF3",
        .entry = 0x819bf3,
        .ret_op = 0x819c61,  // RTS, shared with the rest-frame exit
        .ret_kind = COSIM_RTS,
        .run = shim_actor_step_bearing,
        // 134..16,904, mean 5,154 over 16,569 calls — and the spread is the
        // whole story: 134 is a rest frame, which is four instructions, and the
        // ceiling is six calls into three other ported routines, two of which
        // walk the visible-actor list. One movie in the corpus reaches it at
        // all (`level21-bubble`), which is what a per-actor behaviour looks
        // like from here.
        .cycles = 5154,
        .stack_bytes = 7,  // the deepest of the three JSLs: $80:BF67's own
                           // PHD and PEA under the call's three bytes
    },
    {
        .name = "monster_anim",
        .symbol = "$81:C16B",
        .entry = 0x81c16b,
        .ret_op = 0x81c1a6,  // the RTS after the JSR; the mirror exits at $C1B0
        .ret_kind = COSIM_RTS,
        .run = shim_monster_anim,
        // 280..1,166, call-weighted 473 over 26,736 calls on the two movies
        // that meet the creature — `level25-lane` and `level45-race`, at 461
        // and 480, which is as close as two movies get in this table.
        .cycles = 473,
        .stack_bytes = 2,  // the JSR into $81:C00B on three of the four paths
    },
    {
        .name = "monster_place_carried",
        .symbol = "$81:C00B",
        .entry = 0x81c00b,
        .ret_op = 0x81c025,  // RTS
        .ret_kind = COSIM_RTS,
        .run = shim_monster_place_carried,
        // 104..400, call-weighted 120 over 24,422 calls, and the two ends are
        // the two paths: 104 is the `CPY #$FFFF` guard and an `RTS`, and the
        // rest is two table reads and two coordinate adds. The 2,314-call gap
        // between this and `monster_anim` is the mirrored exit that returns
        // without calling it.
        .cycles = 120,
        .stack_bytes = 0,
    },
    {
        .name = "monster_seek",
        .symbol = "$81:BB75",
        .entry = 0x81bb75,
        // Three exits in two routines: `$BB92`, `$BBA3`, and the `RTS` at
        // `$81:BEE2` that the `JMP $BEDA` tail returns through. This is the
        // first of them, chosen because it is the one that touches nothing.
        .ret_op = 0x81bb92,
        .ret_kind = COSIM_RTS,
        .run = shim_monster_seek,
        // 6,494..10,948, call-weighted 7,964 over 27,280 calls on four movies.
        // Eleven of its own instructions and one `JSL actor_nearest`, which is
        // 32 slots walked whatever the board looks like — so this budget is
        // almost entirely the callee's, and the 4,454-cycle spread is how many
        // of those 32 hold something worth measuring.
        .cycles = 7964,
        .stack_bytes = 7,  // the `JSL`s' three bytes with `actor_nearest`'s own
                           // PHD and `player_in_range`'s deeper pushes on top
    },
    {
        .name = "monster_deliver",
        .symbol = "$81:BBA4",
        .entry = 0x81bba4,
        .ret_op = 0x81bbea,  // likewise: the plain `RTS`, not the stub's
        .ret_kind = COSIM_RTS,
        .run = shim_monster_deliver,
        // **Not measured, because no movie in the corpus reaches it.** The
        // creature has to pick somebody up and carry them, and 43 movies never
        // once do; the profiler agrees, counting zero calls at `$81:BBA4` in
        // every one of the eleven traces. This is `monster_seek`'s figure,
        // which is defensible rather than measured: the two routines make the
        // same single `JSL actor_nearest` and that call is nearly all of the
        // budget, and the extra work here is three compares and, on one path,
        // an `actor_slot_free`. It has never been spent and, on the evidence,
        // may never be.
        .cycles = 7964,
        .stack_bytes = 7,  // likewise `actor_nearest`'s, which is the deepest
                           // of the two calls it can make
    },
    {
        .name = "actor_slot_alloc",
        .symbol = "$80:BE0C",
        .entry = 0x80be0c,
        .ret_op = 0x80be3a,  // the success RTL; a full board returns at $BE27
        .ret_kind = COSIM_RTL,
        .run = shim_actor_slot_alloc,
        // 458..2,578, call-weighted 1,277 over 1,606 calls on six movies. The
        // floor is a first-slot hit and the ceiling is a scan that walked all
        // 32, so the mean reads as **how full the board is**: 1,008 on
        // `level9-weapons` against 1,631 on `level25-lane`. It is the one
        // routine in this registry whose cost is a linear search nothing
        // bounds but the array.
        .cycles = 1277,
        .stack_bytes = 3,  // PHB, and the PEA that costs a byte more than it
                           // needs to
    },
    {
        .name = "actor_slot_free",
        .symbol = "$80:BE41",
        .entry = 0x80be41,
        .ret_op = 0x80be7c,  // RTL; the two declines branch straight to it
        .ret_kind = COSIM_RTL,
        .run = shim_actor_slot_free,
        // 502..2,288, call-weighted 1,071 over 1,552 calls on six movies, and
        // the shape is the same as the allocator's for the same reason: the
        // unlink walks the list, so a long list is a slow free. 502 is a
        // decline, which is four instructions.
        .cycles = 1071,
        .stack_bytes = 2,  // PHD
    },
    {
        .name = "weapon_select_next",
        .symbol = "$80:EA63",
        .entry = 0x80ea63,
        .ret_op = 0x80eaa3,  // RTS; both exits converge on it
        .ret_kind = COSIM_RTS,
        .run = shim_weapon_select_next,
        .cycles = 3227,
        // The unchanged exit pushes nothing — `level1-rescue.zmv` measures 0 —
        // and a change adds the `JSR $EA4B` and the `JSL apu_play_sfx` under it.
        .stack_bytes = 7,
    },
    {
        .name = "item_select_next",
        .symbol = "$80:EAA8",
        .entry = 0x80eaa8,
        .ret_op = 0x80eae0,  // RTS; both exits converge on it, as next door
        .ret_kind = COSIM_RTS,
        .run = shim_item_select_next,
        // Measured on `movies/level1-keys.zmv`, the only movie that reaches it
        // at all: five calls spanning 1,038..12,486, and this is their mean.
        // The spread is `apu_play_sfx`'s — the two exits differ by a sound
        // effect, and a sound effect's cost is how long the SPC700 took to
        // acknowledge the last one.
        .cycles = 4316,
        .stack_bytes = 4,  // the `JSL apu_play_sfx` the changed exit ends with
    },
    {
        .name = "score_add",
        .symbol = "$80:C7D9",
        .entry = 0x80c7d9,
        .ret_op = 0x80c818,  // the discard entry's bare RTL; all three return alike
        .ret_kind = COSIM_RTL,
        .run = shim_score_add,
        .supported = guard_score_add,
        .supported_readonly = true,
        .cycles = 524,
        .stack_bytes = 4,  // the opening `PHX`, plus the `JSR $C7C2` under it
    },
    {
        .name = "actor_collide_notify",
        .symbol = "$80:BE8F",
        .entry = 0x80be8f,
        .ret_op = 0x80bec8,  // RTS
        .ret_kind = COSIM_RTS,
        .run = shim_actor_collide_notify,
        .supported = guard_actor_collide_notify,
        .fast_guard = fast_guard_actor_collide_notify,
        .cycles = 2842,
        .stack_bytes = 22,   // the `JSL $80:8480` it ends on, and all of what that reaches
    },
    {
        .name = "actor_overlap_pass",
        .symbol = "$80:BEC9",
        .entry = 0x80bec9,
#if defined(XBOX_PORT) && defined(ZAMN_R50_NATIVE_HOTPATHS)
        .run = shim_overlap_scan, .accepts = accepts_overlap_scan,
        COSIM_EXITS(overlap_scan_exits), .uncalled = true,
#else
        .ret_op = 0x80bf1a,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_actor_overlap_pass,
        .supported = guard_actor_overlap_pass,
#endif
        .cycles = 4426,
        .stack_bytes = 26,  // its `PHY`, plus the deepest the dispatch under it goes
    },
    {
        .name = "sprite_build_oam",
        .symbol = "$80:BD1F",
        .entry = 0x80bd1f,
        .ret_op = 0x80bde2,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_sprite_build_oam,
        .supported = guard_sprite_build_oam,
#if defined(XBOX_PORT) && defined(ZAMN_R56_SAFE_OAM_CUTOVER)
        .fast_rom_guard = r56_fast_guard_oam,
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R53_NATIVE_MOVEMENT)
        .fast_guard = r53_fast_guard_empty_oam,
#endif
#if defined(XBOX_PORT) && defined(ZAMN_R40_FAST_OAM_PREFLIGHT)
        // The R40 preflight touches only bank $7E.  Collision handlers use
        // 16-bit direct/absolute addresses and the sprite safety checks read
        // the actor/OAM/thread state from the same bank, so there is no reason
        // to clone the untouched $7F half for this guard.
        .guard_copy_bytes = 0x10000u,
#endif
        .excludes = BUILD_OAM_EXCLUDES,
        .exclude_count = 1,
        // The most variable routine in the registry: 5,864 when nothing is on
        // screen and **137,160** on `level25-boss.zmv`, where the ceiling used
        // to read 66,412 because no movie had put that much on the board. A
        // whole NTSC frame is 357,366 master cycles — see `COSIM_FRAME_CYCLES`,
        // not the 57,000 this comment claimed — so the worst pass is 38% of a
        // frame and the mean is 12%.
        //
        // 5,014 of the 6,042 calls `level25-boss` makes are priced; the other
        // 1,028 are the ones whose collisions land on a handler with no cost
        // table, and they are also the expensive ones, so the fallback mean of
        // 43,111 leaves the largest single debt in the drift table — -41.7M
        // cycles over that movie. That is the next thing to fix and it is a
        // guard to widen, not a model to write.
        .cycles = 43111,
        .stack_bytes = 32,  // PHB + PHD + the deepest nested JSR/JSL
    },
    {
        .name = "fade_in",
        .symbol = "$80:891A",
        .entry = 0x80891a,
        .end = 0x808933,     // one past the `RTL`; bounds the yield-site test
        .ret_op = 0x808932,  // RTL
        .ret_kind = COSIM_RTL,
        .run_yield = shim_fade_in,
        .yield_op = 0x808923,  // the `JSL thread_yield` native mode jumps to
        .ctx_size = (int)sizeof(FadeCtx),
        // Per *segment*, not per call: what `verify` measured a run between two
        // suspensions to cost (162..238, mean 201 over 16 segments).
        .cycles = 199,
        .stack_bytes = 3,  // pushes nothing of its own
    },
    {
        .name = "actor_nearest",
        .symbol = "$80:B123",
        .entry = 0x80b123,
        .ret_op = 0x80b18e,  // RTL, after the PLD that undoes the opening PHD
        .ret_kind = COSIM_RTL,
        .run = shim_actor_nearest,
        // A fixed 32 slots whatever the board holds, so the spread is narrow
        // and it is all in how many records get as far as the subtraction:
        // 6,498..7,592 over 1,006 calls on movies/level1.zmv. The fallback
        // now, and not the price — `nearest_cycles` reports a real one on
        // every call, and being unable to decline it never reports anything
        // else. See `NEAREST_COST`.
        .cycles = 7195,
        .stack_bytes = 2,  // the opening PHD
    },
    {
        // Before its two callers, because the registry reads callees-first.
        .name = "actor_gap",
        .symbol = "$80:B093",
        .entry = 0x80b093,
        // Two `RTS`es, at $B0B6 and $B0BA, and neither is preceded by anything
        // that has to run: `native_publish` has already put the answer and the
        // flags in place, and an `RTS` writes none of either.
        .ret_op = 0x80b0b6,
        .ret_kind = COSIM_RTS,
        .run = shim_actor_gap,
        // 88..490 over 44,164 calls on ten movies, call-weighted. The floor is
        // the empty-record exit, which is four instructions, and the ceiling is
        // both absolute values being taken; nothing here varies with the board,
        // so this is as tight as a budget in the registry gets.
        .cycles = 230,
        .stack_bytes = 0,  // it pushes nothing at all
    },
    {
        .name = "actor_nearest_id3",
        .symbol = "$80:B18F",
        .entry = 0x80b18f,
        .ret_op = 0x80b1eb,  // RTL, after the PLD that undoes the opening PHD
        .ret_kind = COSIM_RTL,
        .run = shim_actor_nearest_id3,
        // 5,482..8,256 over 147 calls on five movies. A fixed 32 slots like
        // `actor_nearest`, and a wider spread than its 6,498..7,592 for the
        // opposite reason to the usual one: with one id instead of four, more
        // slots are dismissed before the subtraction and fewer after it.
        // Also the fallback and not the price: the same `NEAREST_COST` table
        // prices this, through its own two id blocks.
        .cycles = 6909,
        .stack_bytes = 2,  // the opening PHD
    },
    {
        .name = "actor_bearing_point",
        .symbol = "$80:B1EC",
        .entry = 0x80b1ec,
        .ret_op = 0x80b21d,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_actor_bearing_point,
        // 604..748 over 35 calls on two movies, which is the thinnest sample in
        // the registry and is the sample the corpus has: three call sites, and
        // 45 executions of them in eleven traced movies.
        .cycles = 679,
        .stack_bytes = 4,  // the opening PHD and the PHA under it
    },
    {
        .name = "actor_bearing",
        .symbol = "$80:B22A",
        .entry = 0x80b22a,
        .ret_op = 0x80b25e,  // RTL
        .ret_kind = COSIM_RTL,
        .run = shim_actor_bearing,
        // 514..760 over 12,172 calls on ten movies, call-weighted. Straight-line
        // code with two branches in it, so the spread is only which of them are
        // taken.
        .cycles = 611,
        .stack_bytes = 2,  // the opening PHD
    },
    {
        .name = "player_in_range",
        .symbol = "$80:B26B",
        .entry = 0x80b26b,
        // Three `RTL`s — $B298, $B29E, $B2A4 — and which one is named does not
        // affect what returns, for the same reason as everywhere else here.
        .ret_op = 0x80b298,
        .ret_kind = COSIM_RTL,
        .run = shim_player_in_range,
        // 1,002..1,298 over 3,385 calls on five movies, call-weighted, and both
        // `actor_gap` calls are inside it — which is the whole of why it costs
        // four and a half times what one of those does.
        .cycles = 1104,
        .stack_bytes = 4,  // the opening PHD and the PEA under it
    },
    {
        .name = "player_bearing",
        .symbol = "$80:B2A5",
        .entry = 0x80b2a5,
        .ret_op = 0x80b2d2,  // the RTL on the nobody-in-range path
        .ret_kind = COSIM_RTL,
        .run = shim_player_bearing,
        // 1,002..1,636 over 18,697 calls on seven movies, call-weighted. The
        // floor is the nobody-in-range exit and the ceiling is the direction
        // path with its stack traffic and its long-addressed table read, and the
        // mean sits near the top because 99 calls in 100 find somebody.
        .cycles = 1548,
        // The opening PHD, the PEI that saves the winning distance, and the PHY
        // under that — the deepest of the six, and only on the direction path.
        .stack_bytes = 6,
    },
    {
        .name = "actor_at_point",
        .symbol = "$80:BF67",
        .entry = 0x80bf67,
        .ret_op = 0x80bfc0,  // the RTL on the found path, after its SEC
        .ret_kind = COSIM_RTL,
        .run = shim_actor_at_point,
        // 698..6,958 across seven movies, and the spread is the board: unlike
        // `actor_nearest`'s fixed 32 slots this walks only what the cull kept,
        // and it stops early when it finds something. 2,645 is the mean
        // weighted by the 26,796 calls those movies made, not one movie's —
        // and it is the fallback now, not the price. See `AT_POINT_COST`.
        .cycles = 2645,
        .stack_bytes = 4,  // the opening PHD and the PEA under it
    },
    {
        .name = "actor_obstacle_at_point",
        .symbol = "$80:BFC8",
        .entry = 0x80bfc8,
        .ret_op = 0x80c041,  // the RTL on the blocked path, after its SEC
        .ret_kind = COSIM_RTL,
        .run = shim_actor_obstacle_at_point,
        // 426..7,794, call-weighted over 34,211 calls on eleven movies rather
        // than taken from one. Borrowing `actor_at_point`'s 2,645 would have
        // been 27% low: same loop, but a filter that accepts far fewer ids
        // means far fewer early exits, so the walk usually runs to the end.
        // `OBSTACLE_COST` prices it per call now, so this is the fallback —
        // and that 27% is the measurement of why it could not be borrowed.
        .cycles = 3630,
        .stack_bytes = 4,  // the opening PHD and the PEA under it
    },
    {
        .name = "camera_window_update",
        .symbol = "$80:A54D",
        .entry = 0x80a54d,
        .ret_op = 0x80a587,
        .ret_kind = COSIM_RTS,
        .run = shim_camera_window_update,
        // 608..648 over 227 calls. Twenty-seven instructions and no branch,
        // so the whole 40-cycle band is the bus.
        .cycles = 613,
        .stack_bytes = 0,  // no push at all
    },
    {
        .name = "camera_split_y",
        .symbol = "$80:A588",
        .entry = 0x80a588,
        .ret_op = 0x80a598,
        .ret_kind = COSIM_RTS,
        .run = shim_camera_split_y,
        // 206..246 over 203 calls; six instructions, same band.
        .cycles = 210,
        .stack_bytes = 0,
    },
    {
        .name = "vram_queue_request",
        COSIM_COMMIT(COMMIT_RENDER_FLAGS),
        .symbol = "$80:9E6D",
        .entry = 0x809e6d,
        .ret_op = 0x809e7a,
        .ret_kind = COSIM_RTS,
        .run = shim_vram_queue_request,
        // 166..206. The three exits are 4, 5 and 7 instructions, so this is
        // a mean over paths as well as over the bus -- narrow because the
        // paths barely differ.
        .cycles = 173,
        .stack_bytes = 0,
    },
    {
        .name = "tilemap_copy_row",
        .symbol = "$80:A61D",
        .entry = 0x80a61d,
        .ret_op = 0x80a64e,
        .ret_kind = COSIM_RTS,
        .run = shim_tilemap_copy_row,
        .supported = guard_tilemap_copy_row,
        .supported_readonly = true,
        // 8,074..10,126. The loop count is fixed at 33, so unlike
        // `tilemap_copy_column` this spread is the priority branch and
        // the bus rather than a variable trip count -- a 2,000-cycle band
        // around a routine that always does the same amount of work.
        .cycles = 9314,
        // The JSL to $80:AD1C is 3 and its own PHA is 2 under that. The
        // JSR to $80:A401 only reaches 4, so 5 is the floor.
        .stack_bytes = 5,
    },
    {
        .name = "tilemap_buffer_alloc",
        .symbol = "$80:A401",
        .entry = 0x80a401,
        .ret_op = 0x80a415,
        .ret_kind = COSIM_RTS,
        .run = shim_tilemap_buffer_alloc,
        .supported = guard_tilemap_buffer_alloc,
        .supported_readonly = true,
        // 342..382, and the 40 cycles are the bus. The spin would put the
        // ceiling in the thousands; it has never once been entered, which
        // is the same thing the guard's zero declines says from the front.
        .cycles = 370,
        .stack_bytes = 2,  // the PHA it reads back through `$01,S`
    },
    {
        .name = "tilemap_copy_column",
        .symbol = "$80:A5E5",
        .entry = 0x80a5e5,
        .ret_op = 0x80a61c,
        .ret_kind = COSIM_RTS,
        .run = shim_tilemap_copy_column,
        // 1,526..14,250 over 303 calls, and unlike every other spread in
        // this registry it is not the bus or a branch: it is the count.
        // The loop body is fixed, so the cost is linear in how many tiles
        // the caller asked for, and the mean is a mean over strip lengths.
        .cycles = 8411,
        // Its own PHA, the JSL to $80:AD1C, and that routine's PHA under
        // it -- 2 + 3 + 2, which is what `verify` measured.
        .stack_bytes = 7,
    },
    {
        .name = "tilemap_tile_addr",
        .symbol = "$80:AD1C",
        .entry = 0x80ad1c,
        .ret_op = 0x80ad2a,
        .ret_kind = COSIM_RTL,
        .run = shim_tilemap_tile_addr,
        // 250..290, and the 40-cycle spread is the bus rather than the
        // routine: there are no branches in it at all.
        .cycles = 258,
        .stack_bytes = 2,  // the PHA it reads back through `$01,S`
    },
    {
        .name = "lzss_decompress",
        .symbol = "$80:CD20",
        .entry = 0x80cd20,
        .ret_op = 0x80cdd9,  // the RTL, after the PLD that restores the caller
        .ret_kind = COSIM_RTL,
        .run = shim_lzss_decompress,
        .supported = guard_lzss_decompress,
        .supported_readonly = true,
        // Never used: the shim reports the call's exact cost and every call
        // reports one. It is level 1's first stream — 4.5 frames, and the
        // smallest of the five that movie makes — so the scale is on show and
        // nothing silently falls back to a number nobody measured.
        .cycles = 1591682,
        // How deep it goes, not how much it leaks — everything balances by the
        // `RTL`. `PHD` and `PEA $0000` put it 4 down, the `PLD` at `$CD24`
        // takes 2 back, and the deepest point is inside a match: `PHA` and
        // `PHY` at `$CD82`, then the `JSR` to a byte helper.
        .stack_bytes = 8,
        .run_only = true,
    },
    {
        .name = "lzss_read_byte",
        .symbol = "$80:CDDA",
        .entry = 0x80cdda,
        .ret_op = 0x80cde8,  // the CLC path's RTS; the SEC path's is two later
                             // and identical, and RTS touches no flag
        .ret_kind = COSIM_RTS,
        .run = shim_lzss_read_byte,
        // 98..298 over 37,923 calls, and the spread is the whole routine:
        // the floor is the four-instruction `SEC` exit at the end of a
        // stream, the ceiling the eight-instruction read.
        .cycles = 266,
        .stack_bytes = 0,  // fifteen bytes, no push
        .verify_only = true,
    },
    {
        .name = "lzss_write_byte",
        .symbol = "$80:CDEB",
        .entry = 0x80cdeb,
        .ret_op = 0x80cdf3,
        .ret_kind = COSIM_RTS,
        .run = shim_lzss_write_byte,
        // 170..210 over 69,755 calls. No branches, so the 40-cycle spread
        // is the bus: `STA [$2C]` into WRAM against the same into a
        // register-mapped page.
        .cycles = 175,
        .stack_bytes = 0,
        .verify_only = true,
    },
    {
        .name = "terrain_blocked",
        .symbol = "$80:AE14",
        .entry = 0x80ae14,
        .ret_op = 0x80ae96,  // the RTL, after the PLD that undoes the PHD
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_blocked,
        // 678..1,776, call-weighted over 41,635 calls on eleven movies. The
        // spread is how many of the six probes it gets through before one of
        // them blocks, and the floor is a first probe that already has.
        .cycles = 1591,
        .stack_bytes = 4,  // the PHD, and the PHA/PLA that stashes X under it
    },
    {
        .name = "terrain_blocked_enemy",
        .symbol = "$80:AE97",
        .entry = 0x80ae97,
        .ret_op = 0x80af28,  // the RTL on the clear path, after its CLC
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_blocked_enemy,
        // 696..1,842 over 45,015 calls — the same loop, so the same shape.
        .cycles = 1578,
        .stack_bytes = 4,
    },
    {
        .name = "terrain_out_of_bounds",
        .symbol = "$80:B422",
        .entry = 0x80b422,
        .ret_op = 0x80b444,  // the RTL both the compare exits reach
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_out_of_bounds,
        // 138..390 over 59,422 calls, and the narrowest spread of anything in
        // this registry: six exits, none of them a loop.
        .cycles = 359,
        .stack_bytes = 0,  // it pushes nothing at all
    },
    {
        .name = "terrain_blocked_wide",
        .symbol = "$82:90F7",
        .entry = 0x8290f7,
        .ret_op = 0x829188,  // the RTS on the clear path, after its PLD
        .ret_kind = COSIM_RTS,
        .run = shim_terrain_blocked_wide,
        // 634..3,268, call-weighted over 127,455 calls on five movies, and the
        // most expensive leaf in the registry. The floor is the first tile
        // failing the priority test; the ceiling is all ten probes, both reads
        // each, with nothing found — and because the loop is unrolled the
        // ceiling is a straight line rather than an iteration count.
        //
        // Now the fallback and not the price: `WIDE_COST` reports each call
        // from what that call actually probed. Both ends of the range above are
        // block sums — see `wide_cycles`.
        .cycles = 3167,
        .stack_bytes = 4,  // the PHD, and the PHA/PLA that stashes X under it
    },
    {
        .name = "terrain_point_bit2",
        .symbol = "$80:AF2C",
        .entry = 0x80af2c,
        .ret_op = 0x80af62,  // the RTL on the clear path, after its CLC
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_point_bit2,
        // 1,046..1,086 over 4,477 calls on `level25-lane`, and the flattest
        // profile of anything in this registry: forty cycles between the
        // cheapest call and the dearest. There is no loop and the bounds test
        // in front of it is nearly as straight, so what looks like two paths
        // costs the same either way.
        .cycles = 1077,
        .stack_bytes = 5,  // the PHD, then the JSL under it — the deeper path
    },
    {
        .name = "terrain_footprint_bit12",
        .symbol = "$80:AF66",
        .entry = 0x80af66,
        .ret_op = 0x80aff7,  // the RTL all six probes reach, after its CLC
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_footprint_bit12,
        // 810..2,128 over 166 calls on `level49-corner`, the only movie in the
        // corpus that reaches it. The floor is the first probe answering and
        // the ceiling is all six — the same shape as `terrain_blocked`, run
        // the other way round.
        .cycles = 885,
        .stack_bytes = 4,
    },
    {
        .name = "terrain_tile_bit3",
        .symbol = "$80:B03B",
        .entry = 0x80b03b,
        .ret_op = 0x80b05b,  // the RTL on the clear path
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_tile_bit3,
        // 700..746 over 426 calls on `level5`, `level21` and `level21-spin`,
        // the only three movies that reach it. Forty-six cycles of spread and
        // no loop: the tile either has the bit or it does not.
        .cycles = 721,
        .stack_bytes = 7,  // the PHD, the JSL, and tilemap_tile_addr's own PHA
    },
    {
        .name = "terrain_point_bit8",
        .symbol = "$80:B05F",
        .entry = 0x80b05f,
        .ret_op = 0x80b08f,  // the RTL on the clear path
        .ret_kind = COSIM_RTL,
        .run = shim_terrain_point_bit8,
        // 630..676 over 5,806 calls on `level37-e6e4`. One tile, no bounds
        // test in front of it, so it is `terrain_point_bit2` minus the `JSL`.
        .cycles = 655,
        .stack_bytes = 4,
    },
    {
        .name = "partner_near",
        .symbol = "$80:AFFB",
        .entry = 0x80affb,
        .ret_op = 0x80b038,  // the RTL on the far path, after its CLC
        .ret_kind = COSIM_RTL,
        .run = shim_partner_near,
        // 152..192 over **three calls in the whole corpus**, and the cheapest
        // entry in this registry — because all three are one-player, where the
        // second `BIT` answers eleven instructions in and nothing is measured.
        // What a two-player call costs is not known, and the corpus has never
        // made one.
        .cycles = 165,
        .stack_bytes = 0,  // no PHD and no pushes, the same as $80:B422
    },
    {
        .name = "step_propose",
        .symbol = "$80:E450",
        .entry = 0x80e450,
        .ret_op = 0x80e485,  // the RTS, after the store the flags do not come from
        .ret_kind = COSIM_RTS,
        .run = shim_step_propose,
        // 536..696, call-weighted over 32,306 calls on eleven movies. The
        // narrowest spread in the registry after `terrain_out_of_bounds`:
        // there is no loop in it, and the 160 cycles are the second add.
        .cycles = 647,
        .stack_bytes = 0,  // no pushes; it is two table reads and four adds
    },
    {
        .name = "step_tether_blocked",
        .symbol = "$80:A8B3",
        .entry = 0x80a8b3,
        .ret_op = 0x80a8eb,  // the RTL on the allowed path, after its CLC
        .ret_kind = COSIM_RTL,
        .run = shim_step_tether_blocked,
        // 322..1,450, call-weighted over 52,988 calls on eleven movies, and
        // the widest ratio in the registry that is not a loop: 322 is the
        // one-player exit eleven instructions in, 1,450 is the far path with
        // four absolute differences in it. Which one a movie gets is decided
        // entirely by whether a second player is on the board.
        .cycles = 519,
        .stack_bytes = 2,  // the opening PHD, and nothing else
    },
    {
        .name = "camera_split_x",
        .symbol = "$80:A599",
        .entry = 0x80a599,
        .ret_op = 0x80a5c0,  // the first branch's RTS; the other is at $A5E4
                             // and RTS touches no flag either way
        .ret_kind = COSIM_RTS,
        .run = shim_camera_split_x,
        // 452..522, call-weighted over 455 calls on six movies. Twenty-two
        // instructions, no loop, and the two branches are the same length, so
        // the whole 70-cycle spread is the bus.
        .cycles = 458,
        .stack_bytes = 0,
    },
    {
        .name = "camera_scroll_left",
        COSIM_COMMIT(COMMIT_VRAM_QUEUE),
        .symbol = "$80:A68B",
        .entry = 0x80a68b,
        .ret_op = 0x80a709,  // the one RTL all three exits reach
        .ret_kind = COSIM_RTL,
        .run = shim_camera_scroll_left,
        .supported = guard_camera_scroll_left,
        .supported_readonly = true,
        // 94..17,032, call-weighted over 15,896 calls on six movies, and the
        // widest spread in the registry by a distance -- 180x, where the next
        // worst is `tilemap_copy_column`'s 9x. Three exits of wildly different
        // lengths is only half of it; the other half is that *which* exit a
        // movie takes is a property of the movie. `movies/level49.zmv` holds
        // the camera against the left edge of the map for its whole length and
        // contributes 13,560 calls at 94 cycles each, which is what drags this
        // mean down to a value no single call has ever cost.
        .cycles = 394,
        .stack_bytes = 9,
    },
    {
        .name = "camera_scroll_right",
        COSIM_COMMIT(COMMIT_VRAM_QUEUE),
        .symbol = "$80:A70A",
        .entry = 0x80a70a,
        .ret_op = 0x80a788,  // two RTLs, at $A711 and here; this is the tail
        .ret_kind = COSIM_RTL,
        .run = shim_camera_scroll_right,
        .supported = guard_camera_scroll_right,
        .supported_readonly = true,
        // 116..18,476 over 2,504 calls. Same shape as its mirror, without a
        // movie that pins the camera against this edge -- so the mean lands
        // near the strip path rather than far below every call.
        .cycles = 1883,
        .stack_bytes = 9,
    },
    {
        .name = "camera_scroll_down",
        COSIM_COMMIT(COMMIT_VRAM_QUEUE),
        .symbol = "$80:A789",
        .entry = 0x80a789,
        .ret_op = 0x80a815,  // the early exits share an RTL at $A790
        .ret_kind = COSIM_RTL,
        .run = shim_camera_scroll_down,
        .supported = guard_camera_scroll_down,
        .supported_readonly = true,
        // 116..14,578 over 10,378 calls, and the same bimodality one axis
        // over: `level13` and `level29-fighting` between them are 7,304 calls
        // that cross no tile boundary at all.
        .cycles = 342,
        .stack_bytes = 7,
    },
    {
        .name = "camera_scroll_up",
        COSIM_COMMIT(COMMIT_VRAM_QUEUE),
        .symbol = "$80:A816",
        .entry = 0x80a816,
        .ret_op = 0x80a8a3,  // ...and this one's at $A81B
        .ret_kind = COSIM_RTL,
        .run = shim_camera_scroll_up,
        .supported = guard_camera_scroll_up,
        .supported_readonly = true,
        // 250..15,032 over 2,159 calls -- the narrowest of the four, because
        // nothing in the corpus scrolls upward for long without stopping.
        .cycles = 1731,
        .stack_bytes = 7,
    },
    {
        .name = "camera_follow",
        .symbol = "$80:A93F",
        .entry = 0x80a93f,
        .ret_op = 0x80a9cb,  // the main RTL; the early exits share one at $A952
                             // and both are the same PLD : SEC : RTL
        .ret_kind = COSIM_RTL,
        .run = shim_camera_follow,
        .supported = guard_camera_follow,
        .supported_readonly = true,
        // 266..27,816, call-weighted over 94,784 calls on seven movies. The
        // floor is the exit that finds both deltas already zero -- two thirds
        // of all calls -- and the ceiling is a two-player frame that scrolls on
        // both axes at once, which is four routines deep and buys two strips.
        .cycles = 1062,
        // The PHD is 2, the JSL into a scroll routine 3, and that routine's own
        // 9 under it. The X pair are the deep ones; a movie that only ever
        // scrolls on Y measures 12.
        .stack_bytes = 14,
    },
    {
        .name = "actor_aligned",
        .symbol = "$80:B379",
        .entry = 0x80b379,
        .ret_op = 0x80b3f0,  // the no-match RTL; the four direction exits have
                             // one each at $B3BA, $B3BF, $B3DB and $B3E0, and
                             // all five are the same PLD : RTL
        .ret_kind = COSIM_RTL,
        .run = shim_actor_aligned,
        // 758..7,386 over 6,921 calls on level21-bubble, which is the only
        // movie in the corpus that runs this enemy at all. The floor is a
        // match in a high slot and the ceiling is all 32 walked for nothing;
        // the mean sits near the top because nothing is usually lined up.
        .cycles = 4576,
        // The PHD, and nothing else — it calls nothing.
        .stack_bytes = 2,
    },
    {
        .name = "actor_notify_box",
        .symbol = "$80:BF1B",
        .entry = 0x80bf1b,
        .ret_op = 0x80bf66,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_notify_box,
        .supported = guard_actor_notify_box,
        // 642..11,272, call-weighted across 8,556 calls on six movies -- the
        // fallback now and not the price, since `notify_box_cycles` reports
        // every call it can.
        //
        // The floor is worth being exact about, because the model reproduces it
        // and the old description does not. 642 is **not** "a box that found
        // nothing to tell": it is specifically the *one-record refusal* at
        // `$80:BF33`, with all four bounds kept. An empty visible list leaves
        // through `$80:BF2F` two instructions earlier and would cost 606, and
        // that number has never been measured because no call in the corpus has
        // ever found `$9C` at zero -- `notify_no_actors` is untaken on every
        // movie. The ceiling is a box that entered several handlers, so the
        // spread is the handlers' rather than the walk's; the walk is 32
        // records whatever happens.
        .cycles = 5545,
        // Its own PHD and PHY, plus the deepest the dispatch under it goes.
        // Movies that only ever blast one actor measure 18.
        .stack_bytes = 24,
    },
    {
        .name = "actor_snap_to",
        .symbol = "$80:B3F1",
        .entry = 0x80b3f1,
        .ret_op = 0x80b421,
        .ret_kind = COSIM_RTL,
        .run = shim_actor_snap_to,
        // 334..496 over 5,515 calls on level25-lane. The floor is neither axis
        // snapping and the ceiling is both, and there are only four shapes it
        // can have, so the spread is the narrowest in the registry after the
        // boss blitter's.
        .cycles = 407,
        // It calls nothing and pushes nothing.
        .stack_bytes = 0,
    },
    {
        .name = "boss_bg_queue",
        COSIM_COMMIT(COMMIT_BOSS_BG),
        .symbol = "$82:8014",
        .entry = 0x828014,
        .ret_op = 0x828068,
        .ret_kind = COSIM_RTL,
        .run = shim_boss_bg_queue,
        .supported = guard_boss_bg_queue,
        .supported_readonly = true,
        // 11,528..11,686 over 903 calls on level25-lane -- a spread of 158
        // cycles, or 1.4%, and the narrowest of any routine in the registry.
        // Nothing about this routine varies except which of four stored
        // figures it was pointed at, and all four are the same size.
        .cycles = 11565,
        // PHD is 2 and the PEA under it is popped by the PLD, then the closing
        // JSL is 3 with vbl_queue_a_add's own PHY on top.
        .stack_bytes = 7,
    },
    {
        .name = "boss_bg_queue_flip",
        COSIM_COMMIT(COMMIT_BOSS_BG),
        .symbol = "$82:8069",
        .entry = 0x828069,
        .ret_op = 0x8280df,
        .ret_kind = COSIM_RTL,
        .run = shim_boss_bg_queue_flip,
        .supported = guard_boss_bg_queue_flip,
        .supported_readonly = true,
        // 88,980..89,256 over 1,126 calls -- 276 cycles, 0.3%, on a mean seven
        // and a half times larger, because the extra work is the mirror loop
        // and the mirror loop runs a fixed 280 times whatever else happens.
        // This is the largest budget in the registry, by a factor of two, and
        // it is spent entirely on moving 560 bytes of WRAM so that a figure
        // can face the other way.
        .cycles = 89008,
        .stack_bytes = 7,
    },
    {
        .name = "boss_step",
        .symbol = "$82:8F93",
        .entry = 0x828f93,
        // The bare `RTS` on the stuck path. There are two, one under a `SEC`
        // and one under a `CLC`, and the teleport must land on an instruction
        // that does not touch the carry the shim has just published — so it
        // lands on the `RTS` itself rather than on either flag setter.
        .ret_op = 0x829032,
        .ret_kind = COSIM_RTS,
        .run = shim_boss_step,
        // 2,076..15,616, call-weighted over 42,207 calls on the five level-25
        // movies -- the only movies in the corpus that reach it at all. The
        // floor is a direction whose first probe is already in terrain; the
        // ceiling is a diagonal that runs both passes and finds all four probes
        // clear, which is four `terrain_blocked_wide` calls in one step. Almost
        // all of the budget is those calls: the routine's own arithmetic is
        // about sixty instructions and the probes are the rest.
        //
        // Now the fallback and not the price. "Almost all of the budget is
        // those calls" turned out to be 83% of the ceiling, and the model says
        // so by construction: `BOSS_STEP_COST` is the arithmetic and the probes
        // come from `WIDE_COST` through the same `TerrainWideWork` the port
        // fills in. See `boss_step_cycles`.
        .cycles = 11770,
        // Two bytes of `JSR` return address with `terrain_blocked_wide`'s own
        // four on top of it, and the routine pushes nothing itself.
        .stack_bytes = 6,
    },
    {
        .name = "boss_place_parts",
        .symbol = "$82:9265",
        .entry = 0x829265,
        .ret_op = 0x8292c5,  // the only RTS
        .ret_kind = COSIM_RTS,
        .run = shim_boss_place_parts,
        // 1,090..1,142, call-weighted 1,127 over 6,560 calls on the two
        // level-25 movies that raise the figure. **The flattest distribution in
        // this registry**: a 52-cycle spread on a routine that costs eleven
        // hundred, because there is exactly one branch in it and both arms are
        // an `LDY` of a constant. Everything else is 40 straight-line
        // instructions with no call, no loop and no early exit.
        .cycles = 1127,
        .stack_bytes = 0,  // it calls nothing and pushes nothing
    },
    {
        .name = "boss_stomp",
        .symbol = "$82:92D6",
        .entry = 0x8292d6,
        .ret_op = 0x829302,  // the RTS under the JSL
        .ret_kind = COSIM_RTS,
        .run = shim_boss_stomp,
        .supported = guard_boss_stomp,
        // 1,112..13,054, call-weighted 8,230 over 6,549 calls — the fallback
        // now and not the price. Next to `boss_place_parts` above, which runs
        // on the same frames and the same two movies, it is the clearest
        // measurement of what a walk costs: the two routines differ by one
        // `JSL`, and that `JSL` is between 90% and 99% of this one. Which is
        // also why this entry is priced by `actor_notify_box`'s table and owns
        // only the 470 cycles of stores around it — see `BOSS_STOMP_COST`.
        .cycles = 8230,
        .stack_bytes = 21,  // two bytes of `JSR` return address, and
                            // `actor_notify_box`'s dispatch under it
    },
    {
        .name = "blockmap_cell_ptr",
        .symbol = "$80:ACF6",
        .entry = 0x80acf6,
        .ret_op = 0x80ad0a,  // the RTL, after the PLX the flags come from
        .ret_kind = COSIM_RTL,
        .run = shim_blockmap_cell_ptr,
        .supported = guard_blockmap_cell_ptr,
        .supported_readonly = true,
        // 334..374, call-weighted over 1,410 calls on four movies. There is not
        // a branch in the routine, so the 40-cycle spread is the bus and
        // nothing else -- the same shape, and very nearly the same number, as
        // its twin `$80:AD1C tilemap_tile_addr` at 258.
        .cycles = 343,
        .stack_bytes = 2,  // the PHA it reads back through `$01,S`
    },
    {
        .name = "tile_attrs_at_pixel",
        .symbol = "$80:ADC8",
        .entry = 0x80adc8,
        .ret_op = 0x80adf2,  // the RTL, after the second PLB the flags are from
        .ret_kind = COSIM_RTL,
        .run = shim_tile_attrs_at_pixel,
        // 960..1000, and the 40-cycle spread is the bus: there is not a branch
        // in the routine. Very nearly four times `$80:AD1C tilemap_tile_addr`'s
        // 258, which is most of what it does.
        .cycles = 990,
        // PHB, PHD, PHX, PHY are seven and the PEA makes nine, but the PLB
        // takes one back *before* the JSL -- so the deepest point is eight, the
        // three the JSL to $80:AD1C pushes, and that routine's own PHA under
        // them. Thirteen, which is what `verify` measured.
        .stack_bytes = 13,
    },
    {
        .name = "tile_attrs_at_tile",
        .symbol = "$80:ADF3",
        .entry = 0x80adf3,
        .ret_op = 0x80ae13,
        .ret_kind = COSIM_RTL,
        .run = shim_tile_attrs_at_tile,
        // 840..880, call-weighted over the 76 calls the whole corpus makes --
        // 52 on `level9-weapons` and 24 on `level29-ice`, both through
        // `$81:D0D4`, and nothing else in 42 movies reaches it. 128 cycles
        // under the pixel form, which is the six `LSR`s and the two transfers
        // around them almost exactly.
        .cycles = 862,
        .stack_bytes = 13,
    },
    {
        .name = "floor_effect",
        .symbol = "$80:E86D",
        .entry = 0x80e86d,
        .ret_op = 0x80e8d2,  // the RTS every path but $80:E88C reaches
        .ret_kind = COSIM_RTS,
        .run = shim_floor_effect,
        // 1,248..1,792, call-weighted. The floor is 1,248 and everything above
        // it is the two nested calls: `tile_attrs_at_pixel` on every single
        // call, and `terrain_blocked` on the one conveyor direction that asks.
        .cycles = 1322,
        // The routine pushes nothing of its own. The deepest point is the
        // `JSL` to `$80:ADC8` -- three bytes, with that routine's own thirteen
        // under them -- and the `JSR` to the inlined `$80:F935` is only two.
        .stack_bytes = 16,
    },
    {
        .name = "player_state_normal",
        .symbol = "$80:D1FF",
        .entry = 0x80d1ff,
        .ret_op = 0x80d2e5,  // the RTS all eleven paths converge on
        .ret_kind = COSIM_RTS,
        .run = shim_player_state_normal,
        .supported = guard_player_state_normal,
        .supported_readonly = true,
        // 2,104..7,012, call-weighted over 24,419 calls on five movies. The
        // floor is `floor_effect` plus the four countdowns and nothing else --
        // which is most frames -- and the ceiling is a button edge that reaches
        // `apu_play_sfx`, whose cost is how long the SPC700 took to acknowledge
        // the previous sound.
        .cycles = 2434,
        // It pushes nothing of its own: two bytes for the `JSR $E86D`, and
        // `floor_effect`'s own sixteen under that.
        .stack_bytes = 18,
    },
    {
        .name = "sprite_cache_init",
        .symbol = "$80:C05A",
        .entry = 0x80c05a,
        .ret_op = 0x80c07e,  // the RTL, after the two PLBs
        .ret_kind = COSIM_RTL,
        .run = shim_sprite_cache_init,
        // Counted, not measured — there is nothing here for `verify` to have
        // measured. The derivation is above the shim.
        .cycles = 346734,
        // `PHB` then `PEA $007E`: one byte and two, and the exit pulls all
        // three. Nothing else in the body touches the stack.
        .stack_bytes = 3,
        .run_only = true,
    },
    {
        .name = "blockmap_expand",
        .symbol = "$80:AD2B",
        .entry = 0x80ad2b,
        .ret_op = 0x80ad91,  // the RTL, after the PLD that restores the caller
        .ret_kind = COSIM_RTL,
        .run = shim_blockmap_expand,
        .supported = guard_blockmap_expand,
        .supported_readonly = true,
        // Never used: the shim reports the call's exact cost, and every call
        // reports one. It is the level-1 figure, so a reader who wants a sense
        // of the scale has one and nothing silently falls back to it.
        .cycles = 3604294,
        // `PHD` then `PEA $0000`, four bytes; the `PLD` two instructions later
        // takes the `PEA`'s back and the one at `$AD90` takes the `PHD`'s.
        .stack_bytes = 4,
        .run_only = true,
    },
    // The frame's own machinery, and none of it returns. See `port/sched.h`
    // and `CosimRoutine::exits`. `.cycles` is never used: every call prices
    // itself. It is the mean `verify` measured over the corpus, for scale.
    {
        .name = "thread_yield",
        .symbol = "$80:8353",
        .entry = 0x808353,
        .run = shim_thread_yield,
        .accepts = accepts_thread_yield,
        COSIM_EXITS(SCHED_EXITS),
        .cycles = 700,
    },
    {
        .name = "thread_exit",
        .symbol = "$00:833E",
        .entry = 0x00833e,
        .run = shim_thread_exit,
        .accepts = accepts_thread_exit,
        COSIM_EXITS(SCHED_EXITS_00),
        .uncalled = true,
        .cycles = 700,
    },
    {
        .name = "sched_wake",
        .symbol = "$80:8372",
        .entry = 0x808372,
        .run = shim_sched_wake,
        .accepts = accepts_sched_wake,
        COSIM_EXITS(SCHED_WAKE_EXITS),
        .uncalled = true,
        .cycles = 98,
    },
    {
        .name = "sched_rescan",
        .symbol = "$80:8380",
        .entry = 0x808380,
        .run = shim_sched_rescan,
        .accepts = accepts_sched_rescan,
        COSIM_EXITS(SCHED_EXITS),
        .cycles = 2000,
    },
    {
        .name = "sched_wake_00",
        .symbol = "$00:8372",
        .entry = 0x008372,
        .run = shim_sched_wake,
        .accepts = accepts_sched_wake,
        COSIM_EXITS(SCHED_WAKE_EXITS_00),
        .uncalled = true,
        .cycles = 98,
    },
    {
        .name = "sched_rescan_00",
        .symbol = "$00:8380",
        .entry = 0x008380,
        .run = shim_sched_rescan,
        .accepts = accepts_sched_rescan,
        COSIM_EXITS(SCHED_EXITS_00),
        .cycles = 2000,
    },
    {
        .name = "vbl_queue_a_run",
        .symbol = "$80:83E0",
        .entry = 0x8083e0,
        .run = shim_vbl_queue_a_run,
        .accepts = accepts_vbl_run,
        COSIM_EXITS(VBL_A_EXITS),
        .cycles = 500,
    },
    {
        .name = "vbl_queue_a_resume",
        .symbol = "$80:8401",
        .entry = 0x808401,
        .run = shim_vbl_queue_a_resume,
        .accepts = accepts_vbl_run,
        COSIM_EXITS(VBL_A_EXITS),
        .uncalled = true,
        .cycles = 500,
    },
    {
        .name = "vbl_queue_b_run",
        .symbol = "$80:843D",
        .entry = 0x80843d,
        .run = shim_vbl_queue_b_run,
        .accepts = accepts_vbl_run,
        COSIM_EXITS(VBL_B_EXITS),
        .cycles = 500,
    },
    {
        .name = "vbl_queue_b_resume",
        .symbol = "$80:845E",
        .entry = 0x80845e,
        .run = shim_vbl_queue_b_resume,
        .accepts = accepts_vbl_run,
        COSIM_EXITS(VBL_B_EXITS),
        .uncalled = true,
        .cycles = 500,
    },
    // Entered by `JML [$0000]` from the trampoline in bank $00, and then by
    // the instructions after each hardware access.
    {
        .name = "nmi_enter",
        .symbol = "$80:8179",
        .entry = 0x808179,
        .run = shim_nmi_enter,
        .accepts = accepts_nmi_enter,
        COSIM_EXITS(NMI_ENTER_EXITS),
        .uncalled = true,
        .cycles = 356,
    },
    {
        .name = "nmi_stack",
        .symbol = "$80:8199",
        .entry = 0x808199,
        .run = shim_nmi_stack,
        .accepts = accepts_nmi,
        COSIM_EXITS(NMI_STACK_EXITS),
        .uncalled = true,
        .cycles = 88,
    },
    {
        .name = "nmi_input",
        .symbol = "$80:81BB",
        .entry = 0x8081bb,
        .run = shim_nmi_input,
        .accepts = accepts_nmi,
        COSIM_EXITS(NMI_INPUT_EXITS),
        .uncalled = true,
        .cycles = 394,
    },
    {
        .name = "nmi_leave",
        .symbol = "$80:81E4",
        .entry = 0x8081e4,
        .run = shim_nmi_leave,
        .accepts = accepts_nmi_leave,
        COSIM_EXITS(NMI_LEAVE_EXITS),
        .uncalled = true,
        .cycles = 366,
    },
    // Where `JSR init_ppu_regs` returns, to just before the NMI is turned on.
    // Nothing interrupts it, and `verify` checks it like any other call. The
    // cost is a cold start's, about seventeen frames of `MVN`.
    {
        .name = "reset_clear",
        .symbol = "$80:80C1",
        .entry = 0x8080c1,
        .run = shim_reset_clear,
        .accepts = accepts_reset_clear,
        COSIM_EXITS(RESET_EXITS),
        .uncalled = true,
        .cycles = 6031452,
    },
    // Thread bodies, between one yield or call and the next. See
    // `port/bodies.h`. `.cycles` is never used; every call prices itself.
    // The entries are written out rather than named, because
    // `tools/native_share.py` reads them from here.
    {
        .name = "victims_start",
        .symbol = "$81:81F6",
        .entry = 0x8181f6,
        .run = shim_victims_start,
        .accepts = accepts_body,
        COSIM_EXITS(VICTIMS_YIELD_EXITS),
        .uncalled = true,
        .cycles = 172,
    },
    {
        .name = "victims_resume",
        .symbol = "$81:8206",
        .entry = 0x818206,
        .run = shim_victims_resume,
        .accepts = accepts_body_list,
        COSIM_EXITS(VICTIMS_RESUME_EXITS),
        .uncalled = true,
        .cycles = 5000,
    },
    {
        .name = "victims_started",
        .symbol = "$81:8263",
        .entry = 0x818263,
        .run = shim_victims_started,
        .accepts = accepts_body,
        COSIM_EXITS(VICTIMS_YIELD_EXITS),
        .uncalled = true,
        .cycles = 86,
    },
    {
        .name = "victims_stopped",
        .symbol = "$81:828F",
        .entry = 0x81828f,
        .run = shim_victims_stopped,
        .accepts = accepts_body,
        COSIM_EXITS(VICTIMS_YIELD_EXITS),
        .uncalled = true,
        .cycles = 86,
    },
    {
        .name = "object_resume",
        .symbol = "$80:C911",
        .entry = 0x80c911,
        .run = shim_object_resume,
        .accepts = accepts_body_low,
        COSIM_EXITS(OBJECT_RESUME_EXITS),
        .uncalled = true,
        .cycles = 1000,
    },
    {
        .name = "object_polled",
        .symbol = "$80:C918",
        .entry = 0x80c918,
        .run = shim_object_polled,
        .accepts = accepts_body_low,
        COSIM_EXITS(OBJECT_POLLED_EXITS),
        .uncalled = true,
        .cycles = 1000,
    },
    {
        .name = "object_given",
        .symbol = "$80:C967",
        .entry = 0x80c967,
        .run = shim_object_acted,
        .accepts = accepts_body,
        COSIM_EXITS(OBJECT_YIELD_EXITS),
        .uncalled = true,
        .cycles = 154,
    },
    {
        .name = "object_freed",
        .symbol = "$80:C971",
        .entry = 0x80c971,
        .run = shim_object_acted,
        .accepts = accepts_body,
        COSIM_EXITS(OBJECT_YIELD_EXITS),
        .uncalled = true,
        .cycles = 154,
    },
    {
        .name = "actors_checked",
        .symbol = "$81:8113",
        .entry = 0x818113,
        .run = shim_actors_checked,
        .accepts = accepts_body_list,
        COSIM_EXITS(ACTORS_CHECKED_EXITS),
        .uncalled = true,
        .cycles = 300,
    },
    {
        .name = "actors_measured",
        .symbol = "$81:814B",
        .entry = 0x81814b,
        .run = shim_actors_measured,
        .accepts = accepts_body,
        COSIM_EXITS(ACTORS_YIELD_EXITS),
        .uncalled = true,
        .cycles = 200,
    },
    {
        .name = "actors_started",
        .symbol = "$81:817C",
        .entry = 0x81817c,
        .run = shim_actors_started,
        .accepts = accepts_body,
        COSIM_EXITS(ACTORS_YIELD_EXITS),
        .uncalled = true,
        .cycles = 110,
    },
    {
        .name = "tile_anim_resume",
        .symbol = "$82:D881",
        .entry = 0x82d881,
        .run = shim_tile_anim_resume,
        .accepts = accepts_body_low,
        COSIM_EXITS(TILE_ANIM_RESUME_EXITS),
        .uncalled = true,
        .cycles = 900,
    },
    {
        .name = "tile_anim_queued",
        .symbol = "$82:D87A",
        .entry = 0x82d87a,
        .run = shim_tile_anim_queued,
        .accepts = accepts_body,
        COSIM_EXITS(TILE_ANIM_YIELD_EXITS),
        .uncalled = true,
        .cycles = 18,
    },

    // R27: one native zombie tick, from the return after thread_yield through
    // seek, dynamic behavior, animation and back to the next yield.  It is an
    // uncalled thread-body entry, not a JSR/JSL routine.
    {
        .name = "zombie_frame_native",
        .symbol = "$81:8834",
        .entry = R27_ZOMBIE_FRAME_PC,
        .run = shim_zombie_frame_native,
        .accepts = accepts_zombie_tables,
        COSIM_EXITS(R27_ZOMBIE_FRAME_EXITS),
        .uncalled = true,
        .cycles = 20000,
    },

    // R29: random/wander/turn states sharing the $81:8A5C movement path.
    {
        .name = "enemy_8a5c_native", .symbol = "$81:8A5C",
        .entry = 0x818a5c, .run = shim_enemy_8a5c_native,
        .accepts = accepts_enemy_8a5c, COSIM_EXITS(R29_ENEMY_EXITS),
        .cycles = 5000,
    },
    {
        .name = "enemy_89bf_native", .symbol = "$81:89BF",
        .entry = 0x8189bf, .run = shim_enemy_8a5c_native,
        .accepts = accepts_enemy_8a5c, COSIM_EXITS(R29_ENEMY_EXITS),
        .cycles = 5000,
    },
    {
        .name = "enemy_8a72_native", .symbol = "$81:8A72",
        .entry = 0x818a72, .run = shim_enemy_8a5c_native,
        .accepts = accepts_enemy_8a5c, COSIM_EXITS(R29_ENEMY_EXITS),
        .cycles = 9000,
    },

    // R26: the two hottest Level-1 zombie paths are fused through their already-
    // verified C helpers, so movement and target selection no longer return to
    // the 65816 core around every JSL. Mid-range R25 entries remain below as
    // safe fallbacks for any control path that reaches them independently.
    {
        .name = "zombie_move_native",
        .symbol = "$81:85EF",
        .entry = ZOMBIE_MOVE_SETUP_PC,
        .run = shim_zombie_move_native,
        .accepts = accepts_zombie_tables,
        COSIM_EXITS(ZOMBIE_MOVE_NATIVE_EXITS),
        .cycles = 5000,
    },
    {
        .name = "zombie_move_after_terrain",
        .symbol = "$81:861C",
        .entry = ZOMBIE_MOVE_AFTER_TERRAIN_PC,
        .run = shim_zombie_move_after_terrain,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_AFTER_TERRAIN_EXITS),
        .cycles = 70,
    },
    {
        .name = "zombie_move_after_actor",
        .symbol = "$81:862B",
        .entry = ZOMBIE_MOVE_AFTER_ACTOR_PC,
        .run = shim_zombie_move_after_actor,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_AFTER_ACTOR_EXITS),
        .cycles = 168,
    },
    {
        .name = "zombie_move_rotate",
        .symbol = "$81:863E",
        .entry = ZOMBIE_MOVE_ROTATE_PC,
        .run = shim_zombie_move_rotate,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_ROTATE_EXITS),
        .cycles = 226,
    },
    {
        .name = "zombie_seek_native",
        .symbol = "$81:8706",
        .entry = ZOMBIE_SEEK_PC,
        .run = shim_zombie_seek_native,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_SEEK_NATIVE_EXITS),
        .cycles = 5000,
    },
    {
        .name = "zombie_after_nearest",
        .symbol = "$81:870E",
        .entry = ZOMBIE_AFTER_NEAREST_PC,
        .run = shim_zombie_after_nearest,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_AFTER_NEAREST_EXITS),
        .cycles = 58,
    },
    {
        .name = "zombie_bearing_prep",
        .symbol = "$81:8716",
        .entry = ZOMBIE_BEARING_PREP_PC,
        .run = shim_zombie_bearing_prep,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_BEARING_PREP_EXITS),
        .cycles = 72,
    },
    {
        .name = "zombie_after_bearing",
        .symbol = "$81:8721",
        .entry = ZOMBIE_AFTER_BEARING_PC,
        .run = shim_zombie_after_bearing,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_AFTER_BEARING_EXITS),
        .cycles = 62,
    },
    {
        .name = "zombie_anim",
        .symbol = "$81:8736",
        .entry = ZOMBIE_ANIM_PC,
        .run = shim_zombie_anim,
        .accepts = accepts_zombie_tables,
        COSIM_EXITS(ZOMBIE_ANIM_EXITS),
        .cycles = 330,
    },
    {
        .name = "zombie_handler_call",
        .symbol = "$81:8837",
        .entry = ZOMBIE_HANDLER_CALL_PC,
        .run = shim_zombie_handler_call,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_HANDLER_EXITS),
        .cycles = 92,
    },
    {
        .name = "zombie_post_anim",
        .symbol = "$81:8842",
        .entry = ZOMBIE_POST_ANIM_PC,
        .run = shim_zombie_post_anim,
        .accepts = accepts_body,
        COSIM_EXITS(ZOMBIE_POST_ANIM_EXITS),
        .cycles = 126,
    },

    // The player's frame and the movement handler, the same way. The four the
    // body calls are called, so they serve a call; the rest are reached by a
    // return or by the body's `RTS` into a handler.
    {
        .name = "player_ticks",
        .symbol = "$80:CDF7",
        .entry = 0x80cdf7,
        .run = shim_player_ticks,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_TICKS_EXITS),
        .uncalled = true,
        .cycles = 18,
    },
    {
        .name = "player_state",
        .symbol = "$80:CE04",
        .entry = 0x80ce04,
        .run = shim_player_state,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_STATE_EXITS),
        .uncalled = true,
        .cycles = 102,
    },
    {
        .name = "player_move",
        .symbol = "$80:CE0C",
        .entry = 0x80ce0c,
        .run = shim_player_move,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_MOVE_EXITS),
        .uncalled = true,
        .cycles = 114,
    },
    {
        .name = "player_buttons",
        .symbol = "$80:CE19",
        .entry = 0x80ce19,
        .run = shim_player_buttons,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_BUTTONS_EXITS),
        .uncalled = true,
        .cycles = 56,
    },
    {
        .name = "player_loop",
        .symbol = "$80:CE23",
        .entry = 0x80ce23,
        .run = shim_player_loop,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_LOOP_EXITS),
        .uncalled = true,
        .cycles = 36,
    },
    {
        .name = "player_branch",
        .symbol = "$80:D1EA",
        .entry = 0x80d1ea,
        .run = shim_player_branch,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_BRANCH_EXITS),
        .cycles = 28,
    },
    {
        .name = "player_hurt",
        .symbol = "$80:D01B",
        .entry = 0x80d01b,
        .run = shim_player_hurt,
        .accepts = accepts_body,
        COSIM_EXITS(PLAYER_HURT_EXITS),
        .cycles = 148,
    },
    {
        .name = "player_won",
        .symbol = "$80:CE25",
        .entry = 0x80ce25,
        .run = shim_player_won,
        .accepts = accepts_body_low,
        COSIM_EXITS(PLAYER_WON_EXITS),
        .cycles = 52,
    },
    {
        .name = "player_dead",
        .symbol = "$80:CE72",
        .entry = 0x80ce72,
        .run = shim_player_dead,
        .accepts = accepts_player_dead,
        COSIM_EXITS(PLAYER_DEAD_EXITS),
        .cycles = 80,
    },
    // The first routine in readable C. See `port/walk.h`. Entered by the
    // frame's `RTS` with the frame's return address already pushed, so it is
    // a routine like any other to the harness. It declines the double step
    // and the tiles with a reaction of their own, and the ROM walks those.
    {
        .name = "player_walk",
        .symbol = "$80:E4BA",
        .entry = PLAYER_WALK_PC,
        .ret_op = PLAYER_WALK_RTS_PC,
        .ret_kind = COSIM_RTS,
        .run = shim_player_walk,
        .accepts = accepts_player_walk,
#if defined(XBOX_PORT) && defined(ZAMN_R38_READONLY_WALK_GUARD)
        .supported = supported_player_walk_readonly,
        .supported_readonly = true,
#else
        .supported = supported_player_walk,
#endif
        .uncalled = true,
        // Never charged: the shim prices every call it serves.
        .cycles = 6000,
        // The deepest the ROM goes: a `JSL`, then `terrain_blocked`'s `PHD`
        // and `PHA`, or `actor_obstacle_at_point`'s `PHD` and `PEA`.
        .stack_bytes = 7,
    },
    // The second, and the same arrangement: see `port/chase.h`. Entered by
    // the monster thread's computed `RTS`. Leap setup resumes in ROM at BCF1.
    {
        .name = "monster_chase",
        .symbol = "$81:BEE3",
        .entry = MONSTER_CHASE_PC,
        .ret_op = MONSTER_CHASE_RTS_PC,
        .ret_kind = COSIM_RTS,
        .run = shim_monster_chase,
        .accepts = accepts_monster_chase,
        COSIM_EXITS(R29_CHASE_EXITS),
        .uncalled = true,
        // Never charged: the shim prices every call it serves.
        .cycles = 12000,
        // The `JSR` to the leap test, its `JSL`, and `tile_attrs_at_pixel`'s
        // own thirteen.
        .stack_bytes = 18,
    },
    // Vblank jobs, which write the PPU. See `port/vblank.h`. Each prices
    // itself through `cosim_hw`, so `.cycles` is only what a refused trace
    // would fall back to. The NMI calls the first; the dispatcher reaches the
    // other three by `RTL`.
    {
        .name = "vram_queue_flush",
        .symbol = "$80:9E7B",
        .entry = 0x809e7b,
        .ret_op = 0x809ecd,  // the busy path has its own; either returns alike
        .ret_kind = COSIM_RTL,
        .run = shim_vram_queue_flush,
        .accepts = accepts_vram_queue_flush,
        .hw = true,
        .cycles = 1000,
    },
    {
        .name = "sprite_upload_flush",
        .symbol = "$80:B947",
        .entry = 0x80b947,
        .ret_op = 0x80b9c6,
        .ret_kind = COSIM_RTL,
        .run = shim_sprite_upload_flush,
        .accepts = accepts_sprite_upload_flush,
        .hw = true,
        .uncalled = true,
        .cycles = 1000,
    },
    {
        .name = "bg2_scroll_job",
        .symbol = "$80:9E3E",
        .entry = 0x809e3e,
        .ret_op = 0x809e6c,
        .ret_kind = COSIM_RTL,
        .run = shim_bg2_scroll_job,
        .accepts = accepts_vbl_job,
        .hw = true,
        .uncalled = true,
        .cycles = 486,
    },
    {
        .name = "camera_scroll_job",
        .symbol = "$82:8209",
        .entry = 0x828209,
        .ret_op = 0x828244,  // the parked path has its own; either returns alike
        .ret_kind = COSIM_RTL,
        .run = shim_camera_scroll_job,
        .accepts = accepts_vbl_job,
        .hw = true,
        .uncalled = true,
        .cycles = 514,
    },
    {
        .name = "scroll_shadow_job",
        .symbol = "$80:9BFC",
        .entry = 0x809bfc,
        .ret_op = 0x809c49,
        .ret_kind = COSIM_RTL,
        .run = shim_scroll_shadow_job,
        .accepts = accepts_vbl_job,
        .hw = true,
        .uncalled = true,
        .cycles = 690,
    },
    {
        .name = "boss_bg_dma",
        .symbol = "$82:81C9",
        .entry = 0x8281c9,
        .ret_op = 0x828208,
        .ret_kind = COSIM_RTL,
        .run = shim_boss_bg_dma,
        .accepts = accepts_boss_bg_dma,
        .hw = true,
        .uncalled = true,
        .cycles = 2000,
    },
    {
        .name = "player_idle_resume", .symbol = "$80:D53D", .entry = 0x80d53du,
        .run = shim_player_idle_resume, .accepts = accepts_body,
        COSIM_EXITS(player_idle_resume_exits), .uncalled = true, .cycles = 222,
    },
    {
        .name = "player_walk_resume", .symbol = "$80:D6A8", .entry = 0x80d6a8u,
        .run = shim_player_walk_resume, .accepts = accepts_body,
        COSIM_EXITS(player_walk_resume_exits), .uncalled = true, .cycles = 108,
    },
    {
        .name = "player_walk_fire_resume", .symbol = "$80:D6B8", .entry = 0x80d6b8u,
        .run = shim_player_walk_fire_resume, .accepts = accepts_body,
        COSIM_EXITS(player_walk_fire_resume_exits), .uncalled = true, .cycles = 228,
    },
    {
        .name = "player_walk_after_fire", .symbol = "$80:D6CB", .entry = 0x80d6cbu,
        .run = shim_player_walk_after_fire, .accepts = accepts_body,
        COSIM_EXITS(player_walk_after_fire_exits), .uncalled = true, .cycles = 64,
    },
    {
        .name = "player_walk_animation", .symbol = "$80:D72A", .entry = 0x80d72au,
        .run = shim_player_walk_animation, .accepts = accepts_body,
        COSIM_EXITS(player_walk_animation_exits), .uncalled = true, .cycles = 312,
    },

#if defined(XBOX_PORT) && defined(ZAMN_R50_NATIVE_HOTPATHS)
    {
        .name="actor_overlap_resume", .symbol="$80:BF12", .entry=0x80bf12,
        .run=shim_overlap_scan, .accepts=accepts_overlap_scan,
        COSIM_EXITS(overlap_scan_exits), .uncalled=true, .cycles=100,
    },
    {
        .name="sprite_emit_native", .symbol="$80:BA51", .entry=0x80ba51,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_native_exits), .uncalled=true, .cycles=500,
    },
    {
        .name="sprite_emit_resume", .symbol="$80:BA9B", .entry=0x80ba9b,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_native_exits), .uncalled=true, .cycles=200,
    },
#if defined(ZAMN_R51_NATIVE_HOTPATHS)
    {
        .name="sprite_emit_flipx", .symbol="$80:BABA", .entry=0x80baba,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_flipx_exits), .uncalled=true, .cycles=550,
    },
    {
        .name="sprite_emit_flipx_resume", .symbol="$80:BB0B", .entry=0x80bb0b,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_flipx_exits), .uncalled=true, .cycles=220,
    },
    {
        .name="sprite_emit_flipy", .symbol="$80:BB30", .entry=0x80bb30,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_flipy_exits), .uncalled=true, .cycles=550,
    },
    {
        .name="sprite_emit_flipy_resume", .symbol="$80:BB81", .entry=0x80bb81,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_flipy_exits), .uncalled=true, .cycles=220,
    },
    {
        .name="sprite_emit_flipxy", .symbol="$80:BBA6", .entry=0x80bba6,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_flipxy_exits), .uncalled=true, .cycles=600,
    },
    {
        .name="sprite_emit_flipxy_resume", .symbol="$80:BBFE", .entry=0x80bbfe,
        .run=shim_sprite_emit_native, .supported=guard_sprite_emit_native,
        .supported_readonly=true,
        COSIM_EXITS(sprite_emit_flipxy_exits), .uncalled=true, .cycles=240,
    },
#if defined(ZAMN_RELEASE_NO_DIAGNOSTICS) || defined(ZAMN_R51_PROFILE_FULL_WALKER) || defined(ZAMN_R70_DIAGNOSTIC_OAM_PARITY)
    {
        .name="sprite_oam_walk", .symbol="$80:BD30", .entry=0x80bd30,
        .run=shim_oam_walk_native, .supported=guard_oam_walk_native,
        .supported_readonly=true,
        COSIM_EXITS(oam_walk_native_exits), .uncalled=true, .cycles=400,
    },
    {
        .name="sprite_oam_walk_resume", .symbol="$80:BDB7", .entry=0x80bdb7,
        .run=shim_oam_walk_native, .supported=guard_oam_walk_native,
        .supported_readonly=true,
        COSIM_EXITS(oam_walk_native_exits), .uncalled=true, .cycles=220,
    },
    {
        .name="sprite_oam_tail", .symbol="$80:BDD0", .entry=0x80bdd0,
        .run=shim_oam_walk_native, .supported=guard_oam_walk_native,
        .supported_readonly=true,
        COSIM_EXITS(oam_tail_native_exits), .uncalled=true, .cycles=120,
    },
#endif
#endif
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R53_NATIVE_MOVEMENT)
    { .name="r53_move_9715", .symbol="$82:9715", .entry=0x829715,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9715),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_973e", .symbol="$82:973E", .entry=0x82973e,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_973e),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_974a", .symbol="$82:974A", .entry=0x82974a,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_974a),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9751", .symbol="$82:9751", .entry=0x829751,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9751),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9759", .symbol="$82:9759", .entry=0x829759,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9759),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9765", .symbol="$82:9765", .entry=0x829765,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9765),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9773", .symbol="$82:9773", .entry=0x829773,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9773),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_977f", .symbol="$82:977F", .entry=0x82977f,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_977f),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_97be", .symbol="$82:97BE", .entry=0x8297be,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_97be),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_97d4", .symbol="$82:97D4", .entry=0x8297d4,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_97d4),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_97e0", .symbol="$82:97E0", .entry=0x8297e0,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_97e0),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_97e9", .symbol="$82:97E9", .entry=0x8297e9,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_97e9),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9800", .symbol="$82:9800", .entry=0x829800,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9800),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_980e", .symbol="$82:980E", .entry=0x82980e,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_980e),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_981a", .symbol="$82:981A", .entry=0x82981a,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_981a),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9831", .symbol="$82:9831", .entry=0x829831,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9831),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_983f", .symbol="$82:983F", .entry=0x82983f,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_983f),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_985b", .symbol="$82:985B", .entry=0x82985b,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_985b),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9863", .symbol="$82:9863", .entry=0x829863,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9863),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_986b", .symbol="$82:986B", .entry=0x82986b,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_986b),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_98ea", .symbol="$82:98EA", .entry=0x8298ea,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_98ea),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_98fe", .symbol="$82:98FE", .entry=0x8298fe,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_98fe),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_9994", .symbol="$82:9994", .entry=0x829994,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_9994),
      .uncalled=true, .cycles=120 },
    { .name="r53_move_99cb", .symbol="$82:99CB", .entry=0x8299cb,
      .run=shim_r53_movement, .supported=guard_r53_movement,
      .supported_readonly=true, COSIM_EXITS(r53_exit_99cb),
      .uncalled=true, .cycles=120 },
#endif


#if defined(XBOX_PORT) && defined(ZAMN_R54_NATIVE_D9_CLUSTER)
    { .name="r54_actor_d8db", .symbol="$82:D8D9", .entry=0x82d8dbu,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d8db),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d8fd", .symbol="$82:D8FB", .entry=0x82d8fdu,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d8fd),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d907", .symbol="$82:D905", .entry=0x82d907u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d907),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d913", .symbol="$82:D911", .entry=0x82d913u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d913),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d91d", .symbol="$82:D91B", .entry=0x82d91du,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d91d),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d92a", .symbol="$82:D928", .entry=0x82d92au,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d92a),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d938", .symbol="$82:D936", .entry=0x82d938u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d938),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d945", .symbol="$82:D943", .entry=0x82d945u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d945),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d94d", .symbol="$82:D94B", .entry=0x82d94du,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d94d),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d96e", .symbol="$82:D970", .entry=0x82d96eu,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d96e),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d989", .symbol="$82:D98B", .entry=0x82d989u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d989),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d999", .symbol="$82:D99B", .entry=0x82d999u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d999),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d9ae", .symbol="$82:D9B0", .entry=0x82d9aeu,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d9ae),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d9b4", .symbol="$82:D9B4", .entry=0x82d9b4u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d9b4),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d9c3", .symbol="$82:D9C3", .entry=0x82d9c3u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d9c3),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d9d5", .symbol="$82:D9D5", .entry=0x82d9d5u,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d9d5),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d9dc", .symbol="$82:D9DC", .entry=0x82d9dcu,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d9dc),
      .uncalled=true, .cycles=120 },
    { .name="r54_actor_d9eb", .symbol="$82:D9EB", .entry=0x82d9ebu,
      .run=shim_r54_cluster, .supported=guard_r54_cluster,
      .supported_readonly=true, COSIM_EXITS(r54_exit_d9eb),
      .uncalled=true, .cycles=120 },
#endif


#if defined(XBOX_PORT) && defined(ZAMN_R55_NATIVE_ACTOR_FRAGMENTS)
    { .name="r55_actor_839843", .symbol="$83:9843", .entry=0x839843,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_839843),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_83984f", .symbol="$83:984F", .entry=0x83984f,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_83984f),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_83985d", .symbol="$83:985D", .entry=0x83985d,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_83985d),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_839865", .symbol="$83:9865", .entry=0x839865,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_839865),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_839880", .symbol="$83:9880", .entry=0x839880,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_839880),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_83989b", .symbol="$83:989B", .entry=0x83989b,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_83989b),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_8398aa", .symbol="$83:98AA", .entry=0x8398aa,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_8398aa),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_8398d2", .symbol="$83:98D2", .entry=0x8398d2,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_8398d2),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_8398f1", .symbol="$83:98F1", .entry=0x8398f1,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_8398f1),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_82988d", .symbol="$82:988D", .entry=0x82988d,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_82988d),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_829890", .symbol="$82:9890", .entry=0x829890,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_829890),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_8298b0", .symbol="$82:98B0", .entry=0x8298b0,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_8298b0),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_8298ce", .symbol="$82:98CE", .entry=0x8298ce,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_8298ce),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_82991a", .symbol="$82:991A", .entry=0x82991a,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_82991a),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_829927", .symbol="$82:9927", .entry=0x829927,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_829927),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_82993b", .symbol="$82:993B", .entry=0x82993b,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_82993b),
      .uncalled=true, .cycles=120 },
    { .name="r55_actor_82993f", .symbol="$82:993F", .entry=0x82993f,
      .run=shim_r55_fragments, .supported=guard_r55_fragments,
      .supported_readonly=true, COSIM_EXITS(r55_exit_82993f),
      .uncalled=true, .cycles=120 },
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R59_NATIVE_COLLISION_FRAGMENTS)
    { .name="r59_native_be9e", .symbol="$80:BE9E", .entry=0x80be9eu,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_be9e),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_bea0", .symbol="$80:BEA0", .entry=0x80bea0u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_bea0),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_bea8", .symbol="$80:BEA8", .entry=0x80bea8u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_bea8),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_beb8", .symbol="$80:BEB8", .entry=0x80beb8u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_beb8),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_8483", .symbol="$80:8483", .entry=0x808483u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_8483),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_8486", .symbol="$80:8486", .entry=0x808486u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_8486),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_f25d", .symbol="$81:F25D", .entry=0x81f25du,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_f25d),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_f276", .symbol="$81:F276", .entry=0x81f276u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_f276),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_f27a", .symbol="$81:F27A", .entry=0x81f27au,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_f27a),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_9845", .symbol="$82:9845", .entry=0x829845u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_9845),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_984d", .symbol="$82:984D", .entry=0x82984du,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_984d),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_984f", .symbol="$82:984F", .entry=0x82984fu,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_984f),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_9852", .symbol="$82:9852", .entry=0x829852u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_9852),
      .uncalled=true, .cycles=120 },
    { .name="r59_native_9854", .symbol="$82:9854", .entry=0x829854u,
      .run=shim_r59_fragments, .supported=guard_r59_fragments,
      .supported_readonly=true, COSIM_EXITS(r59_exit_9854),
      .uncalled=true, .cycles=120 },
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R60_CONNECTED_NATIVE)
    { .name="r60_connected_be91", .symbol="$80:BE91", .entry=0x80be91u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_be91),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_be93", .symbol="$80:BE93", .entry=0x80be93u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_be93),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_be95", .symbol="$80:BE95", .entry=0x80be95u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_be95),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_be97", .symbol="$80:BE97", .entry=0x80be97u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_be97),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_be99", .symbol="$80:BE99", .entry=0x80be99u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_be99),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_be9b", .symbol="$80:BE9B", .entry=0x80be9bu,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_be9b),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_f1cd", .symbol="$81:F1CD", .entry=0x81f1cdu,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_f1cd),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_f1d0", .symbol="$81:F1D0", .entry=0x81f1d0u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_f1d0),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_993d", .symbol="$82:993D", .entry=0x82993du,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_993d),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_9942", .symbol="$82:9942", .entry=0x829942u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_9942),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_9944", .symbol="$82:9944", .entry=0x829944u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_9944),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_994a", .symbol="$82:994A", .entry=0x82994au,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_994a),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_994d", .symbol="$82:994D", .entry=0x82994du,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_994d),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_994f", .symbol="$82:9950", .entry=0x82994fu,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_994f),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_9952", .symbol="$82:9953", .entry=0x829952u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_9952),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_9955", .symbol="$82:9955", .entry=0x829955u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_9955),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_9958", .symbol="$82:9958", .entry=0x829958u,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_9958),
      .uncalled=true, .cycles=120 },
    { .name="r60_connected_995b", .symbol="$82:995B", .entry=0x82995bu,
      .run=shim_r60, .supported=guard_r60,
      .supported_readonly=true, COSIM_EXITS(r60_exit_995b),
      .uncalled=true, .cycles=120 },
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R61_THREAD_ACTOR_NATIVE)
    { .name="r61_native_808488", .symbol="$80:8488", .entry=0x808488u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_808488),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_808489", .symbol="$80:8489", .entry=0x808489u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_808489),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_80848a", .symbol="$80:848A", .entry=0x80848au,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_80848a),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084a3", .symbol="$80:84A3", .entry=0x8084a3u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084a3),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084a5", .symbol="$80:84A5", .entry=0x8084a5u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084a5),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084a6", .symbol="$80:84A6", .entry=0x8084a6u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084a6),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084a8", .symbol="$80:84A8", .entry=0x8084a8u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084a8),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084ab", .symbol="$80:84AB", .entry=0x8084abu,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084ab),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084ae", .symbol="$80:84AE", .entry=0x8084aeu,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084ae),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_8084af", .symbol="$80:84AF", .entry=0x8084afu,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_8084af),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1d3", .symbol="$81:F1D3", .entry=0x81f1d3u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1d3),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1d5", .symbol="$81:F1D5", .entry=0x81f1d5u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1d5),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1d7", .symbol="$81:F1D7", .entry=0x81f1d7u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1d7),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1d9", .symbol="$81:F1D9", .entry=0x81f1d9u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1d9),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1db", .symbol="$81:F1DB", .entry=0x81f1dbu,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1db),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1de", .symbol="$81:F1DE", .entry=0x81f1deu,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1de),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_81f1e1", .symbol="$81:F1E1", .entry=0x81f1e1u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_81f1e1),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_829887", .symbol="$82:9887", .entry=0x829887u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_829887),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_829930", .symbol="$82:9930", .entry=0x829930u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_829930),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_829933", .symbol="$82:9933", .entry=0x829933u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_829933),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_829935", .symbol="$82:9935", .entry=0x829935u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_829935),
      .uncalled=true, .cycles=120 },
    { .name="r61_native_829936", .symbol="$82:9936", .entry=0x829936u,
      .run=shim_r61, .supported=guard_r61,
      .supported_readonly=true, COSIM_EXITS(r61_exit_829936),
      .uncalled=true, .cycles=120 },
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R62_ACTOR_CONTROL_NATIVE)
    { .name="r62_native_81f175", .symbol="$81:F175", .entry=0x81f175u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f175),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f178", .symbol="$81:F178", .entry=0x81f178u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f178),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f17a", .symbol="$81:F17A", .entry=0x81f17au,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f17a),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f17d", .symbol="$81:F17D", .entry=0x81f17du,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f17d),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f17f", .symbol="$81:F17F", .entry=0x81f17fu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f17f),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f182", .symbol="$81:F182", .entry=0x81f182u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f182),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f184", .symbol="$81:F184", .entry=0x81f184u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f184),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f186", .symbol="$81:F186", .entry=0x81f186u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f186),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f188", .symbol="$81:F188", .entry=0x81f188u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f188),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f18a", .symbol="$81:F18A", .entry=0x81f18au,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f18a),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f18d", .symbol="$81:F18D", .entry=0x81f18du,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f18d),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f18f", .symbol="$81:F18F", .entry=0x81f18fu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f18f),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f191", .symbol="$81:F191", .entry=0x81f191u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f191),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f193", .symbol="$81:F193", .entry=0x81f193u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f193),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f195", .symbol="$81:F195", .entry=0x81f195u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f195),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f198", .symbol="$81:F198", .entry=0x81f198u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f198),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f19a", .symbol="$81:F19A", .entry=0x81f19au,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f19a),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f19c", .symbol="$81:F19C", .entry=0x81f19cu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f19c),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f19e", .symbol="$81:F19E", .entry=0x81f19eu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f19e),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1a0", .symbol="$81:F1A0", .entry=0x81f1a0u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1a0),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1a3", .symbol="$81:F1A3", .entry=0x81f1a3u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1a3),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1a5", .symbol="$81:F1A5", .entry=0x81f1a5u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1a5),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1a7", .symbol="$81:F1A7", .entry=0x81f1a7u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1a7),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1a9", .symbol="$81:F1A9", .entry=0x81f1a9u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1a9),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1ab", .symbol="$81:F1AB", .entry=0x81f1abu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1ab),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1ad", .symbol="$81:F1AD", .entry=0x81f1adu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1ad),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1b0", .symbol="$81:F1B0", .entry=0x81f1b0u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1b0),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1b2", .symbol="$81:F1B2", .entry=0x81f1b2u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1b2),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1b5", .symbol="$81:F1B5", .entry=0x81f1b5u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1b5),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1b7", .symbol="$81:F1B7", .entry=0x81f1b7u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1b7),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1b8", .symbol="$81:F1B8", .entry=0x81f1b8u,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1b8),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1bb", .symbol="$81:F1BB", .entry=0x81f1bbu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1bb),
      .uncalled=true, .cycles=120 },
    { .name="r62_native_81f1bd", .symbol="$81:F1BD", .entry=0x81f1bdu,
      .run=shim_r62, .supported=guard_r62,
      .supported_readonly=true, COSIM_EXITS(r62_exit_81f1bd),
      .uncalled=true, .cycles=120 },
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R63_POSTCALL_NATIVE)
    { .name="r63_native_81f1c4", .symbol="$81:F1C4", .entry=0x81f1c4u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1c4),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1c7", .symbol="$81:F1C7", .entry=0x81f1c7u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1c7),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1c9", .symbol="$81:F1C9", .entry=0x81f1c9u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1c9),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1ca", .symbol="$81:F1CA", .entry=0x81f1cau,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1ca),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1cb", .symbol="$81:F1CB", .entry=0x81f1cbu,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1cb),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1e6", .symbol="$81:F1E6", .entry=0x81f1e6u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1e6),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1ed", .symbol="$81:F1ED", .entry=0x81f1edu,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1ed),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1ee", .symbol="$81:F1EE", .entry=0x81f1eeu,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1ee),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1f1", .symbol="$81:F1F1", .entry=0x81f1f1u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1f1),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1f4", .symbol="$81:F1F4", .entry=0x81f1f4u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1f4),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_81f1f9", .symbol="$81:F1F9", .entry=0x81f1f9u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_81f1f9),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_829962", .symbol="$82:9962", .entry=0x829962u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_829962),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_829969", .symbol="$82:9969", .entry=0x829969u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_829969),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_82996a", .symbol="$82:996A", .entry=0x82996au,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_82996a),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_82996d", .symbol="$82:996D", .entry=0x82996du,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_82996d),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_829970", .symbol="$82:9970", .entry=0x829970u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_829970),
      .uncalled=true, .cycles=120 },
    { .name="r63_native_829975", .symbol="$82:9975", .entry=0x829975u,
      .run=shim_r63, .supported=guard_r63,
      .supported_readonly=true, COSIM_EXITS(r63_exit_829975),
      .uncalled=true, .cycles=120 },
#endif

#if defined(XBOX_PORT) && defined(ZAMN_R64_ACTOR_RECORD_NATIVE)
    { .name="r64_native_81f204", .symbol="$81:F204", .entry=0x81f204u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f206", .symbol="$81:F206", .entry=0x81f206u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f208", .symbol="$81:F208", .entry=0x81f208u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f20b", .symbol="$81:F20B", .entry=0x81f20bu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f20c", .symbol="$81:F20C", .entry=0x81f20cu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f20e", .symbol="$81:F20E", .entry=0x81f20eu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f211", .symbol="$81:F211", .entry=0x81f211u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f214", .symbol="$81:F214", .entry=0x81f214u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f217", .symbol="$81:F217", .entry=0x81f217u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f219", .symbol="$81:F219", .entry=0x81f219u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f21c", .symbol="$81:F21C", .entry=0x81f21cu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f21f", .symbol="$81:F21F", .entry=0x81f21fu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f222", .symbol="$81:F222", .entry=0x81f222u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f225", .symbol="$81:F225", .entry=0x81f225u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f228", .symbol="$81:F228", .entry=0x81f228u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f22b", .symbol="$81:F22B", .entry=0x81f22bu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f22e", .symbol="$81:F22E", .entry=0x81f22eu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f231", .symbol="$81:F231", .entry=0x81f231u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f234", .symbol="$81:F234", .entry=0x81f234u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f237", .symbol="$81:F237", .entry=0x81f237u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f23a", .symbol="$81:F23A", .entry=0x81f23au,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f23d", .symbol="$81:F23D", .entry=0x81f23du,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f23f", .symbol="$81:F23F", .entry=0x81f23fu,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f241", .symbol="$81:F241", .entry=0x81f241u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f244", .symbol="$81:F244", .entry=0x81f244u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f246", .symbol="$81:F246", .entry=0x81f246u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
    { .name="r64_native_81f249", .symbol="$81:F249", .entry=0x81f249u,
      .run=shim_r64, .supported=guard_r64,
      .supported_readonly=true, COSIM_EXITS(r64_exit),
      .uncalled=true, .cycles=160 },
#endif

};

const CosimRoutine* cosim_routines(int* count) {
  *count = (int)(sizeof ROUTINES / sizeof ROUTINES[0]);
  return ROUTINES;
}
