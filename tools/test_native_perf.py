"""ROM-free differential tests. No xemu or gameplay is launched.
Requires VS 2022 Build Tools, Windows SDK, RXDK clang and the supplied source ZIP.
Outputs stay under this repository's work/perf-validation directory.
"""
from pathlib import Path
import os, re, subprocess, zipfile
root=Path(__file__).resolve().parents[1]
out=root/'work/perf-validation'; out.mkdir(parents=True,exist_ok=True)
vcroot=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC'
vc=sorted(vcroot.iterdir())[-1]
sdk=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Windows Kits/10'
ver=sorted((sdk/'Lib').iterdir())[-1].name
env=os.environ.copy()
env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/ver/'ucrt',sdk/'Include'/ver/'um',sdk/'Include'/ver/'shared']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/ver/'ucrt/x64',sdk/'Lib'/ver/'um/x64']))
env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
env['TMP']=env['TEMP']=str(out)
original=out/'original_statehandler.c'
if not original.exists():
    archive=Path.home()/'Downloads/ZAMN_R49_9_7_ASTRA_NATIVE_PERF_FULL_SOURCE.zip'
    with zipfile.ZipFile(archive) as z:
        original.write_bytes(z.read('ZAMN_R49_9_7_ASTRA_NATIVE_PERF_FULL_SOURCE/third_party/lakesnes/snes/statehandler.c'))
header=(root/'third_party/lakesnes/snes/statehandler.h').read_text()
names=re.findall(r'\b(sh_\w+)\(',header)
wrapper=out/'original_wrapper.c'
wrapper.write_text('\n'.join(f'#define {n} original_{n}' for n in names)+'\n#include "original_statehandler.c"\n')
clang=r'C:\ProgramData\RXDK\llvm\xboxog-windows-x64\bin\clang.exe'
cmd=[clang,'-std=c23','-O2','-I'+str(root/'src'),'-I'+str(root/'third_party/lakesnes/snes'),str(root/'tools/test_native_perf.c'),str(wrapper),str(root/'third_party/lakesnes/snes/statehandler.c'),str(root/'src/port/score.c'),str(root/'src/assets/rom.c'),'-o',str(out/'native_perf.exe')]
result=subprocess.run(cmd,cwd=out,env=env,capture_output=True,text=True)
(out/'compile.txt').write_text(result.stdout+result.stderr)
if result.returncode: print(result.stdout+result.stderr)
result.check_returncode()
result=subprocess.run([str(out/'native_perf.exe')],cwd=out,env=env,capture_output=True,text=True)
(out/'results.txt').write_text(result.stdout+result.stderr)
print(result.stdout+result.stderr); result.check_returncode()
