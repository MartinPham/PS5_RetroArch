"""Real HTTP requests against the native WebUI handlers and streaming parser."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest
from urllib.parse import quote

ROOT = Path(__file__).resolve().parent.parent


class WebUI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = subprocess.check_output(['bash', 'tools/build-webui-http.sh', 'host'], cwd=ROOT, text=True).strip()
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        for name in ('config', 'content', 'webui'):
            (cls.root / name).mkdir()
        (cls.root / 'webui/index.html').write_text('<!doctype html><title>RetroArch</title>')
        cls.original = b'audio_volume = "-6"\ninput_rumble_gain = "75"\nunrelated = "preserve"\n'
        (cls.root / 'config/retroarch.cfg').write_bytes(cls.original)
        cls.binary = cls.root / 'server'
        subprocess.run(['c++', '-std=c++17', '-O1', '-g', '-pthread',
                        '-I'+str(ROOT / '.deps/webui/libmicrohttpd-1.0.10/src/include'),
                        'tests/webui_server_main.cpp', 'src/webui_ps5.cpp', archive, '-o', str(cls.binary)], cwd=ROOT, check=True)
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0)); cls.port = s.getsockname()[1]
        cls.process = subprocess.Popen([str(cls.binary), str(cls.root), str(cls.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                with socket.create_connection(('127.0.0.1', cls.port), timeout=.1): break
            except OSError:
                if cls.process.poll() is not None: raise RuntimeError('HTTP server exited')
                time.sleep(.02)
        else: raise RuntimeError('HTTP server did not start')
        _, _, body = cls.request('GET', '/api/status')
        cls.token = json.loads(body)['token']

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate(); cls.process.wait(timeout=5)
        cls.temp.cleanup()

    @classmethod
    def request(cls, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection('127.0.0.1', cls.port, timeout=5)
        fields = {'X-RetroArch-Token': getattr(cls, 'token', '')}
        fields.update(headers or {})
        conn.request(method, path, body, fields)
        response = conn.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        conn.close()
        return result

    def test_origin_session_and_assets(self):
        status, headers, body = self.request('GET', '/')
        self.assertEqual(status, 200)
        self.assertIn(b'RetroArch', body)
        self.assertIn("frame-ancestors 'none'", headers['Content-Security-Policy'])
        self.assertEqual(self.request('GET', '/api/status', headers={'Host': 'attacker.example'})[0], 403)
        self.assertEqual(self.request('POST', '/api/folder?path=nope', headers={'Origin': 'https://attacker.example'})[0], 403)
        self.assertEqual(self.request('POST', '/api/folder?path=nope', headers={'X-RetroArch-Token': 'wrong'})[0], 403)
        self.assertEqual(self.request('GET', '/config/retroarch.cfg')[0], 404)

    def test_upload_download_and_collision(self):
        self.assertEqual(self.request('POST', '/api/folder?path=PSP')[0], 201)
        content = bytes(range(256)) * 8192
        path = '/api/upload?path=' + quote('PSP/test & game.iso')
        status, _, body = self.request('PUT', path, content)
        self.assertEqual(status, 201, body)
        self.assertEqual((self.root / 'content/PSP/test & game.iso').read_bytes(), content)
        self.assertEqual(self.request('PUT', path, b'replace')[0], 409)
        self.assertEqual(self.request('GET', path.replace('upload', 'download'))[2], content)
        listing = json.loads(self.request('GET', '/api/content?path=PSP')[2])
        self.assertEqual(listing['entries'][0]['name'], 'test & game.iso')
        self.assertEqual(listing['entries'][0]['size'], len(content))

    def test_traversal_symlinks_and_hidden_files(self):
        (self.root / 'content/escape').symlink_to(self.root / 'config', target_is_directory=True)
        for path in ('../config/retroarch.cfg', '/etc/passwd', 'escape/retroarch.cfg', 'a/../b', '.private'):
            for verb, endpoint, body in [('GET', 'content', None), ('PUT', 'upload', b'bad')]:
                status = self.request(verb, '/api/'+endpoint+'?path='+quote(path, safe=''), body)[0]
                self.assertIn(status, (400, 409), path)
        self.assertEqual((self.root / 'config/retroarch.cfg').read_bytes(), self.original)
        self.assertNotIn('escape', [e['name'] for e in json.loads(self.request('GET', '/api/content')[2])['entries']])

    def test_settings_preserve_original_and_validate(self):
        fields = {s['key']: s['value'] for s in json.loads(self.request('GET', '/api/settings')[2])['settings']}
        self.assertEqual(fields['audio_volume'], '-6')
        self.assertEqual(self.request('POST', '/api/settings', b'audio_volume=-12\ninput_rumble_gain=50')[0], 200)
        saved = (self.root / 'config/webui.cfg').read_bytes()
        self.assertIn(b'audio_volume = "-12"', saved)
        self.assertNotIn(b'video_vsync', saved)
        self.assertEqual((self.root / 'config/retroarch.cfg').read_bytes(), self.original)
        for bad in (b'audio_volume=999', b'input_rumble_gain=-1', b'menu_driver=evil', b'savefile_directory=/tmp', b'video_vsync=perhaps'):
            self.assertEqual(self.request('POST', '/api/settings', bad)[0], 400)
            self.assertEqual((self.root / 'config/webui.cfg').read_bytes(), saved)
        self.assertEqual(self.request('POST', '/api/settings', b'a'*17000)[0], 413)
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=5)
        conn.request('POST', '/api/settings', [b'a'*9000, b'b'*9000],
                     {'X-RetroArch-Token': self.token}, encode_chunked=True)
        self.assertEqual(conn.getresponse().status, 413)
        conn.close()
        self.assertEqual((self.root / 'config/webui.cfg').read_bytes(), saved)

    def test_interrupted_upload_is_removed(self):
        with socket.create_connection(('127.0.0.1', self.port)) as s:
            request = (f'PUT /api/upload?path=unfinished.iso HTTP/1.1\r\nHost: 127.0.0.1:{self.port}\r\n'
                       f'X-RetroArch-Token: {self.token}\r\nContent-Length: 1000000\r\n\r\n').encode()
            s.sendall(request + b'a'*1000)
            time.sleep(.05)
        for _ in range(100):
            if not list((self.root / 'content').glob('.upload-*')): break
            time.sleep(.02)
        self.assertFalse(list((self.root / 'content').glob('.upload-*')))
        self.assertFalse((self.root / 'content/unfinished.iso').exists())
