"""ES-DE's systems on the PS5 (frontends/es-de/systems.py): RetroArch's content folders,
each shown as the ES-DE system systems.tsv names, with ES-DE's own name, extensions and
theme for it."""
from pathlib import Path
import importlib.util
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parent.parent
PORT = ROOT / 'frontends' / 'es-de'
BUNDLED = ROOT / '.deps' / 'es-de' / 'resources' / 'systems' / 'unix' / 'es_systems.xml'
spec = importlib.util.spec_from_file_location('esde_systems', PORT / 'systems.py')
systems = importlib.util.module_from_spec(spec)
spec.loader.exec_module(systems)

SAMPLE = '''<?xml version="1.0"?>
<systemList>
    <system>
        <name>snes</name>
        <fullname>Nintendo SNES (Super Nintendo)</fullname>
        <path>%ROMPATH%/snes</path>
        <extension>.sfc .SFC .zip .ZIP</extension>
        <command label="Snes9x - Current">%EMULATOR_RETROARCH% -L %CORE_RETROARCH%/snes9x_libretro.so %ROM%</command>
        <platform>snes</platform>
        <theme>snes</theme>
    </system>
    <system>
        <name>cps</name>
        <fullname>Capcom Play System</fullname>
        <path>%ROMPATH%/cps</path>
        <extension>.7z .7Z .zip .ZIP</extension>
        <command label="FinalBurn Neo">%EMULATOR_RETROARCH% %ROM%</command>
        <platform>arcade</platform>
        <theme>cps</theme>
    </system>
</systemList>
'''


class EsdeSystems(unittest.TestCase):
    def write(self, directory, name, text):
        path = Path(directory) / name
        path.write_text(text, encoding='utf-8')
        return path

    def test_folders_become_systems_with_es_de_details(self):
        with tempfile.TemporaryDirectory() as td:
            bundled = self.write(td, 'bundled.xml', SAMPLE)
            table = self.write(td, 'systems.tsv', '# comment\ncps\tARCADE - CPS I, II & III\n'
                               'snes\tNintendo - Super Nintendo Entertainment System\n')
            document = systems.systems_document(bundled, systems.read_table(table))
        self.assertTrue(document.startswith('<?xml version="1.0"?>\n'))
        found = ET.fromstring(document.split('\n', 2)[2]).findall('system')
        self.assertEqual([system.findtext('name') for system in found], ['cps', 'snes'])
        cps = found[0]
        self.assertEqual(cps.findtext('path'), '/app0/content/ARCADE - CPS I, II & III')
        self.assertIn('&amp;', document)
        self.assertEqual(cps.findtext('fullname'), 'Capcom Play System')
        self.assertEqual(cps.findtext('extension'), '.7z .7Z .zip .ZIP')
        self.assertEqual((cps.findtext('platform'), cps.findtext('theme')), ('arcade', 'cps'))
        commands = cps.findall('command')
        self.assertEqual([(c.get('label'), c.text) for c in commands], [('RetroArch', '%EMULATOR_RETROARCH% %ROM%')])

    def test_rejects_unknown_systems_paths_and_duplicates(self):
        with tempfile.TemporaryDirectory() as td:
            bundled = self.write(td, 'bundled.xml', SAMPLE)
            for text in ('nosuch\tFolder\n', 'snes\tsub/folder\n', 'snes\t..\n', 'snes\n',
                         'snes\tA\nsnes\tB\n'):
                table = self.write(td, 'systems.tsv', text)
                with self.subTest(text=text), self.assertRaises(SystemExit):
                    systems.systems_document(bundled, systems.read_table(table))

    def test_every_listed_system_is_one_of_es_de_3_5(self):
        if not BUNDLED.is_file():
            self.skipTest('ES-DE not fetched (tools/build-esde.sh clones it)')
        rows = systems.read_table(PORT / 'systems.tsv')
        document = systems.systems_document(BUNDLED, rows)
        self.assertEqual(document.count('<system>'), len(rows))
        self.assertGreaterEqual(len(rows), 20)


if __name__ == '__main__':
    unittest.main()
