"""Isolated native HTTP fixture for webui_update_browser.cjs (port 6770).
Run after the host tests and a title build; no console access or owner data.
"""
from pathlib import Path
import shutil, subprocess, json
import tempfile, os
ROOT = Path(__file__).resolve().parent.parent
os.chdir(ROOT)
work = tempfile.TemporaryDirectory(prefix='retroarch-webui-fixture-')
r = Path(work.name)
for folder in ['webui','webui/core-metadata','cores','info','system','config','content/PSP','content/Arcade','content/Saturn','config/PPSSPP']:(r/folder).mkdir(parents=True,exist_ok=True)
shutil.copytree('webui',r/'webui',dirs_exist_ok=True);shutil.copytree('build/webui-core-metadata',r/'webui/core-metadata',dirs_exist_ok=True)
(r/'webui/version.json').write_text(json.dumps({'release':'v0.5.7-alpha.5','build':'browser-fixture'}))
(r/'config/retroarch.cfg').write_text('audio_volume = "0"\ninput_rumble_gain = "100"\n')
for file in Path('dist/PPSA99169/cores').glob('*.so'):(r/'cores'/file.name).touch()
shutil.copytree('dist/PPSA99169/info',r/'info',dirs_exist_ok=True)
subprocess.run(['c++','-std=c++17','-pthread','-I.deps/webui/libmicrohttpd-1.0.10/src/include','-I.deps/native/zlib/zlib-1.3.2/contrib/minizip','-Ivendor/retroarch/deps/mbedtls','tests/webui_server_main.cpp','src/webui_ps5.cpp','src/webui_transfer.cpp','src/webui_update.cpp','build/webui-mhd-host/src/microhttpd/.libs/libmicrohttpd.a','build/webui-update-host/libupdate.a','-lz','-o',str(r/'server')],check=True)
try:
    subprocess.run([str(r/'server'),str(r),'6770'],check=True)
finally:
    work.cleanup()
