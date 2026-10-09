#include "native/video.h"

#include <string.h>

#include "ppu.h"

void zamn_native_video_init(ZamnNativeVideo* v) {
  if(v) memset(v, 0, sizeof(*v));
}

bool zamn_native_video_render_line(void* ctx, Ppu* ppu, int line) {
  ZamnNativeVideo* v = (ZamnNativeVideo*)ctx;
  const bool owned = ppu_xboxRenderNativeLine(ppu, line);
  if(v) {
    if(owned) ++v->owned_lines;
    else ++v->fallback_lines;
  }
  return owned;
}

void zamn_native_video_install(ZamnNativeVideo* v, Ppu* ppu) {
  if(!v || !ppu) return;
  ppu_setNativeLineRenderer(ppu, zamn_native_video_render_line, v);
}

void zamn_native_video_take(ZamnNativeVideo* v, uint64_t* owned,
                            uint64_t* fallback) {
  if(owned) *owned = v ? v->owned_lines : 0;
  if(fallback) *fallback = v ? v->fallback_lines : 0;
  if(v) v->owned_lines = v->fallback_lines = 0;
}
