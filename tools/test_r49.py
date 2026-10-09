"""Windows host checks. Needs Python, VS C++ Build Tools, Windows SDK and user's ROM.
Run: python tools/test_r49.py PATH_TO_ROM
Generated files go to work/r49-tests, never into the source archives.
"""
from pathlib import Path
import os, subprocess, sys
root=Path(__file__).resolve().parents[1]
out=root/'work/r49-tests';out.mkdir(parents=True,exist_ok=True)
vcroot=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC'
vc=sorted(vcroot.iterdir())[-1]
sdk=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Windows Kits/10'
ver=sorted((sdk/'Lib').iterdir())[-1].name
env=os.environ.copy()
env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/ver/'ucrt',sdk/'Include'/ver/'um',sdk/'Include'/ver/'shared']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/ver/'ucrt/x64',sdk/'Lib'/ver/'um/x64']))
env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
def compile_run(name,sources,extra=(),args=()):
    cmd=[str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/O2','/I'+str(root/'src'),
         '/I'+str(root/'third_party/lakesnes/snes'),'/I'+str(out),*extra,
         *[str(root/s) for s in sources],'/Fe'+str(out/(name+'.exe')),'/Fo'+str(out)+os.sep]
    subprocess.run(cmd,cwd=out,env=env,check=True)
    subprocess.run([str(out/(name+'.exe')),*args],cwd=out,check=True)

net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text()
main=(root/'src/platform/xbox/xbox_main.cpp').read_text()
def section(s,a,b):return s[s.index(a):s.index(b,s.index(a))]
parts=[section(net,'struct RollbackPrediction','static void rollback_reset(uint8_t delay, uint8_t window);'),
       section(net,'#define ZNP_AUTO_WINDOW','static int step_delay_option'),
       section(net,'static bool rollback_auto_lan_bypass','static bool wait_host_handshake'),
       section(net,'static void rollback_reset(uint8_t delay, uint8_t window) {','static const char* exit_reason_name'),
       section(net,'bool Xbox_Netplay_RollbackEnabled(void)','bool Xbox_Netplay_AfterFrame'),
       section(main,'#define R48_RB_SLOTS','#endif')]
production='\n'.join(parts)
production=production.replace('  R498LevelClock clock;\n','')
production=production.replace('  slot->clock = s_r498_clock;\n','')
production=production.replace('  s_r498_clock=slot->clock;\n  R498_PendingScoresDiscardFrom(frame);\n','')
production=production.replace('  R498_PendingScoresClear();\n','')
production=production.replace('    {\n      uint16_t completed_level=0;uint32_t completed_frames=0;\n      if(R498_AdvanceLevelClock(runtime,s_r498_ruleset,2,&completed_level,&completed_frames))\n        R498_QueuePendingScore(s_r498_ruleset,completed_level,completed_frames,2,false,rf);\n    }\n','')
(out/'r49_production.inc').write_text(production)
compile_run('rollback',['tools/test_r49_rollback.cpp','src/netplay/netplay_core.c'],['/std:c++17','/TP'])
if len(sys.argv)!=2:raise SystemExit('Pass your ROM path to also run native/reference checks.')
compile_run('player',['tools/test_r49_player.c','src/port/player_resume.c',
                    'third_party/lakesnes/snes/cpu.c','third_party/lakesnes/snes/statehandler.c'],['/std:c11'],[str(Path(sys.argv[1]).resolve())])
