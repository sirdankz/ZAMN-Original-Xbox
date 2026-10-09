#ifndef ZAMN_NATIVE_VIDEO_H
#define ZAMN_NATIVE_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Ppu Ppu;

typedef struct {
  uint64_t owned_lines;
  uint64_t fallback_lines;
} ZamnNativeVideo;

void zamn_native_video_init(ZamnNativeVideo* v);
void zamn_native_video_install(ZamnNativeVideo* v, Ppu* ppu);
bool zamn_native_video_render_line(void* ctx, Ppu* ppu, int line);
void zamn_native_video_take(ZamnNativeVideo* v, uint64_t* owned,
                            uint64_t* fallback);

#endif
