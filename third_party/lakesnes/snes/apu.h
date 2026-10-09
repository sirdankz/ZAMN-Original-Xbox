
#ifndef APU_H
#define APU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct Apu Apu;

#include "snes.h"
#include "spc.h"
#include "dsp.h"
#include "statehandler.h"

typedef struct Timer {
  uint8_t cycles;
  uint8_t divider;
  uint8_t target;
  uint8_t counter;
  bool enabled;
} Timer;

struct Apu {
  Snes* snes;
  Spc* spc;
  Dsp* dsp;
  uint8_t ram[0x10000];
  bool romReadable;
  uint8_t dspAdr;
  uint32_t cycles;
  uint8_t inPorts[6]; // includes 2 bytes of ram
  uint8_t outPorts[4];
  Timer timer[3];
};

Apu* apu_init(Snes* snes);
void apu_free(Apu* apu);
void apu_reset(Apu* apu);
void apu_handleState(Apu* apu, StateHandler* sh);
int apu_runCycles(Apu* apu, int wantedCycles);
#if defined(XBOX_PORT) && defined(ZAMN_R69_PORTING_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
#define ZAMN_R69_SPC_TOP 5
#define ZAMN_R69_AUDIO_CMD_TOP 4
typedef struct {
  uint16_t pc_page[ZAMN_R69_SPC_TOP];
  uint32_t page_samples[ZAMN_R69_SPC_TOP];
  uint8_t opcode[ZAMN_R69_SPC_TOP];
  uint32_t opcode_samples[ZAMN_R69_SPC_TOP];
  uint8_t command[ZAMN_R69_AUDIO_CMD_TOP];
  uint32_t command_writes[ZAMN_R69_AUDIO_CMD_TOP];
  uint64_t run_calls, executed_opcodes, executed_cycles, dsp_writes, port_writes[4], total_samples;
} ZamnR69ApuWindow;
void apu_r69_cpu_port_write(uint8_t index, uint8_t value);
void apu_r69_profile_take(ZamnR69ApuWindow* out);
#endif
uint8_t apu_read(Apu* apu, uint16_t adr);
void apu_write(Apu* apu, uint16_t adr, uint8_t val);
uint8_t apu_spcRead(void* mem, uint16_t adr);
void apu_spcWrite(void* mem, uint16_t adr, uint8_t val);
void apu_spcIdle(void* mem, bool waiting);

#endif
