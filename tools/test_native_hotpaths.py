"""ROM-free differential tests. No xemu or gameplay is launched.
Requires VS 2022 Build Tools, Windows SDK, RXDK clang and the supplied source ZIP.
Outputs stay under this repository's work/perf-validation directory.
"""
from pathlib import Path
import os, re, subprocess, zipfile
root=Path(__file__).resolve().parents[1]
out=root/'work/hotpath-validation'; out.mkdir(parents=True,exist_ok=True)
vcroot=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC'
vc=sorted(vcroot.iterdir())[-1]
sdk=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Windows Kits/10'
ver=sorted((sdk/'Lib').iterdir())[-1].name
env=os.environ.copy()
env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/ver/'ucrt',sdk/'Include'/ver/'um',sdk/'Include'/ver/'shared']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/ver/'ucrt/x64',sdk/'Lib'/ver/'um/x64']))
env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['TMP']=env['TEMP']=str(out)

import sys
clang=r'C:\ProgramData\RXDK\llvm\xboxog-windows-x64\bin\clang.exe'
cmd=[clang,'-std=c23','-O2','-D_CRT_SECURE_NO_WARNINGS','-I'+str(root/'src'),'-I'+str(root/'third_party/lakesnes/snes'),str(root/'tools/test_native_hotpaths.c'),str(root/'src/port/hotpaths.c'),str(root/'src/assets/rom.c'),str(root/'third_party/lakesnes/snes/cpu.c'),str(root/'third_party/lakesnes/snes/statehandler.c'),'-o',str(out/'hotpaths.exe')]
subprocess.run(cmd,cwd=out,env=env,check=True)
rom=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root.parent/'zamn.sfc'
result=subprocess.run([str(out/'hotpaths.exe'),str(rom)],cwd=out,env=env,capture_output=True,text=True)
(out/'results.txt').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr);result.check_returncode()
