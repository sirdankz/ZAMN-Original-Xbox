from pathlib import Path
import os,subprocess
root=Path(__file__).resolve().parents[1]
out=root/'work/r494-tests';out.mkdir(parents=True,exist_ok=True)
vc=sorted((Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC').iterdir())[-1]
sdk=Path(os.environ.get('ProgramFiles(x86)',r'C:\Program Files (x86)'))/'Windows Kits/10';ver=sorted((sdk/'Lib').iterdir())[-1].name
env=os.environ.copy();env['INCLUDE']=';'.join(map(str,[vc/'include',sdk/'Include'/ver/'ucrt',sdk/'Include'/ver/'um',sdk/'Include'/ver/'shared']))
env['LIB']=';'.join(map(str,[vc/'lib/x64',sdk/'Lib'/ver/'ucrt/x64',sdk/'Lib'/ver/'um/x64']))
env['PATH']=str(vc/'bin/Hostx64/x64')+';'+env['PATH']
net=(root/'src/platform/xbox/xbox_netplay.cpp').read_text()
def section(a,b):return net[net.index(a):net.index(b,net.index(a))]
(out/'r494_handshake.inc').write_text(section('#define ZNP_AUTO_WINDOW','static bool ipv4_is_private_lan')+section('static uint8_t clamp_delay','static bool is_lan_direct_1f')+section('static void receive_inputs(void)','static bool send_input_history'))
subprocess.run([str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/O2','/std:c++17','/I'+str(out),'/I'+str(root/'src'),str(root/'tools/test_r49_4_handshake.cpp'),str(root/'src/netplay/netplay_core.c'),'/Fe'+str(out/'handshake.exe'),'/Fo'+str(out)+os.sep],env=env,cwd=out,check=True)
subprocess.run([str(out/'handshake.exe')],cwd=out,check=True)
