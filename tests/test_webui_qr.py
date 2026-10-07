"""Decode the real runtime QR and RGUI drawing, using controlled network changes."""
import ctypes as C
import hashlib
import json
import runpy
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QR(C.Structure):
    _fields_ = [('url', C.c_char * 64), ('size', C.c_uint),
                ('modules', C.c_ubyte * (37 * 37)), ('checked', C.c_long)]


class WebUIQR(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'qr.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-std=c11', '-D_DEFAULT_SOURCE', '-Isrc',
                        '-Ithird_party', 'src/ps5_webui_qr.c', 'tests/webui_qr_test.c',
                        '-Wl,--wrap=getifaddrs,--wrap=freeifaddrs,--wrap=time', '-o', str(lib)],
                       cwd=ROOT, check=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.ps5_webui_qr_encode.argtypes = [C.POINTER(QR), C.c_char_p]
        cls.lib.ps5_webui_qr_encode.restype = C.c_bool
        cls.lib.ps5_webui_qr_refresh.argtypes = [C.POINTER(QR), C.c_bool]
        cls.lib.ps5_webui_qr_current.restype = C.POINTER(QR)
        cls.lib.ps5_webui_qr_visible.restype = C.c_bool
        cls.lib.qr_test_network.argtypes = [C.c_char_p, C.c_uint, C.c_int, C.c_long]
        cls.lib.qr_test_rgui.argtypes = [C.POINTER(C.c_uint16), C.c_uint, C.c_uint]
        cls.lib.qr_test_xmb.argtypes = cls.lib.qr_test_rgui.argtypes

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def decode(self, data, width, height):
        self.assertIsNotNone(shutil.which('zbarimg'), 'QR verification requires zbarimg')
        path = Path(self.tmp.name) / 'qr.pgm'
        path.write_bytes(f'P5\n{width} {height}\n255\n'.encode() + data)
        result = subprocess.run(['zbarimg', '--quiet', '--raw', str(path)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        return result.stdout.strip().decode()

    def test_encoder_decodes_and_preserves_quiet_zone(self):
        qr = QR()
        for address in (b'192.0.2.42', b'198.51.100.123', b'203.0.113.254', b'10.0.0.1'):
            self.assertTrue(self.lib.ps5_webui_qr_encode(C.byref(qr), address))
            self.assertLessEqual(qr.size, 37)
            side = qr.size
            self.assertTrue(all(not qr.modules[y * side + x] for y in range(side) for x in range(side)
                                if min(x, y, side - x - 1, side - y - 1) < 4))
            scale = 8
            pixels = bytes(0 if qr.modules[(y // scale) * side + x // scale] else 255
                           for y in range(side * scale) for x in range(side * scale))
            self.assertEqual(self.decode(pixels, side * scale, side * scale), f'http://{address.decode()}:6769/')
        for address in (b'', b'0.0.0.0', b'127.0.0.1', b'224.0.0.1', b'255.255.255.255', b'1.2.3.999', b'bad/path'):
            self.assertFalse(self.lib.ps5_webui_qr_encode(C.byref(qr), address))
            self.assertEqual(qr.size, 0)
            self.assertEqual(qr.url, b'')

    def test_current_ip_changes_offline_and_modal_lifecycle(self):
        lib = self.lib
        lib.qr_test_network(b'192.0.2.42', 1, 0, 100)
        lib.ps5_webui_qr_open()
        self.assertTrue(lib.ps5_webui_qr_visible())
        self.assertEqual(lib.ps5_webui_qr_current().contents.url, b'http://192.0.2.42:6769/')
        lib.qr_test_network(b'198.51.100.123', 1, 0, 101)
        self.assertEqual(lib.ps5_webui_qr_current().contents.url, b'http://198.51.100.123:6769/')
        lib.qr_test_network(b'198.51.100.123', 0, 0, 102)
        self.assertEqual(lib.ps5_webui_qr_current().contents.size, 0)
        lib.qr_test_network(b'198.51.100.123', 1, 1, 103)
        self.assertEqual(lib.ps5_webui_qr_current().contents.url, b'')
        lib.ps5_webui_qr_close()
        self.assertFalse(lib.ps5_webui_qr_visible())
        lib.qr_test_network(b'203.0.113.2', 1, 0, 104)
        lib.ps5_webui_qr_open()
        self.assertEqual(lib.ps5_webui_qr_current().contents.url, b'http://203.0.113.2:6769/')
        lib.ps5_webui_qr_close()

    def test_rgui_rendered_frame_decodes(self):
        self.lib.qr_test_network(b'192.0.2.42', 1, 0, 200)
        self.lib.ps5_webui_qr_open()
        for width, height in ((320, 240), (640, 480)):
            pixels = (C.c_uint16 * (width * height))()
            self.lib.qr_test_rgui(pixels, width, height)
            self.assertEqual(self.decode(bytes(255 if pixel == 0xffff else 0 for pixel in pixels), width, height),
                             'http://192.0.2.42:6769/')
        self.lib.ps5_webui_qr_close()

    def test_xmb_rendered_frame_decodes(self):
        self.lib.qr_test_network(b'198.51.100.123', 1, 0, 300)
        self.lib.ps5_webui_qr_open()
        for width, height in ((1280, 720), (1920, 1080)):
            pixels = (C.c_uint16 * (width * height))()
            self.lib.qr_test_xmb(pixels, width, height)
            self.assertEqual(self.decode(bytes(255 if pixel == 0xffff else 0 for pixel in pixels), width, height),
                             'http://198.51.100.123:6769/')
        self.lib.ps5_webui_qr_close()

    def test_picker_buttons_consume_input(self):
        source = (ROOT / 'frontends/picker/picker.cpp').read_text()
        block = source.split('// Consume the overlay', 1)[1].split('\n', 1)[1].split('\n\t\tif (input.nav', 1)[0]
        test = r"""
#include <cassert>
namespace hui {
enum class Action { north=1, back=2, confirm=4, west=8 };
struct InputFrame { unsigned bits=0; bool is_pressed(Action a) const { return bits & (unsigned)a; } };
}
bool showWebUI=false; int webuiQR=0;
void ps5_webui_qr_refresh(int*,bool) {}
void update(hui::InputFrame &input) {
""" + block + r"""
}
int main() {
 hui::InputFrame i{1}; update(i); assert(showWebUI && !i.bits);
 i.bits=4|8; update(i); assert(showWebUI && !i.bits);
 i.bits=1; update(i); assert(!showWebUI && !i.bits);
 i.bits=1; update(i); assert(showWebUI && !i.bits);
 i.bits=2; update(i); assert(!showWebUI && !i.bits);
 i.bits=2; update(i); assert(!showWebUI && i.bits==2);
}
"""
        file = Path(self.tmp.name) / 'picker-input.cpp'
        file.write_text(test)
        binary = file.with_suffix('')
        subprocess.run(['c++', '-std=c++17', str(file), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

    def test_menu_hooks_apply_and_are_idempotent(self):
        edits = runpy.run_path(str(ROOT / 'tools/apply-port-patches.py'))['EDITS']
        files = {}
        for name, anchor, replacement, marker in edits:
            if name not in files:
                files[name] = (ROOT / 'vendor/retroarch' / name).read_text()
            if marker not in files[name]:
                self.assertIn(anchor, files[name], marker)
                files[name] = files[name].replace(anchor, replacement, 1)
        before = files.copy()
        for name, anchor, replacement, marker in edits:
            if "0115" in replacement and marker not in files[name]:
                files[name] = files[name].replace(anchor, replacement, 1)
        self.assertEqual(before, files)
        xmb = files['menu/drivers/xmb.c']
        cache = xmb.split('static void xmb_list_cache', 1)[1]
        self.assertIn('case XMB_SYSTEM_TAB_WEBUI:', cache)
        tabs = xmb.split('static void xmb_refresh_system_tabs_list', 1)[1].split('static ', 1)[0]
        self.assertLess(tabs.index('XMB_SYSTEM_TAB_WEBUI'), tabs.index('XMB_SYSTEM_TAB_SETTINGS'))
        rgui = files['menu/menu_displaylist.c'].split('case DISPLAYLIST_MAIN_MENU:', 1)[1]
        self.assertLess(rgui.index('"ps5_webui"'), rgui.index('RUNLOOP_FLAG_CORE_RUNNING'))

    def test_upstream_encoder_is_unmodified(self):
        folder = ROOT / 'third_party/qrcodegen'
        for name, digest in json.loads((folder / 'source.json').read_text())['files'].items():
            self.assertEqual(hashlib.sha256((folder / name).read_bytes()).hexdigest(), digest)


if __name__ == '__main__':
    unittest.main()
