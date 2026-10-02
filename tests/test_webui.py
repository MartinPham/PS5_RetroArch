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

    def test_advanced_global_and_per_core_persistence(self):
        # Full saved profiles, including values not in the quick-settings list.
        (self.root / 'retroarch.cfg').write_text('video_rotation = "0"\nvideo_vsync = "true"\n')
        profile = self.root / 'config/Example Core'
        profile.mkdir()
        original = b'example_resolution = "6x"\nexample_filter = "nearest"\n'
        (profile / 'Example Core.opt').write_bytes(original)
        (self.root / 'config/not-a-core').symlink_to(self.root / 'content', target_is_directory=True)
        cores = json.loads(self.request('GET', '/api/cores')[2])['cores']
        self.assertIn('Example Core', cores)
        self.assertNotIn('not-a-core', cores)
        def read(url):
            status, _, body = self.request('GET', url)
            self.assertEqual(status, 200, body)
            return json.loads(body)
        def save(url, body, revision):
            return self.request('POST', url, body, {'X-RetroArch-Revision': revision})[0]
        global_url = '/api/config?scope=global'
        state = read(global_url)
        self.assertEqual(save(global_url, b'video_rotation=2', state['revision']), 200)
        self.assertEqual(save(global_url, b'video_rotation=3', state['revision']), 409)
        for invalid in (b'new_arbitrary_key=1', b'audio_volume=abc', b'video_vsync=yes', b'video_rotation=2"\ninjected=1', b'audio_volume=999'):
            self.assertEqual(save(global_url, invalid, read(global_url)['revision']), 400)
        # A quick setting must retain the advanced override.
        self.assertEqual(self.request('POST', '/api/settings', b'input_rumble_gain=75')[0], 200)
        self.assertIn(b'video_rotation = "2"', (self.root / 'config/webui.cfg').read_bytes())
        # Unknown config fields and core enums remain strings across edits.
        self.assertEqual(save(global_url, b'unrelated=1', read(global_url)['revision']), 200)
        self.assertEqual(save(global_url, b'unrelated=preserve', read(global_url)['revision']), 200)
        options_url = '/api/config?scope=core-options&core=Example%20Core'
        self.assertEqual(save(options_url, b'example_resolution=1', read(options_url)['revision']), 200)
        self.assertEqual(save(options_url, b'example_resolution=4x', read(options_url)['revision']), 200)
        self.assertEqual((profile / 'Example Core.opt').read_bytes(), original, 'Running core profile is untouched')
        override_url = '/api/config?scope=core-settings&core=Example%20Core'
        self.assertEqual(save(override_url, b'video_vsync=false', read(override_url)['revision']), 200)
        self.assertNotEqual(next(x['value'] for x in read(global_url)['settings'] if x['key'] == 'video_vsync'), 'false')
        self.assertEqual(self.request('GET', '/api/config?scope=core-options&core=..%2Fconfig')[0], 404)
        # Simulate the current core saving on exit after the browser edit.
        (profile / 'Example Core.opt').write_bytes(original)
        cls = self.__class__
        cls.process.terminate(); cls.process.wait(timeout=5)
        cls.process = subprocess.Popen([str(cls.binary), str(cls.root), str(cls.port)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                status, _, body = cls.request('GET', '/api/status')
                if status == 200: break
            except OSError: pass
            time.sleep(.02)
        else: self.fail('Server did not restart')
        cls.token = json.loads(body)['token']
        self.assertIn(b'example_resolution = "4x"', (profile / 'Example Core.opt').read_bytes())
        self.assertIn(b'example_filter = "nearest"', (profile / 'Example Core.opt').read_bytes())
        self.assertIn(b'video_vsync = "false"', (profile / 'Example Core.cfg').read_bytes())
        self.assertFalse((self.root / 'config/webui-cores/Example Core.opt').exists())
        self.assertFalse((self.root / 'config/webui-cores/Example Core.cfg').exists())
        self.assertEqual((self.root / 'config/retroarch.cfg').read_bytes(), self.original)
        # Leave the shared fixture's original quick-settings baseline intact.
        (self.root / 'config/webui.cfg').unlink()

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
