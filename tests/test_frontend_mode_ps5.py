"""Which frontend a launch of eboot.bin starts (src/frontend_mode_ps5.cpp), against a mocked LoadExec."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent


class FrontendModePs5(unittest.TestCase):
    def test_decisions_and_launches(self):
        with tempfile.TemporaryDirectory() as td:
            binary = str(Path(td) / 'frontend-mode-test')
            subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-Isrc',
                            'tests/frontend_mode_ps5_test.cpp', '-o', binary], cwd=ROOT, check=True)
            subprocess.run([binary, td], cwd=ROOT, check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
