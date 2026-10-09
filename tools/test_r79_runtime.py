from pathlib import Path
import re, subprocess, os, json
r=Path(__file__).resolve().parents[1]
o=r/'work/r79-test';o.mkdir(parents=True,exist_ok=True)
base=(r/'tools/test_r49_3_runtime.py').read_text(); setup=base[base.index('vcroot='):base.index('import json')]; exec(setup)
(o/'strings.h').write_text('#include <string.h>\n#define strcasecmp _stricmp\n#define strncasecmp _strnicmp\n')
defs=re.findall(r"'([A-Z][A-Z0-9_]*(?:=[^']+)?)'",(r/'BUILD_XBOX_RELEASE.ps1').read_text().split('foreach ($d in @(')[1].split('))')[0]); defs += ['XBOX_PORT']
sources=[str(r/p) for p in json.loads((r/'rxdk.project.json').read_text())['configurations']['Release']['sources'] if p.endswith('.c') and not p.endswith('runtime.c')]
cmd=[r'C:\ProgramData\RXDK\llvm\xboxog-windows-x64\bin\clang.exe','-std=c23','-O1','-fms-extensions','-Wno-deprecated-declarations','-I'+str(o),'-I'+str(r/'src'),'-I'+str(r/'third_party/lakesnes/snes'),*['-D'+d for d in defs],str(r/'tests/r79_runtime_weapon_test.c'),*sources,'-o',str(o/'probe.exe'),'-Wl,/STACK:16777216']
p=subprocess.run(cmd,env=env,capture_output=True,text=True);(o/'compile.log').write_text(p.stdout+p.stderr);print((p.stdout+p.stderr)[-2500:]);p.check_returncode()
subprocess.run([str(o/'probe.exe'),str(r/'Zombies Ate My Neighbors.sfc')],env=env,check=True)
