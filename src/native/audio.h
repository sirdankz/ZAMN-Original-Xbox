#ifndef ZAMN_NATIVE_AUDIO_H
#define ZAMN_NATIVE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  ZAMN_AUDIO_ORIGINAL = 0,
  ZAMN_AUDIO_ENHANCED = 1,
  ZAMN_AUDIO_REMASTERED = 2,
} ZamnNativeAudioMode;

typedef enum {
  ZAMN_AUDIO_PROFILE_BALANCED = 0,
  ZAMN_AUDIO_PROFILE_PUNCHY = 1,
  ZAMN_AUDIO_PROFILE_WIDE = 2,
} ZamnNativeAudioProfile;

typedef struct {
  uint32_t offset;
  uint32_t frames;
  uint32_t loop_start;
  uint32_t loop_end;
  uint32_t rate;
} ZamnRemasteredSample;

typedef struct {
  ZamnNativeAudioMode mode;
  ZamnNativeAudioProfile profile;
  const uint8_t* bank;
  uint32_t bank_size;
  ZamnRemasteredSample sample[256];
  struct {
    uint64_t phase_q16;
    uint8_t srcn;
    bool active;
  } voice[8];
  int32_t bass_l;
  int32_t bass_r;
} ZamnNativeAudio;

void zamn_native_audio_init(ZamnNativeAudio* a);
void zamn_native_audio_set_mode(ZamnNativeAudio* a, ZamnNativeAudioMode mode);
const char* zamn_native_audio_mode_name(const ZamnNativeAudio* a);
void zamn_native_audio_set_profile(ZamnNativeAudio* a,
                                   ZamnNativeAudioProfile profile);
const char* zamn_native_audio_profile_name(const ZamnNativeAudio* a);
bool zamn_native_audio_set_bank(ZamnNativeAudio* a, const uint8_t* data,
                                uint32_t size);
bool zamn_native_audio_has_bank(const ZamnNativeAudio* a);

// Hooked into the compatibility DSP only in Remastered mode. The SPC700 keeps
// sequencing notes, pitch, envelope and timing; this replaces the BRR waveform
// for a source-number when a higher-quality PCM entry exists in the bank.
bool zamn_native_audio_sample(void* ctx, int channel, uint8_t srcn,
                              uint16_t pitch, bool restart, bool delayed,
                              int16_t* out);

// Native Xbox-side mastering after the game has produced one 48-kHz block.
// Original is bit-identical. Enhanced and Remastered use one of three profiles:
// Balanced = the proven R41 mix, Punchy = stronger centred impact/low-end,
// Wide = more stereo space with restrained bass lift.
void zamn_native_audio_process(ZamnNativeAudio* a, int16_t* stereo, int frames);

#endif
