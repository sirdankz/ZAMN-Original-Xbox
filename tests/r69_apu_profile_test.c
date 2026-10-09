// Host-only deterministic diagnostic sampler test; not part of Xbox release.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "apu.h"

// Stub executes one instruction in exactly 3 APU cycles. The sampler must
// never touch CPU/SPC state; only diagnostics are read and reset.
void spc_runOpcode(Spc* spc) {
  ((Apu*)spc->mem)->cycles += 3;
  ++spc->pc;
}

int main(void) {
  Apu apu; Spc spc;
  memset(&apu, 0, sizeof(apu));
  memset(&spc, 0, sizeof(spc));
  apu.spc = &spc; spc.mem = &apu;
  for (unsigned i=0; i<sizeof(apu.ram); ++i) apu.ram[i] = (uint8_t)i;
  const int run = apu_runCycles(&apu, 6144);
  assert(run == 6144 && apu.cycles == 6144 && spc.pc == 2048);
  apu_r69_cpu_port_write(2, 0x31);
  apu_r69_cpu_port_write(2, 0x31);
  apu_r69_cpu_port_write(2, 0x05);
  apu_r69_cpu_port_write(1, 0xee);
  ZamnR69ApuWindow st;
  apu_r69_profile_take(&st);
  assert(st.run_calls == 1 && st.executed_opcodes == 2048);
  assert(st.executed_cycles == 6144 && st.total_samples == 2);
  assert(st.port_writes[2] == 3 && st.port_writes[1] == 1);
  assert(st.command[0] == 0x31 && st.command_writes[0] == 2);
  assert(st.page_samples[0] == 1 && st.page_samples[1] == 1);
  assert(st.pc_page[0] == 0x03f0 && st.pc_page[1] == 0x07f0);
  assert(st.opcode_samples[0] == 2 && st.opcode[0] == 0xff);
  apu_r69_profile_take(&st);
  assert(st.run_calls == 0 && st.total_samples == 0 && st.port_writes[2] == 0);
  puts("R69 APU diagnostic sampler PASS: 2048 opcodes, two samples, command histogram, window reset");
  return 0;
}
