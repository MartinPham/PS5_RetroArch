#!/usr/bin/env python3
"""Write the es_systems.xml ES-DE reads on the PS5, from RetroArch's content folders.

    frontends/es-de/systems.py <ES-DE's unix es_systems.xml> <systems.tsv> <output>

Each row of systems.tsv names an ES-DE system, its default core and the RetroArch
content folder it shows. The system's full name, extensions, platform and theme are
ES-DE's own; its path is the folder under /app0/content, where RetroArch keeps the
same games; its one command is RetroArch's command line, which the title's game mode
runs (src/ps5_game.h): "/app0/eboot.bin -L /app0/cores/<core>_libretro.so %ROM%", or
without -L for a system with no core ("-"), whose games RetroArch's playlists must
name a core for. The output replaces ES-DE's bundled file, so ES-DE scans those
folders and no others.
"""
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

CONTENT = '/app0/content'
CORES = '/app0/cores'


def command(core):
    if core == '-':
        return '/app0/eboot.bin %ROM%'
    return f'/app0/eboot.bin -L {CORES}/{core}_libretro.so %ROM%'


def read_table(path):
    rows = []
    for number, line in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        if not line.strip() or line.startswith('#'):
            continue
        fields = line.split('\t')
        if len(fields) != 3 or not all(fields):
            raise SystemExit(f'{path}:{number}: expected "<ES-DE system>\\t<core>\\t<content folder>"')
        name, core, folder = fields
        if '/' in folder or folder in ('.', '..'):
            raise SystemExit(f'{path}:{number}: {folder!r} is not one folder name')
        if core != '-' and not re.fullmatch(r'[a-z0-9_]+', core):
            raise SystemExit(f'{path}:{number}: {core!r} is not a core name')
        rows.append((name, core, folder))
    names = [name for name, _, _ in rows]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise SystemExit(f'{path}: systems named twice: {", ".join(duplicates)}')
    return rows


def systems_document(bundled, rows):
    known = {system.findtext('name'): system for system in ET.parse(bundled).getroot().iter('system')}
    root = ET.Element('systemList')
    for name, core, folder in rows:
        source = known.get(name)
        if source is None:
            raise SystemExit(f'ES-DE has no system named {name!r} ({bundled})')
        system = ET.SubElement(root, 'system')
        ET.SubElement(system, 'name').text = name
        ET.SubElement(system, 'fullname').text = source.findtext('fullname')
        ET.SubElement(system, 'path').text = f'{CONTENT}/{folder}'
        ET.SubElement(system, 'extension').text = source.findtext('extension')
        ET.SubElement(system, 'command', label='RetroArch').text = command(core)
        ET.SubElement(system, 'platform').text = source.findtext('platform')
        ET.SubElement(system, 'theme').text = source.findtext('theme')
    ET.indent(root, space='    ')
    return ('<?xml version="1.0"?>\n'
            '<!-- ES-DE systems on the PS5: RetroArch\'s content folders (frontends/es-de/systems.tsv) -->\n'
            + ET.tostring(root, encoding='unicode') + '\n')


def main(argv):
    if len(argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[2].strip())
    Path(argv[3]).write_text(systems_document(argv[1], read_table(argv[2])), encoding='utf-8')


if __name__ == '__main__':
    main(sys.argv)
