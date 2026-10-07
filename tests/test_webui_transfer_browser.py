"""The page's transfer engine in Chromium (tests/webui_transfer_browser.cjs) against
the native server, then the files on disk compared with what the page made."""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
PLAYWRIGHT = os.environ.get('PLAYWRIGHT_PATH') or next(
    (str(p) for p in (Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright-core',
                      Path('/usr/lib/chatgpt/resources/cua_node/lib/node_modules/playwright-core')) if p.exists()), '')


def pattern(size, seed):
    data = bytearray(size)
    for i in range(0, size, 4096):
        data[i] = (seed + i) & 255
    return bytes(data)


@unittest.skipUnless(PLAYWRIGHT and shutil.which('node') and Path('/usr/bin/chromium').exists(), 'needs node, Playwright and Chromium')
class TransferBrowser(unittest.TestCase):
    def test_page_engine(self):
        http = subprocess.check_output(['bash', 'tools/build-webui-http.sh', 'host'], cwd=ROOT, text=True).strip()
        update = subprocess.check_output(['bash', 'tools/build-webui-update.sh', 'host'], cwd=ROOT, text=True).strip()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'title'
            for name in ('config', 'content', 'webui'):
                (root / name).mkdir(parents=True)
            shutil.copytree(ROOT / 'webui', root / 'webui', dirs_exist_ok=True)
            binary = Path(temp) / 'server'
            subprocess.run(['c++', '-std=c++17', '-O1', '-pthread',
                            '-I' + str(ROOT / '.deps/webui/libmicrohttpd-1.0.10/src/include'),
                            '-I' + str(ROOT / '.deps/native/zlib/zlib-1.3.2/contrib/minizip'),
                            '-I' + str(ROOT / 'vendor/retroarch/deps/mbedtls'),
                            'tests/webui_server_main.cpp', 'src/webui_ps5.cpp', 'src/webui_transfer.cpp',
                            'src/webui_update.cpp', http, update, '-lz', '-o', str(binary)], cwd=ROOT, check=True)
            with socket.socket() as s:
                s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
            server = subprocess.Popen([str(binary), str(root), str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                time.sleep(0.5)
                run = subprocess.run(['node', 'tests/webui_transfer_browser.cjs'], cwd=ROOT, capture_output=True, text=True, timeout=600,
                                     env={**os.environ, 'PLAYWRIGHT_PATH': PLAYWRIGHT, 'WEBUI_TEST_URL': f'http://127.0.0.1:{port}'})
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                print(run.stdout.strip())
            finally:
                server.terminate(); server.wait(timeout=5)
            folder = root / 'content/engine'
            for i in range(600):
                self.assertEqual((folder / f'shot-{i:04}.png').read_bytes(), pattern(1000 + (i * 997) % 60000, i))
            for i in range(3):
                self.assertEqual((folder / f'medium-{i}.bin').read_bytes(), pattern(9 * 1024 * 1024 + i, 7 + i))
            for i in range(2):
                self.assertEqual((folder / f'large-{i}.iso').read_bytes(), pattern(150 * 1024 * 1024 + 4097 * i, 31 + i))
            self.assertFalse((folder / 'cancel.iso').exists())
            self.assertEqual(list(folder.glob('.upload-*')), [])


if __name__ == '__main__':
    unittest.main()
