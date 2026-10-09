#ifndef PORT_PLAYER_RESUME_H
#define PORT_PLAYER_RESUME_H
#include "port/cpu.h"

// R49: idle/walk state dispatch and its animation helper. External shooting,
// pose setup and movement calls retain their existing native/ROM owners.
// Exits are before JSR/RTS, or after a JMP to its destination. No stack writes.
typedef struct { int fast_cycles, rom_bytes, dp_accesses; } PlayerResumeWork;
void player_idle_resume(const Wram*, PortCpu*, PlayerResumeWork*);
void player_walk_resume(const Wram*, PortCpu*, PlayerResumeWork*);
void player_walk_fire_resume(Wram*, PortCpu*, PlayerResumeWork*);
void player_walk_after_fire(Wram*, PortCpu*, PlayerResumeWork*);
void player_walk_animation(Wram*, PortCpu*, PlayerResumeWork*);
#endif
