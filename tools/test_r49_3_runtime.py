"""Windows host checks. Needs Python, VS C++ Build Tools, Windows SDK and user's ROM.
Run: python tools/test_r49_3_runtime.py [PATH_TO_ROM]
Generated files go to work/r49-tests, never into the source archives.
"""
from pathlib import Path
import os, subprocess, sys
root=Path(__file__).resolve().parents[1]
out=root/'work/r493-tests';out.mkdir(parents=True,exist_ok=True)
vcroot=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC'
vc=sorted(vcroot.iterdir())[-1]
sdk=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Windows Kits/10'
ver=sorted((sdk/'Lib').iterdir())[-1].name
env=os.environ.copy()
env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/ver/'ucrt',sdk/'Include'/ver/'um',sdk/'Include'/ver/'shared']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/ver/'ucrt/x64',sdk/'Lib'/ver/'um/x64']))
env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']

import json
r=root
(out/'strings.h').write_text('#include <string.h>\n#define strcasecmp _stricmp\n#define strncasecmp _strnicmp\n')
manifest=json.loads((r/'rxdk.project.json').read_text())
src=[r/p for p in manifest['configurations']['Release']['sources'] if p.endswith('.c') and not p.endswith('runtime.c')]
defs=['_XBOX', 'XBOX', '__ASSERT_VERBOSE', 'XBOX_PORT', 'D3DCOMPILE_NOTINLINE=1', 'COSIM_FAST_RUNTIME=1', 'ZAMN_R15_THINLTO=1', 'ZAMN_R20_LIGHT_PROF=1', 'ZAMN_R23_MMX_PPU=1', 'ZAMN_R24_RELEASE_FAST=1', 'ZAMN_R28_NATIVE_BULK_BURN=1', 'ZAMN_R31_MMX_TRANSPARENT_EARLYOUT=1', 'ZAMN_R32_PPU_HOISTS=1', 'ZAMN_R33_TRUE_NATIVE_CUTOVER=1', 'ZAMN_R33_RELEASE_LEAN=1', 'ZAMN_R34_TRUE_NATIVE_CUTOVER=1', 'ZAMN_R35_NATIVE_OBJECT_PIPELINE=1', 'ZAMN_R36_NATIVE_LEAN=1', 'ZAMN_R37_READONLY_GUARDS=1', 'ZAMN_R38_READONLY_WALK_GUARD=1', 'ZAMN_R39_BUFFERED_LOG=1', 'ZAMN_R40_FAST_OAM_PREFLIGHT=1', 'ZAMN_R41_TRANSITION_WAIT_CUTOVER=1', 'ZAMN_R41_NATIVE_RENDER_OWNER=1', 'ZAMN_R41_NATIVE_AUDIO=1', 'ZAMN_R42_LEVEL_INTRO_WAIT_CUTOVER=1', 'ZAMN_R43_NATIVE_WAIT_CUTOVER=1', 'ZAMN_R44_NATIVE_BURN_ATTRIB=1', 'ZAMN_R45_APU_SET_NATIVE_TRANSFER=1', 'ZAMN_R46_APU_COMPACT_TRACE=1', 'ZAMN_R46_STRICT_NATIVE_SHARE=1', 'ZAMN_R47_NETPLAY=1', 'ZAMN_R48_ROLLBACK=1', 'ZAMN_R41_AUDIO_DEFAULT=1', 'ZAMN_R421_AUDIO_PROFILE=2', 'ZAMN_R421_ENHANCED_PRESETS=1', 'ZAMN_R291_HOT_RESIDUE=1']
clang=r'C:\ProgramData\RXDK\llvm\xboxog-windows-x64\bin\clang.exe'
cmd=[clang,'-std=c23','-O2','-fms-extensions','-Wno-deprecated-declarations',
     '-I'+str(out),'-I'+str(r/'src'),'-I'+str(r/'third_party/lakesnes/snes'),
     *['-D'+d for d in defs],str(r/'tools/test_r49_3_runtime.c'),*map(str,src),
     '-o',str(out/'runtime.exe'),'-Wl,/STACK:16777216']
result=subprocess.run(cmd,env=env,cwd=out,capture_output=True,text=True)
(out/'compile.txt').write_text(result.stdout+result.stderr)
print((result.stdout+result.stderr)[-2000:]);result.check_returncode()
subprocess.run([str(out/'runtime.exe'),str(Path(sys.argv[1]).resolve() if len(sys.argv)>1 else r/'zamn.sfc')],cwd=out,check=True)
