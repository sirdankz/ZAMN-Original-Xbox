#include "native/audio.h"

#include <stddef.h>
#include <string.h>

#define ZSB_HEADER_BYTES 16u
#define ZSB_ENTRY_BYTES 20u
#define ZSB_COUNT 256u

static uint32_t rd32(const uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static int16_t clamp16(int32_t v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return (int16_t)v;
}

void zamn_native_audio_init(ZamnNativeAudio* a) {
  memset(a, 0, sizeof(*a));
  a->mode = ZAMN_AUDIO_ORIGINAL;
  a->profile = ZAMN_AUDIO_PROFILE_BALANCED;
  for (int i = 0; i < 8; ++i) a->voice[i].srcn = 0xff;
}

void zamn_native_audio_set_mode(ZamnNativeAudio* a, ZamnNativeAudioMode mode) {
  if (!a) return;
  if (mode < ZAMN_AUDIO_ORIGINAL || mode > ZAMN_AUDIO_REMASTERED)
    mode = ZAMN_AUDIO_ORIGINAL;
  a->mode = mode;
  a->bass_l = a->bass_r = 0;
  for (int i = 0; i < 8; ++i) {
    a->voice[i].phase_q16 = 0;
    a->voice[i].srcn = 0xff;
    a->voice[i].active = false;
  }
}

const char* zamn_native_audio_mode_name(const ZamnNativeAudio* a) {
  if (!a) return "Original";
  if (a->mode == ZAMN_AUDIO_REMASTERED) return "Remastered";
  if (a->mode == ZAMN_AUDIO_ENHANCED) return "Enhanced";
  return "Original";
}

void zamn_native_audio_set_profile(ZamnNativeAudio* a,
                                   ZamnNativeAudioProfile profile) {
  if (!a) return;
  if (profile < ZAMN_AUDIO_PROFILE_BALANCED ||
      profile > ZAMN_AUDIO_PROFILE_WIDE)
    profile = ZAMN_AUDIO_PROFILE_BALANCED;
  a->profile = profile;
  a->bass_l = a->bass_r = 0;
}

const char* zamn_native_audio_profile_name(const ZamnNativeAudio* a) {
  if (!a) return "Balanced";
  if (a->profile == ZAMN_AUDIO_PROFILE_PUNCHY) return "Punchy";
  if (a->profile == ZAMN_AUDIO_PROFILE_WIDE) return "Wide";
  return "Balanced";
}

bool zamn_native_audio_set_bank(ZamnNativeAudio* a, const uint8_t* data,
                                uint32_t size) {
  if (!a) return false;
  a->bank = NULL;
  a->bank_size = 0;
  memset(a->sample, 0, sizeof(a->sample));
  if (!data || size < ZSB_HEADER_BYTES + ZSB_COUNT * ZSB_ENTRY_BYTES)
    return false;
  if (memcmp(data, "ZSB1", 4) != 0 || rd32(data + 4) != 1u ||
      rd32(data + 8) != ZSB_COUNT || rd32(data + 12) != ZSB_ENTRY_BYTES)
    return false;

  const uint8_t* t = data + ZSB_HEADER_BYTES;
  for (uint32_t i = 0; i < ZSB_COUNT; ++i, t += ZSB_ENTRY_BYTES) {
    ZamnRemasteredSample e;
    e.offset = rd32(t + 0);
    e.frames = rd32(t + 4);
    e.loop_start = rd32(t + 8);
    e.loop_end = rd32(t + 12);
    e.rate = rd32(t + 16);
    if (!e.frames) continue;
    if (e.rate < 4000u || e.rate > 96000u) return false;
    if (e.offset < ZSB_HEADER_BYTES + ZSB_COUNT * ZSB_ENTRY_BYTES ||
        e.offset > size || e.frames > (size - e.offset) / 2u)
      return false;
    if (e.loop_end) {
      if (e.loop_start >= e.loop_end || e.loop_end > e.frames) return false;
    }
    a->sample[i] = e;
  }
  a->bank = data;
  a->bank_size = size;
  return true;
}

bool zamn_native_audio_has_bank(const ZamnNativeAudio* a) {
  return a && a->bank && a->bank_size;
}

bool zamn_native_audio_sample(void* ctx, int channel, uint8_t srcn,
                              uint16_t pitch, bool restart, bool delayed,
                              int16_t* out) {
  ZamnNativeAudio* a = (ZamnNativeAudio*)ctx;
  if (!a || !out || channel < 0 || channel >= 8 ||
      a->mode != ZAMN_AUDIO_REMASTERED || !a->bank)
    return false;
  const ZamnRemasteredSample* e = &a->sample[srcn];
  if (!e->frames) return false;

  if (restart || a->voice[channel].srcn != srcn) {
    a->voice[channel].phase_q16 = 0;
    a->voice[channel].srcn = srcn;
    a->voice[channel].active = true;
  }
  if (delayed) {
    *out = 0;
    return true;
  }
  if (!a->voice[channel].active) return false;

  uint64_t phase = a->voice[channel].phase_q16;
  uint32_t pos = (uint32_t)(phase >> 16);
  if (pos >= e->frames) {
    if (e->loop_end) {
      const uint32_t loop_len = e->loop_end - e->loop_start;
      pos = e->loop_start + (pos - e->loop_start) % loop_len;
      phase = ((uint64_t)pos << 16) | (phase & 0xffffu);
    } else {
      a->voice[channel].active = false;
      *out = 0;
      return true;
    }
  }

  uint32_t next = pos + 1;
  if (e->loop_end && next >= e->loop_end) next = e->loop_start;
  else if (next >= e->frames) next = e->frames - 1;
  const int16_t* pcm = (const int16_t*)(a->bank + e->offset);
  const int32_t s0 = pcm[pos], s1 = pcm[next];
  const uint32_t frac = (uint32_t)phase & 0xffffu;
  *out = (int16_t)(s0 + (((s1 - s0) * (int32_t)frac) >> 16));

  // DSP pitch $1000 is unity at the DSP's ~32 kHz output rate. In Q16 source
  // frames per DSP tick this simplifies to sample_rate * pitch / 2000.
  const uint32_t step_q16 =
      (uint32_t)(((uint64_t)e->rate * (uint64_t)pitch + 1000u) / 2000u);
  a->voice[channel].phase_q16 = phase + step_q16;
  return true;
}

void zamn_native_audio_process(ZamnNativeAudio* a, int16_t* stereo, int frames) {
  if (!a || !stereo || frames <= 0 || a->mode == ZAMN_AUDIO_ORIGINAL) return;

  for (int i = 0; i < frames; ++i) {
    int32_t l = stereo[i * 2 + 0];
    int32_t r = stereo[i * 2 + 1];

    if (a->profile == ZAMN_AUDIO_PROFILE_PUNCHY) {
      // Punchy: a slightly faster low shelf and twice the R41 bass lift. Keep
      // the stereo width restrained so explosions/weapons retain a solid
      // centre image. A small mid lift adds impact without a blanket gain bump.
      a->bass_l += (l - a->bass_l) >> 4;
      a->bass_r += (r - a->bass_r) >> 4;
      l += a->bass_l >> 3;  // +12.5% low-frequency body
      r += a->bass_r >> 3;

      int32_t mid = (l + r) >> 1;
      int32_t side = (l - r) >> 1;
      mid += mid >> 5;      // +3.125% centre punch
      side += side >> 4;    // +6.25% width
      l = mid + side;
      r = mid - side;

      // Slightly firmer knee than Balanced. Peaks keep their attack while the
      // added centre/bass energy cannot wrap or hard-clip.
      if (l > 27648) l = 27648 + ((l - 27648) >> 2);
      if (l < -27648) l = -27648 + ((l + 27648) >> 2);
      if (r > 27648) r = 27648 + ((r - 27648) >> 2);
      if (r < -27648) r = -27648 + ((r + 27648) >> 2);
    } else if (a->profile == ZAMN_AUDIO_PROFILE_WIDE) {
      // Wide: keep bass changes subtle and open the side component by 25%.
      // Centre information (dialogue-like cues, explosions, kick transients)
      // stays centred while music ambience occupies more stereo space.
      a->bass_l += (l - a->bass_l) >> 5;
      a->bass_r += (r - a->bass_r) >> 5;
      l += a->bass_l >> 5;  // +3.125% low-frequency body
      r += a->bass_r >> 5;

      const int32_t mid = (l + r) >> 1;
      int32_t side = (l - r) >> 1;
      side += side >> 2;    // +25% width
      l = mid + side;
      r = mid - side;

      if (l > 28160) l = 28160 + ((l - 28160) >> 2);
      if (l < -28160) l = -28160 + ((l + 28160) >> 2);
      if (r > 28160) r = 28160 + ((r - 28160) >> 2);
      if (r < -28160) r = -28160 + ((r + 28160) >> 2);
    } else {
      // Balanced: exact R41/R42 Enhanced mastering. Keep this branch stable so
      // it remains the known A/B reference from the user's hardware tests.
      a->bass_l += (l - a->bass_l) >> 5;
      a->bass_r += (r - a->bass_r) >> 5;
      l += a->bass_l >> 4;  // +6.25% low-frequency body
      r += a->bass_r >> 4;

      const int32_t mid = (l + r) >> 1;
      int32_t side = (l - r) >> 1;
      side += side >> 3;    // +12.5% width
      l = mid + side;
      r = mid - side;

      if (l > 28672) l = 28672 + ((l - 28672) >> 2);
      if (l < -28672) l = -28672 + ((l + 28672) >> 2);
      if (r > 28672) r = 28672 + ((r - 28672) >> 2);
      if (r < -28672) r = -28672 + ((r + 28672) >> 2);
    }

    stereo[i * 2 + 0] = clamp16(l);
    stereo[i * 2 + 1] = clamp16(r);
  }
}
