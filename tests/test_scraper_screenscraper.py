"""ScreenScraper as a scraping source (src/scraper.cpp) on the host, against a fake
ScreenScraper API: signing in (checked with the source, kept on the console 0600, the
password never in an answer), a job matched by checksum, by name and by search, with
details in the language asked for, the account's thread limit kept, an ambiguous game
resolved, a quota that pauses the job until it is resumed, PC mode refused (the media
links carry the developer account), and the sign-in form in a browser. The developer account is a fake one
(PS5_SCRAPER_TEST_DEVID, read by host builds only)."""
from pathlib import Path
import http.client
import http.server
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_scraper as base  # noqa: E402
import webui_build  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DB = 'Nintendo - Super Nintendo Entertainment System'
DEV_ID, DEV_PASSWORD = 'fake-dev', 'fake-dev-secret'
USER, PASSWORD = 'tester', 'user-secret-Pw9'
SNES = '4'
# The fake API's games: id -> (names by region, the checksum and file name it knows them by).
GAMES = {
    '1001': ({'us': 'Super Metroid', 'jp': 'Super Metroid (JP)'}, 'D63ED5F8', ''),
    '1002': ({'us': 'Chrono Trigger', 'jp': 'Chrono Trigger (JP)'}, '', ''),
    '1003': ({'wor': 'Chrono Trigger: Jet Bike Special'}, '', ''),
    '1004': ({'us': 'Donkey Kong Country 2'}, '', "Donkey Kong Country 2 - Diddy's Kong Quest (USA).zip"),
}
TYPES = {'box-2D': 'png', 'ss': 'png', 'video-normalized': 'mp4', 'wheel-hd': 'png'}


def media_bytes(game, kind, region):
    return f'{kind}/{region}/{game}'.encode() * 40


def game_json(game_id, base_url):
    names, _, _ = GAMES[game_id]
    medias = []
    for kind, fmt in TYPES.items():
        for region in ('jp', 'us'):
            query = urllib.parse.urlencode({'id': game_id, 'media': kind, 'region': region, 'devid': DEV_ID,
                                            'devpassword': DEV_PASSWORD, 'ssid': USER, 'sspassword': PASSWORD})
            medias.append({'type': kind, 'region': region, 'format': fmt, 'url': f'{base_url}/medias.php?{query}'})
    return {'id': game_id, 'noms': [{'region': r, 'text': n} for r, n in names.items()],
            'synopsis': [{'langue': 'en', 'text': f'English story {game_id}'}, {'langue': 'fr', 'text': f'Histoire {game_id}'}],
            'developpeur': {'text': 'Studio'}, 'editeur': {'text': 'Publisher'}, 'joueurs': {'text': '1-2'},
            'note': {'text': '17'}, 'dates': [{'region': 'jp', 'text': '1994-03-19'}, {'region': 'us', 'text': '1994-04-18'}],
            'genres': [{'noms': [{'langue': 'en', 'text': 'Action'}, {'langue': 'fr', 'text': 'Action FR'}]}],
            'medias': medias}


class FakeScreenScraper(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(('127.0.0.1', 0), Handler)
        self.lock = threading.Lock()
        self.reset()

    def reset(self):
        self.quota_hit = False
        self.calls, self.urls, self.busy, self.most_busy, self.max_threads = [], [], 0, 0, '2'


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, body, kind='application/json'):
        data = body if isinstance(body, bytes) else (json.dumps(body) if isinstance(body, dict) else body).encode()
        self.send_response(status); self.send_header('Content-Type', kind); self.send_header('Content-Length', str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(url.query))
        endpoint = url.path.rsplit('/', 1)[-1]
        server = self.server
        with server.lock:
            server.calls.append((endpoint, q)); server.urls.append(self.path)
            server.busy += 1; server.most_busy = max(server.most_busy, server.busy)
        try:
            time.sleep(0.05)
            if (q.get('devid'), q.get('devpassword')) != (DEV_ID, DEV_PASSWORD):
                return self.reply(403, 'Erreur de login : Vérifier vos identifiants développeur !', 'text/plain')
            if endpoint == 'medias.php':
                return self.reply(200, media_bytes(q['id'], q['media'], q['region']), 'image/png')
            if q.get('ssid') != USER or q.get('sspassword') != PASSWORD:
                return self.reply(403, 'Erreur de login : Vérifier vos identifiants utilisateur !', 'text/plain')
            if server.quota_hit and endpoint != 'ssuserInfos.php':
                return self.reply(430, 'Votre quota de scrape est dépassé pour aujourd\'hui !', 'text/plain')
            user = {'id': USER, 'niveau': '1', 'maxthreads': server.max_threads, 'requeststoday': '12', 'maxrequestsperday': '20000'}
            if endpoint == 'ssuserInfos.php':
                return self.reply(200, {'response': {'ssuser': user}})
            base_url = f'http://127.0.0.1:{server.server_port}'
            if q.get('systemeid') != SNES:
                return self.reply(400, 'Erreur : systeme inconnu', 'text/plain')
            if endpoint == 'jeuInfos.php':
                found = q.get('gameid') if q.get('gameid') in GAMES else next(
                    (i for i, (_, crc, rom) in GAMES.items() if (crc and q.get('crc', '').upper() == crc) or (rom and q.get('romnom') == rom)), None)
                if not found:
                    return self.reply(404, 'Erreur : Rom/Iso/Dossier non trouvée !', 'text/plain')
                return self.reply(200, {'response': {'ssuser': user, 'jeu': game_json(found, base_url)}})
            if endpoint == 'jeuRecherche.php':
                words = q.get('recherche', '').lower().split()
                hits = [game_json(i, base_url) for i, (names, _, _) in GAMES.items()
                        if any(all(w in n.lower() for w in words) for n in names.values())]
                return self.reply(200, {'response': {'ssuser': user, 'jeux': hits}})
            return self.reply(404, 'Erreur', 'text/plain')
        finally:
            with server.lock:
                server.busy -= 1


class ScreenScraper(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.binary = webui_build.build(Path(cls.temp.name) / 'server', 'tests/webui_server_main.cpp')
        cls.source = FakeScreenScraper()
        threading.Thread(target=cls.source.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.source.shutdown()
        cls.temp.cleanup()

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(dir=self.temp.name))
        for folder in ('config', 'content/SNES', 'webui', 'cores', 'info', 'playlists'):
            (self.root / folder).mkdir(parents=True)
        (self.root / 'webui/index.html').write_text('<!doctype html><title>RetroArch</title>')
        (self.root / 'cores/snes9x_libretro.so').write_bytes(b'')
        (self.root / 'info/snes9x_libretro.info').write_text(
            'display_name = "Snes9x"\ncorename = "Snes9x"\ndatabase = "' + DB + '"\nsupported_extensions = "sfc|smc|zip"\n')
        self.games = {  # label -> (path, crc)
            'Super Metroid (Japan, USA) (En,Ja)': ('/app0/content/SNES/Super Metroid (Japan, USA) (En,Ja).sfc', 'D63ED5F8|crc'),  # by checksum
            "Donkey Kong Country 2 - Diddy's Kong Quest (USA)": ("/app0/content/SNES/Donkey Kong Country 2 - Diddy's Kong Quest (USA).zip", ''),  # by file name
            'Chrono Trigger (1995)': ('/app0/content/SNES/Chrono Trigger (1995).sfc', ''),  # by search, the same title
            'Chrono': ('/app0/content/SNES/Chrono.sfc', ''),  # search: two titles, ambiguous
            'Totally Unknown Homebrew': ('/app0/content/SNES/Totally Unknown Homebrew.sfc', ''),  # nowhere
        }
        for path, _ in self.games.values():  # the files, so their sizes are sent
            (self.root / path.replace('/app0/', '')).write_bytes(b'rom' * 100)
        items = [{'path': p, 'label': l, 'core_path': 'DETECT', 'core_name': 'DETECT', 'crc32': c, 'db_name': DB + '.lpl'}
                 for l, (p, c) in self.games.items()]
        (self.root / f'playlists/{DB}.lpl').write_text(json.dumps({'version': '1.5', 'items': items}))
        with self.source.lock:
            self.source.reset()
        self.start_server()

    def start_server(self, developer=True):
        self.port = base.free_port()
        env = {**os.environ, 'PS5_SCRAPER_SCREENSCRAPER_BASE': f'http://127.0.0.1:{self.source.server_port}',
               'PS5_SCRAPER_LIBRETRO_BASE': 'http://127.0.0.1:9'}
        if developer:
            env.update(PS5_SCRAPER_TEST_DEVID=DEV_ID, PS5_SCRAPER_TEST_DEVPASSWORD=DEV_PASSWORD)
        self.server = subprocess.Popen([str(self.binary), str(self.root), str(self.port)], env=env,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(200):
            try:
                self.token = json.loads(self.request('GET', '/api/status')[1])['token']
                return
            except OSError:
                time.sleep(0.02)
        raise RuntimeError('the server did not start')

    def tearDown(self):
        if self.server.poll() is None:
            self.server.terminate(); self.server.wait(timeout=10)

    def request(self, method, path, body=None):
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=60)
        conn.request(method, path, body, {'X-RetroArch-Token': getattr(self, 'token', '')})
        response = conn.getresponse()
        data = response.read()
        conn.close()
        # No answer of the console ever carries the user's password or the developer's.
        for secret in (PASSWORD, DEV_PASSWORD, DEV_ID):
            self.assertNotIn(secret.encode(), data, f'{method} {path}')
        return response.status, data

    def sign_in(self, user=USER, password=PASSWORD):
        return self.request('POST', '/api/scraper/account?source=screenscraper', f'{user}\n{password}'.encode())

    def source_entry(self):
        settings = json.loads(self.request('GET', '/api/scraper/settings')[1])
        return next(s for s in settings['sources'] if s['id'] == 'screenscraper')

    def start(self, mode='ps5', kinds='cover,screenshot,video', language='fr', details=True, overwrite=False):
        query = f'mode={mode}&source=screenscraper&kinds={kinds}&region=us&language={language}&overwrite={int(overwrite)}&details={int(details)}'
        status, body = self.request('POST', f'/api/scraper/start?{query}', b'snes\t')
        self.assertEqual(status, 201, body)
        return json.loads(body)['job']['id']

    def job(self, job_id):
        return json.loads(self.request('GET', f'/api/scraper/job?id={job_id}')[1])['job']

    def wait(self, job_id, until=lambda j: j['state'] != 'running', timeout=60):
        end = time.time() + timeout
        while time.time() < end:
            job = self.job(job_id)
            if until(job):
                return job
            time.sleep(0.1)
        self.fail(f'the job did not settle: {self.job(job_id)}')

    def key(self, label):
        return Path(self.games[label][0]).stem

    def test_sign_in_is_checked_kept_private_and_forgotten(self):
        self.assertEqual(self.source_entry()['available'], True)
        self.assertEqual(self.source_entry()['signed_in'], False)
        # No job before signing in.
        status, body = self.request('POST', '/api/scraper/start?mode=ps5&source=screenscraper&kinds=cover', b'snes\t')
        self.assertEqual((status, json.loads(body)['error']), (409, 'Sign in to ScreenScraper first.'))
        # A wrong password is refused, and nothing is kept.
        status, body = self.sign_in(password='wrong')
        self.assertEqual(status, 403)
        self.assertIn('did not accept', json.loads(body)['error'])
        self.assertFalse((self.root / 'config/private/screenscraper.cfg').exists())
        # The right one: the name and quotas come back, never the password.
        status, body = self.sign_in()
        self.assertEqual(status, 200, body)
        account = json.loads(body)
        self.assertEqual((account['signed_in'], account['user'], account['max_threads'], account['max_requests']),
                         (True, USER, '2', '20000'))
        self.assertNotIn('password', account)
        self.assertEqual(self.source_entry()['signed_in'], True)
        stored = self.root / 'config/private/screenscraper.cfg'
        self.assertEqual(stat.S_IMODE(stored.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(stored.parent.stat().st_mode), 0o700)
        # The sign-in went to the source in the request, not in the console's address.
        self.assertTrue(any(e == 'ssuserInfos.php' and q.get('ssid') == USER for e, q in self.source.calls))
        # Kept across a restart; forgotten on request.
        self.server.terminate(); self.server.wait(timeout=10)
        self.start_server()
        self.assertEqual(json.loads(self.request('GET', '/api/scraper/account?source=screenscraper')[1])['user'], USER)
        status, body = self.request('DELETE', '/api/scraper/account?source=screenscraper')
        self.assertEqual((status, json.loads(body)['signed_in']), (200, False))
        self.assertFalse(stored.exists())
        # Signing in and out needs the page's token.
        self.token = ''
        self.assertEqual(self.sign_in()[0], 403)
        self.assertFalse(stored.exists())

    def test_a_build_without_the_developer_account_offers_no_screenscraper(self):
        self.server.terminate(); self.server.wait(timeout=10)
        self.start_server(developer=False)
        self.assertEqual(self.source_entry()['available'], False)
        status, body = self.sign_in()
        self.assertEqual((status, json.loads(body)['error']), (403, 'This build has no ScreenScraper developer account.'))
        self.assertFalse(self.source.calls)

    def test_ps5_mode_checksum_name_search_details_and_thread_limit(self):
        self.assertEqual(self.sign_in()[0], 200)
        job_id = self.start()
        job = self.wait(job_id)
        self.assertEqual(job['state'], 'done', job)
        self.assertEqual(job['counts'], {'ambiguous': 1, 'done': 3, 'unmatched': 1})
        self.assertLessEqual(self.source.most_busy, 2 + 3)  # two games at once (and their media downloads)
        infos = [q for e, q in self.source.calls if e == 'jeuInfos.php']
        self.assertTrue(any(q.get('crc') == 'D63ED5F8' and q.get('romtaille') == '300' for q in infos))
        library = self.root / 'library/snes'
        metroid = self.key('Super Metroid (Japan, USA) (En,Ja)')
        self.assertEqual((library / 'covers' / f'{metroid}.png').read_bytes(), media_bytes('1001', 'box-2D', 'us'))  # the region asked for
        self.assertEqual((library / 'videos' / f'{metroid}.mp4').read_bytes(), media_bytes('1001', 'video-normalized', 'us'))
        meta = (library / 'metadata' / f'{metroid}.meta').read_text()
        for line in ('name = "Super Metroid"', 'description = "Histoire 1001"', 'genre = "Action FR"', 'rating = "0.85"',
                     'released = "1994-04-18"', 'developer = "Studio"', 'players = "1-2"', 'source = "screenscraper"'):
            self.assertIn(line, meta)
        dk = self.key("Donkey Kong Country 2 - Diddy's Kong Quest (USA)")
        self.assertTrue((library / 'covers' / f'{dk}.png').exists())
        self.assertEqual((library / 'covers' / f'{self.key("Chrono Trigger (1995)")}.png').read_bytes(),
                         media_bytes('1002', 'box-2D', 'us'))
        # Nothing on the console's disk holds the user's password but its own account file.
        for path in self.root.rglob('*'):
            if path.is_file() and path.name != 'screenscraper.cfg':
                self.assertNotIn(PASSWORD.encode(), path.read_bytes(), str(path))
        # The recap: what each game got, per kind, and what is missing.
        recap = json.loads(self.request('GET', f'/api/scraper/recap?id={job_id}')[1])['recap']
        self.assertEqual(recap['totals'], {k: {'got': 3, 'had': 0, 'missed': 2} for k in ('cover', 'screenshot', 'video', 'details')})
        self.assertEqual(recap['kinds'], ['cover', 'screenshot', 'video', 'details'])
        games = {g['label']: g for g in recap['systems'][0]['games']}
        self.assertEqual(games['Super Metroid (Japan, USA) (En,Ja)']['got'], ['cover', 'screenshot', 'video', 'details'])
        self.assertEqual(games['Super Metroid (Japan, USA) (En,Ja)']['matched'], 'Super Metroid')
        self.assertEqual((games['Totally Unknown Homebrew']['state'], games['Totally Unknown Homebrew']['missed']),
                         ('unmatched', ['cover', 'screenshot', 'video', 'details']))
        # The ambiguous game: two titles offered as "Name [id]", one chosen.
        ambiguous = next(p for p in job['problems'] if p['state'] == 'ambiguous')
        self.assertEqual(sorted(ambiguous['candidates']), ['Chrono Trigger [1002]', 'Chrono Trigger: Jet Bike Special [1003]'])
        status, body = self.request('POST', f'/api/scraper/resolve?id={job_id}&item={ambiguous["item"]}&action=choose&value='
                                    + urllib.parse.quote('Chrono Trigger: Jet Bike Special [1003]'))
        self.assertEqual(status, 200, body)
        self.wait(job_id, lambda j: j['state'] == 'done' and not j['counts'].get('pending') and not j['counts'].get('working'))
        self.assertEqual((library / 'covers' / f'{self.key("Chrono")}.png').read_bytes(), media_bytes('1003', 'box-2D', 'us'))
        # A search from the page, by the user's words.
        unmatched = next(p for p in job['problems'] if p['state'] == 'unmatched')
        status, body = self.request('POST', f'/api/scraper/resolve?id={job_id}&item={unmatched["item"]}&action=search&value=metroid')
        found = next(p for p in json.loads(body)['job']['problems'] if p['item'] == unmatched['item'])
        self.assertEqual(found['candidates'], ['Super Metroid [1001]'])
        # One game at a time when that is all the account may do.
        with self.source.lock:
            self.source.reset(); self.source.max_threads = '1'
        self.sign_in()
        shutil.rmtree(library / 'covers')
        second = self.wait(self.start(kinds='cover,screenshot'))
        self.assertEqual(self.source.most_busy, 1)
        # Covers fetched again, screenshots kept: the recap tells them apart (the ambiguous
        # game is asked again in a new job: its screenshot kept, its cover missing).
        totals = json.loads(self.request('GET', f'/api/scraper/recap?id={second["id"]}')[1])['recap']['totals']
        self.assertEqual(totals['cover'], {'got': 3, 'had': 0, 'missed': 2})
        self.assertEqual(totals['screenshot'], {'got': 0, 'had': 4, 'missed': 1})

    def test_details_alone_or_media_alone(self):
        self.assertEqual(self.sign_in()[0], 200)
        library = self.root / 'library/snes'
        metroid = self.key('Super Metroid (Japan, USA) (En,Ja)')
        # Media without details: the pictures, a name, no description.
        self.wait(self.start(kinds='cover', details=False))
        self.assertTrue((library / 'covers' / f'{metroid}.png').exists())
        meta = (library / 'metadata' / f'{metroid}.meta').read_text()
        self.assertIn('name = "Super Metroid"', meta)
        self.assertNotIn('description', meta)
        # Details alone: no kind of media at all, the details filled, no media fetched.
        with self.source.lock:
            self.source.calls.clear()
        job = self.wait(self.start(kinds='', details=True))
        self.assertEqual(job['counts'], {'ambiguous': 1, 'done': 3, 'unmatched': 1})
        self.assertIn('description = "Histoire 1001"', (library / 'metadata' / f'{metroid}.meta').read_text())
        self.assertFalse(any(e == 'medias.php' for e, _ in self.source.calls))
        recap = json.loads(self.request('GET', f'/api/scraper/recap?id={job["id"]}')[1])['recap']
        self.assertEqual((recap['kinds'], recap['totals']['details']), (['details'], {'got': 3, 'had': 0, 'missed': 2}))
        # Again: details already there are kept (nothing to ask), unless replacing.
        again = self.wait(self.start(kinds='', details=True))
        self.assertEqual(again['counts'].get('skipped'), 3)
        # A hand-edited description survives replacing everything.
        path = library / 'metadata' / f'{metroid}.meta'
        path.write_text(path.read_text().replace('Histoire 1001', 'Mine') + 'edited = "description"\n')
        self.wait(self.start(kinds='', details=True, overwrite=True))
        self.assertIn('description = "Mine"', path.read_text())
        # Neither: refused.
        status, body = self.request('POST', '/api/scraper/start?mode=ps5&source=screenscraper&kinds=&details=0', b'snes\t')
        self.assertEqual((status, json.loads(body)['error']), (409, 'Choose at least one kind of media, or the games\' details.'))

    def test_a_quota_pauses_the_job_until_resumed(self):
        self.assertEqual(self.sign_in()[0], 200)
        with self.source.lock:
            self.source.quota_hit = True
        job_id = self.start(kinds='cover')
        job = self.wait(job_id, lambda j: j['state'] == 'paused')
        self.assertIn('quota for today is used up', job['message'])
        self.assertEqual(job['counts'].get('pending'), 5)
        with self.source.lock:
            self.source.quota_hit = False
        self.assertEqual(self.request('POST', f'/api/scraper/resume?id={job_id}')[0], 200)
        job = self.wait(job_id)
        self.assertEqual(job['counts'], {'ambiguous': 1, 'done': 3, 'unmatched': 1})

    def test_pc_mode_is_refused_so_no_account_leaves_the_console(self):
        # ScreenScraper's media links carry the developer account: no helper may get them.
        self.assertEqual(self.sign_in()[0], 200)
        status, body = self.request('POST', '/api/scraper/start?mode=pc&source=screenscraper&kinds=cover', b'snes\t')
        self.assertEqual(status, 409)
        self.assertIn('run on the PS5 only', json.loads(body)['error'])
        self.assertFalse(any(e == 'jeuInfos.php' for e, _ in self.source.calls))

    def test_sign_in_form_in_a_browser(self):
        playwright = next((str(p) for p in (Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright-core',
                                            Path('/usr/lib/chatgpt/resources/cua_node/lib/node_modules/playwright-core')) if p.exists()),
                          os.environ.get('PLAYWRIGHT_PATH', ''))
        if not playwright or not Path('/usr/bin/chromium').exists():
            self.skipTest('needs Playwright and Chromium')
        shutil.copytree(ROOT / 'webui', self.root / 'webui', dirs_exist_ok=True)
        run = subprocess.run(['node', 'tests/webui_screenscraper_browser.cjs'], cwd=ROOT, capture_output=True, text=True, timeout=300,
                             env={**os.environ, 'PLAYWRIGHT_PATH': playwright, 'WEBUI_TEST_URL': f'http://127.0.0.1:{self.port}',
                                  'SS_USER': USER, 'SS_PASSWORD': PASSWORD})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(json.loads(self.request('GET', '/api/scraper/account?source=screenscraper')[1])['signed_in'], False)


if __name__ == '__main__':
    unittest.main()
