// Native sections of the collision scan and unflipped sprite emitter.
#ifndef PORT_HOTPATHS_H
#define PORT_HOTPATHS_H
#include "port/cpu.h"
typedef struct { int fast_cycles, rom_bytes; } NativeHotWork;
bool native_overlap_supported(const Wram* w, const PortCpu* c);
void native_overlap_scan(Wram* w, PortCpu* c, NativeHotWork* k);
bool native_sprite_supported(const Wram* w, const Rom* rom, const PortCpu* c);
void native_sprite_emit(Wram* w, const Rom* rom, PortCpu* c, NativeHotWork* k);
bool native_oam_walk_supported(const Wram* w, const Rom* rom, const PortCpu* c);
void native_oam_walk(Wram* w, const Rom* rom, PortCpu* c, NativeHotWork* k);
// R53: bounded, register-exact 65816 movement and actor-update native fragments.
bool native_r53_supported(const Wram* w, const Rom* rom, const PortCpu* c);
void native_r53_movement(Wram* w, const Rom* rom, PortCpu* c, NativeHotWork* work);
// R54: $82:D9xx native bounded actor-state fragments.
bool native_r54_supported(const Wram*,const Rom*,const PortCpu*);
void native_r54_d9_cluster(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R55: actor/update basic blocks and state fragments.
bool native_r55_supported(const Wram*,const Rom*,const PortCpu*);
void native_r55_actor_fragments(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R59 collision + thread + actor-control 65816 native fragments.
bool native_r59_supported(const Wram*,const Rom*,const PortCpu*);
void native_r59_collision_fragments(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R60 connected 65816 fallback sections.
bool native_r60_supported(const Wram*,const Rom*,const PortCpu*);
void native_r60_connected_blocks(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R61 connected thread/collision stack and actor-state fragments.
bool native_r61_supported(const Wram*,const Rom*,const PortCpu*);
void native_r61_thread_actor(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R62 native actor movement/direction and state bookkeeping.
bool native_r62_supported(const Wram*,const Rom*,const PortCpu*);
void native_r62_actor_control(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R63 native thread/actor post-callback continuations.
bool native_r63_supported(const Wram*,const Rom*,const PortCpu*);
void native_r63_postcall(Wram*,const Rom*,PortCpu*,NativeHotWork*);
// R64: $81:F204-F249 actor-record setup, original engine call boundaries.
bool native_r64_supported(const Wram*,const Rom*,const PortCpu*);
void native_r64_actor_record(Wram*,const Rom*,PortCpu*,NativeHotWork*);
#endif
