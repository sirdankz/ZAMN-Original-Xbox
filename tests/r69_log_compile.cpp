#include "native/runtime.h"
extern "C" void Xbox_Log(const char*, ...);
int main() { ZamnRuntimeWindowStats st = {};
#if defined(ZAMN_R69_PORTING_PROFILE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
      Xbox_Log("R69AUDIO apu-runs=%llu spc-opcodes=%llu apu-cycles=%llu samples=%llu dsp-writes=%llu cpu-to-spc=%llu/%llu/%llu/%llu\n",
               (unsigned long long)st.r69_apu.run_calls,
               (unsigned long long)st.r69_apu.executed_opcodes,
               (unsigned long long)st.r69_apu.executed_cycles,
               (unsigned long long)st.r69_apu.total_samples,
               (unsigned long long)st.r69_apu.dsp_writes,
               (unsigned long long)st.r69_apu.port_writes[0],
               (unsigned long long)st.r69_apu.port_writes[1],
               (unsigned long long)st.r69_apu.port_writes[2],
               (unsigned long long)st.r69_apu.port_writes[3]);
      for (int k=0; k<ZAMN_R69_SPC_TOP && st.r69_apu.page_samples[k]; ++k)
        Xbox_Log("R69SPC rank=%d page=%04X-%04X pc-samples=%lu opcode=%02X opcode-samples=%lu\n",
          k+1, (unsigned)st.r69_apu.pc_page[k],
          (unsigned)(st.r69_apu.pc_page[k]+15u),
          (unsigned long)st.r69_apu.page_samples[k],
          (unsigned)st.r69_apu.opcode[k],
          (unsigned long)st.r69_apu.opcode_samples[k]);
      for (int k=0; k<ZAMN_R69_AUDIO_CMD_TOP && st.r69_apu.command_writes[k]; ++k)
        Xbox_Log("R69SOUND rank=%d port2142-value=%02X writes=%lu (candidate sound command, not confirmed SFX ID)\n",
          k+1, (unsigned)st.r69_apu.command[k],
          (unsigned long)st.r69_apu.command_writes[k]);
      for (int k=0; k<ZAMN_R69_GUARD_TOP && st.r69_guard_hot[k].calls; ++k) {
        const ZamnR69GuardHot& p = st.r69_guard_hot[k];
        Xbox_Log("R69GUARD rank=%d guard=%02X:%04X handler=%02X:%04X copies=%llu bytes=%llu declines=%llu sample-arg=%04X sample-db=%02X%s\n",
          k+1, (unsigned)(p.guard_pc>>16), (unsigned)(p.guard_pc&0xffffu),
          (unsigned)(p.handler_pc>>16), (unsigned)(p.handler_pc&0xffffu),
          (unsigned long long)p.calls, (unsigned long long)p.bytes,
          (unsigned long long)p.declines, (unsigned)p.example_arg,
          (unsigned)(p.example_db&0xffu),
          p.handler_pc ? "" : " (no unique handler attribution)");
      }
      if (st.r69_guard_untracked)
        Xbox_Log("R69GUARD untracked-unique-cases=%llu (profiler table full)\n",
          (unsigned long long)st.r69_guard_untracked);
#endif
return 0;}
