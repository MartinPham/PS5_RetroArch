"""The WebUI daemon (daemon/webui_daemon.cpp) on the host: linked by the title's
programs as src/webui_link.cpp links, it serves the title's folder, names the frontend,
keeps an upload going while the title changes frontend (one program's link closes,
the next one's opens), refuses another build's programs, and stops once the title has
closed. Its folder, ports and idle time are the test's."""
from pathlib import Path
import json
import os
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
TITLE = 'PPSA99169'
IDLE = 2


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def wait_for(predicate, timeout=10):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return True
        time.sleep(0.05)
    return False


class WebuiDaemon(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        http = subprocess.check_output(['bash', 'tools/build-webui-http.sh', 'host'], cwd=ROOT, text=True).strip()
        update = subprocess.check_output(['bash', 'tools/build-webui-update.sh', 'host'], cwd=ROOT, text=True).strip()
        cls.temp = tempfile.TemporaryDirectory()
        cls.homebrew = Path(cls.temp.name)
        cls.link_port, cls.http_port = free_port(), free_port()
        cls.binary = cls.homebrew / 'daemon'
        subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-pthread', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / '.deps/webui/libmicrohttpd-1.0.10/src/include'),
                        '-I' + str(ROOT / '.deps/native/zlib/zlib-1.3.2/contrib/minizip'),
                        '-I' + str(ROOT / 'vendor/retroarch/deps/mbedtls'),
                        f'-DPS5_WEBUI_HOMEBREW="{cls.homebrew}"', f'-DPS5_WEBUI_LINK_PORT={cls.link_port}',
                        f'-DPS5_WEBUI_HTTP_PORT={cls.http_port}', f'-DPS5_WEBUI_IDLE_SECONDS={IDLE}',
                        'daemon/webui_daemon.cpp', 'src/webui_ps5.cpp', 'src/webui_update.cpp', http, update,
                        '-lz', '-o', str(cls.binary)], cwd=ROOT, check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.root = self.homebrew / TITLE
        for name in ('config', 'content', 'webui', 'sce_sys'):
            (self.root / name).mkdir(parents=True, exist_ok=True)
        (self.root / 'eboot.bin').write_bytes(b'eboot')
        (self.root / 'webui/index.html').write_text('<!doctype html><title>RetroArch</title>')
        (self.root / 'webui/version.json').write_text('{"build": "one"}\n')
        (self.root / 'sce_sys/param.json').write_text(json.dumps({'titleId': TITLE}))
        self.daemon = subprocess.Popen([str(self.binary)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.addCleanup(self.stop)
        self.assertTrue(wait_for(self.listening), 'the daemon did not listen for links')

    def stop(self):
        if self.daemon.poll() is None:
            self.daemon.kill()
        self.daemon.wait()
        self.daemon.stderr.close()

    def listening(self):
        try:
            socket.create_connection(('127.0.0.1', self.link_port), timeout=0.2).close()
            return True
        except OSError:
            return False

    def hello(self, frontend, title=TITLE):
        link = socket.create_connection(('127.0.0.1', self.link_port), timeout=5)
        link.sendall(f'hello {frontend} {title}\n'.encode())
        reply = b''
        while not reply.endswith(b'\n'):
            chunk = link.recv(1)
            if not chunk:
                break
            reply += chunk
        return link, reply.decode().strip()

    def status(self):
        with socket.create_connection(('127.0.0.1', self.http_port), timeout=5) as s:
            s.sendall(f'GET /api/status HTTP/1.0\r\nHost: 127.0.0.1:{self.http_port}\r\n\r\n'.encode())
            data = b''
            while chunk := s.recv(65536):
                data += chunk
        return json.loads(data.split(b'\r\n\r\n', 1)[1])

    def serving(self):
        try:
            return self.status()
        except (OSError, ValueError):
            return None

    def test_upload_continues_across_a_frontend_change(self):
        picker, reply = self.hello('picker')
        self.assertEqual(reply, 'ok')
        self.assertTrue(wait_for(self.serving))
        first = self.status()
        self.assertEqual(first['frontend'], 'picker')

        # An upload starts while the picker shows ...
        payload = os.urandom(3 * 1024 * 1024)
        upload = socket.create_connection(('127.0.0.1', self.http_port), timeout=20)
        upload.sendall((f'PUT /api/upload?path=handover.bin HTTP/1.1\r\nHost: 127.0.0.1:{self.http_port}\r\n'
                        f'X-RetroArch-Token: {first["token"]}\r\nContent-Length: {len(payload)}\r\n\r\n').encode())
        upload.sendall(payload[:len(payload) // 2])
        # ... the picker hands over (LoadExec ends it: its link closes) ...
        picker.close()
        self.assertTrue(wait_for(lambda: self.status()['frontend'] == ''))
        time.sleep(1)
        # ... and EmulationStation links in its place, as it starts.
        es_de, reply = self.hello('es-de')
        self.assertEqual(reply, 'ok')
        upload.sendall(payload[len(payload) // 2:])
        response = b''
        while b'\r\n\r\n' not in response:
            chunk = upload.recv(65536)
            if not chunk:
                break
            response += chunk
        upload.close()
        self.assertRegex(response.split(b'\r\n', 1)[0].decode(), r'HTTP/1\.1 20[01]')
        self.assertEqual((self.root / 'content/handover.bin').read_bytes(), payload)
        after = self.status()
        self.assertEqual((after['frontend'], after['token']), ('es-de', first['token']))
        self.assertEqual(self.daemon.poll(), None)

        # A frontend change within the program is announced on its link.
        es_de.sendall(b'frontend retroarch\n')
        self.assertTrue(wait_for(lambda: self.status()['frontend'] == 'retroarch'))

        # The title closes: no program linked, and the daemon stops after the idle time.
        es_de.close()
        closed = time.monotonic()
        self.assertEqual(self.daemon.wait(timeout=IDLE + 10), 0)
        self.assertGreaterEqual(time.monotonic() - closed, IDLE - 0.5)
        log = (self.root / 'webui-daemon.txt').read_text()
        for line in ('linked: picker', 'unlinked: picker', 'linked: es-de', 'frontend: retroarch',
                     'the title is closed', 'stopped'):
            self.assertIn(line, log)

    def test_other_build_and_other_title(self):
        first, reply = self.hello('picker')
        self.assertEqual(reply, 'ok')
        for title, expected in (('PPSA00001', 'other'), ('bad', 'other')):
            link, reply = self.hello('picker', title)
            link.close()
            self.assertEqual(reply, expected)
        # The title is updated: its programs are another build's, and this daemon stops.
        (self.root / 'webui/version.json').write_text('{"build": "two"}\n')
        link, reply = self.hello('retroarch')
        link.close()
        self.assertEqual(reply, 'stale')
        self.assertTrue(wait_for(lambda: not self.listening()), 'a stale daemon keeps its link port')
        first.close()
        self.assertEqual(self.daemon.wait(timeout=10), 0)

    def test_no_title_folder(self):
        link, reply = self.hello('picker', 'PPSA00002')
        link.close()
        self.assertEqual(reply, 'unknown')
        self.assertIsNone(self.serving())


if __name__ == '__main__':
    unittest.main()
