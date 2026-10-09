// R65: exact hot-page native-entry lookup. This cache changes ONLY how an
// existing routine index is found; it cannot execute, skip or alter guest code.
// Five dense 256-byte instruction pages cover repeated thread/collision/actor
// activity in R64's diagnostic samples. Zero means no registered entry.
#ifndef ZAMN_COSIM_R65_HOT_DISPATCH_H
#define ZAMN_COSIM_R65_HOT_DISPATCH_H
#include <stdint.h>
#define R65_HOT_PAGES 5u
#define R65_HOT_BYTES 256u
typedef struct { uint16_t index_plus_one[R65_HOT_PAGES][R65_HOT_BYTES]; } R65HotMap;
// Return -1 for nonselected pages, otherwise 0..4. All address bits matter.
static inline int r65_hot_page(uint32_t pc) {
  switch (pc >> 8) {
    case 0x8084u: return 0; // thread dispatch
    case 0x80beu: return 1; // collision notify
    case 0x81f1u: return 2; // actor direction and scheduler
    case 0x8298u: return 3; // movement chain
    case 0x8299u: return 4; // high-frequency actor state
    default: return -1;
  }
}
// -2: not a hot page (use original hash); -1: no routine here; >=0 index.
static inline int r65_hot_lookup(const R65HotMap *map, uint32_t pc) {
  const int page = r65_hot_page(pc);
  if (page < 0) return -2;
  const uint16_t encoded=map->index_plus_one[page][pc & 255u];
  return encoded ? (int)encoded-1 : -1;
}
// False only on duplicate or nonselected page. Registry indices < 65535.
static inline int r65_hot_insert(R65HotMap *map, uint32_t pc, unsigned index) {
  const int page=r65_hot_page(pc);
  if (page<0) return 0;
  uint16_t *slot=&map->index_plus_one[page][pc&255u];
  if (*slot || index>=65535u) return -1;
  *slot=(uint16_t)(index+1u);
  return 1;
}
#endif
