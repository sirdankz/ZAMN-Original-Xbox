#include "xbox_platform.h"

#define BLOCK_FRAMES ZAMN_AUDIO_SAMPLES_PER_FRAME
#define BLOCK_BYTES (BLOCK_FRAMES * ZAMN_AUDIO_CHANNELS * 2)
#define RING_BLOCKS 8
#define RING_BYTES (RING_BLOCKS * BLOCK_BYTES)
#define DS_BLOCKS 4
#define DS_BYTES (DS_BLOCKS * BLOCK_BYTES)

static int16_t s_ring[RING_BLOCKS * BLOCK_FRAMES * ZAMN_AUDIO_CHANNELS];
static volatile LONG s_wp = 0, s_rp = 0;
static LPDIRECTSOUND8 s_ds = NULL;
static LPDIRECTSOUNDBUFFER s_buf = NULL;
static DWORD s_ds_wp = 0;
static HANDLE s_thread = NULL;
static volatile bool s_run = false;
static volatile LONG s_muted = 0;
// R49.9.2: blocking frontend operations can nest (for example a leaderboard
// menu flush followed by a board fetch). Keep a depth so an inner operation
// cannot unmute DirectSound while an outer loading transition is still active.
static volatile LONG s_mute_depth = 0;

static DWORD WINAPI AudioThread(LPVOID) {
  while (s_run) {
    if (s_muted) { Sleep(2); continue; }
    if (s_buf) {
      DWORD play = 0, write = 0;
      if (SUCCEEDED(s_buf->GetCurrentPosition(&play, &write))) {
        DWORD freeBytes = s_ds_wp <= play ? play - s_ds_wp : DS_BYTES - s_ds_wp + play;
        while (freeBytes >= BLOCK_BYTES) {
          LONG wp = s_wp, rp = s_rp;
          LONG avail = (wp - rp + RING_BYTES) % RING_BYTES;
          if (avail < BLOCK_BYTES) break;
          LONG slot = (rp / BLOCK_BYTES) % RING_BLOCKS;
          BYTE *src = (BYTE*)(s_ring + slot * BLOCK_FRAMES * ZAMN_AUDIO_CHANNELS);
          void *p1 = NULL, *p2 = NULL; DWORD n1 = 0, n2 = 0;
          if (FAILED(s_buf->Lock(s_ds_wp, BLOCK_BYTES, &p1, &n1, &p2, &n2, 0))) break;
          if (p1 && n1) memcpy(p1, src, n1);
          if (p2 && n2) memcpy(p2, src + n1, n2);
          s_buf->Unlock(p1, n1, p2, n2);
          s_ds_wp = (s_ds_wp + BLOCK_BYTES) % DS_BYTES;
          s_rp = (rp + BLOCK_BYTES) % RING_BYTES;
          freeBytes -= BLOCK_BYTES;
        }
      }
    }
    Sleep(2);
  }
  return 0;
}

extern "C" HRESULT Xbox_Audio_Init(void) {
  HRESULT hr = DirectSoundCreate(NULL, &s_ds, NULL);
  if (FAILED(hr)) { Xbox_Log("DirectSoundCreate failed 0x%08X\n", hr); return hr; }
  WAVEFORMATEX w; ZeroMemory(&w, sizeof(w));
  w.wFormatTag = WAVE_FORMAT_PCM;
  w.nChannels = ZAMN_AUDIO_CHANNELS;
  w.nSamplesPerSec = ZAMN_AUDIO_RATE;
  w.wBitsPerSample = 16;
  w.nBlockAlign = w.nChannels * 2;
  w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
  DSBUFFERDESC d; ZeroMemory(&d, sizeof(d));
  d.dwSize = sizeof(d); d.dwFlags = DSBCAPS_CTRLVOLUME; d.dwBufferBytes = DS_BYTES; d.lpwfxFormat = &w;
  hr = s_ds->CreateSoundBuffer(&d, &s_buf, NULL);
  if (FAILED(hr)) { Xbox_Log("CreateSoundBuffer failed 0x%08X\n", hr); return hr; }
  void *p1 = NULL, *p2 = NULL; DWORD n1 = 0, n2 = 0;
  if (SUCCEEDED(s_buf->Lock(0, DS_BYTES, &p1, &n1, &p2, &n2, 0))) {
    if (p1) ZeroMemory(p1, n1); if (p2) ZeroMemory(p2, n2); s_buf->Unlock(p1,n1,p2,n2);
  }
  s_wp = s_rp = 0; s_ds_wp = 0; s_muted = 0; s_mute_depth = 0;
  s_buf->SetVolume(DSBVOLUME_MAX);
  s_buf->Play(0, 0, DSBPLAY_LOOPING);
  s_run = true;
  s_thread = CreateThread(NULL, 0, AudioThread, NULL, 0, NULL);
  Xbox_Log("audio: 48 kHz stereo init %s\n", s_thread ? "OK" : "thread FAILED");
  return s_thread ? S_OK : E_FAIL;
}

extern "C" void Xbox_Audio_Submit(const int16_t *samples, int frames) {
  if (!samples || frames != BLOCK_FRAMES || s_muted) return;
  LONG wp = s_wp, rp = s_rp;
  LONG used = (wp - rp + RING_BYTES) % RING_BYTES;
  if (used + BLOCK_BYTES >= RING_BYTES) return;
  LONG slot = (wp / BLOCK_BYTES) % RING_BLOCKS;
  memcpy(s_ring + slot * BLOCK_FRAMES * ZAMN_AUDIO_CHANNELS, samples, BLOCK_BYTES);
  s_wp = (wp + BLOCK_BYTES) % RING_BYTES;
}

extern "C" void Xbox_Audio_SetMuted(bool muted) {
  if (muted) {
    const LONG depth = InterlockedIncrement(&s_mute_depth);
    if (depth > 1) return;

    // R49.9.2: volume-only muting was not strong enough on all Xbox paths: a
    // blocking frontend/network call could leave the hardware loop cursor
    // repeating the last PCM block. Stop playback on the outermost mute, stop
    // the feeder, discard queued PCM, and clear the entire loop buffer.
    InterlockedExchange(&s_muted, 1);
    if (s_buf) {
      s_buf->SetVolume(DSBVOLUME_MIN);
      s_buf->Stop();
    }
    Sleep(3); // let AudioThread observe s_muted before the buffer clear
    InterlockedExchange(&s_rp, s_wp);
    if (s_buf) {
      void *p1 = NULL, *p2 = NULL; DWORD n1 = 0, n2 = 0;
      if (SUCCEEDED(s_buf->Lock(0, DS_BYTES, &p1, &n1, &p2, &n2, 0))) {
        if (p1 && n1) ZeroMemory(p1, n1);
        if (p2 && n2) ZeroMemory(p2, n2);
        s_buf->Unlock(p1, n1, p2, n2);
      }
    }
    return;
  }

  LONG depth = s_mute_depth;
  if (depth > 0) depth = InterlockedDecrement(&s_mute_depth);
  else InterlockedExchange(&s_mute_depth, 0);
  if (depth > 0) return;
  if (depth < 0) InterlockedExchange(&s_mute_depth, 0);

  // Only the outermost release restarts DirectSound. Point the software writer
  // at the hardware-safe write cursor and resume with a completely silent
  // buffer; fresh title/game PCM is submitted after the transition returns.
  InterlockedExchange(&s_rp, s_wp);
  if (s_buf) {
    DWORD play = 0, write = 0;
    if (SUCCEEDED(s_buf->GetCurrentPosition(&play, &write))) s_ds_wp = write % DS_BYTES;
    s_buf->SetVolume(DSBVOLUME_MAX);
    s_buf->Play(0, 0, DSBPLAY_LOOPING);
  }
  InterlockedExchange(&s_muted, 0);
}

extern "C" void Xbox_Audio_Shutdown(void) {
  s_run = false;
  if (s_thread) { WaitForSingleObject(s_thread, 1000); CloseHandle(s_thread); s_thread = NULL; }
  if (s_buf) { s_buf->Stop(); s_buf->Release(); s_buf = NULL; }
  if (s_ds) { s_ds->Release(); s_ds = NULL; }
}
