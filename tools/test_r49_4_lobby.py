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
(out/'r494_menu.inc').write_text(net[net.index('static const uint8_t* font_rows'):net.index('static void ipv4_text')])
subprocess.run([str(vc/'bin/Hostx64/x64/cl.exe'),'/nologo','/O2','/I'+str(out),str(root/'tools/test_r49_4_lobby.cpp'),'/Fe'+str(out/'lobby.exe'),'/Fo'+str(out/'lobby.obj')],env=env,cwd=out,check=True)
subprocess.run([str(out/'lobby.exe'),str(root/'work/r493-tests/title-hidden.ppm')],cwd=out,check=True)
