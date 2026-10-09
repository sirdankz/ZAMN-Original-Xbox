#include "cosim/profile.h"
#include <stdlib.h>

/* Profiling is a desktop analysis feature. The playable Xbox frontend never
   enables it, but cosim.c still contains references to the API. Keep tiny
   link-compatible stubs instead of pulling filesystem/profile tooling into XBE. */
struct CosimProfile { int unused; };
CosimProfile* cosim_profile_new(uint32_t rom_size) { (void)rom_size; return NULL; }
void cosim_profile_free(CosimProfile* p) { (void)p; }
void cosim_profile_exec(CosimProfile* p, uint32_t off, uint32_t pc24, int len) { (void)p; (void)off; (void)pc24; (void)len; }
void cosim_profile_call(CosimProfile* p, uint32_t caller24, uint32_t callee24) { (void)p; (void)caller24; (void)callee24; }
void cosim_profile_entry(CosimProfile* p, uint32_t pc24) { (void)p; (void)pc24; }
bool cosim_profile_save(const CosimProfile* p, const char* dir) { (void)p; (void)dir; return false; }
