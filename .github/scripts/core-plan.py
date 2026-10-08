#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The cores tools/build-title.sh builds, for the workflow's per-core jobs.

  core-plan.py matrix              the core_names list of tools/build-title.sh as a JSON
                                   matrix: [{"core", "script", "stamp", "fork", "revision",
                                   "source"}]; fork and revision only for a core built from a
                                   mihawk-99 fork checkout (tools/core-fork.sh, LRPS2's own)
  core-plan.py prefetch CORE       that fork's pinned commit alone, fetched into the tree the
                                   build script checks out, so it does not clone the whole
                                   history (MAME's is gigabytes); the script still checks it out
                                   at the pin and cleans it
  core-plan.py collect CORE OUT    what the title job needs from a built core, copied into OUT
                                   at the same paths: build/cores/stage (the library, its info
                                   file, its system/ assets), the stamp that lets the title's
                                   own run of the script skip it, the build and ABI reports
                                   stage-notices.py reads, and the core option sources
                                   tools/generate-core-metadata.py extracts the WebUI's
                                   catalogs from

Each core's script is the one whose core_stamp_skip names <core>_libretro.so, the file
build-title.sh stages.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"


def core_names():
    text = (TOOLS / "build-title.sh").read_text()
    found = re.search(r"^core_names=\(([^)]*)\)", text, re.M)
    if not found:
        sys.exit("core-plan: no core_names=(...) in tools/build-title.sh")
    return found.group(1).split()


def plan():
    scripts = sorted(TOOLS.glob("build-*.sh"))
    cores = []
    for core in core_names():
        output = f"/build/cores/stage/cores/{core}_libretro.so"
        matches = []
        for script in scripts:
            text = script.read_text()
            stamp = re.search(r"core_stamp_skip\s+([\w.-]+)((?:\s*\\?\n?\s*\"[^\"]*\")+)", text)
            if stamp and output in stamp.group(2):
                matches.append((script, text, stamp.group(1)))
        if len(matches) != 1:
            sys.exit(f"core-plan: {len(matches)} scripts stage {core}_libretro.so")
        script, text, stamp = matches[0]
        entry = {"core": core, "script": script.name, "stamp": stamp}
        fork = re.search(r"core_fork_checkout\s+(PS5_\w+)\s", text) or \
            re.search(r"github\.com/mihawk-99/(PS5_\w+)\.git", text)
        revision = re.search(r"^revision=([0-9a-f]{40})\b", text, re.M)
        if fork and revision:
            name = re.search(r"^core_name=([\w.-]+)", text, re.M)
            source = re.search(r'source_dir="\$root/(\.deps/[\w.-]+-src)"', text)
            entry.update(fork=fork.group(1), revision=revision.group(1),
                         source=source.group(1) if source else f".deps/{name.group(1)}-src")
        cores.append(entry)
    return cores


def find(core):
    for entry in plan():
        if entry["core"] == core:
            return entry
    sys.exit(f"core-plan: {core} is not in tools/build-title.sh's core_names")


def git(*args, cwd=None):
    subprocess.run(["git", *args], cwd=cwd, check=True)


def prefetch(core):
    entry = find(core)
    if "fork" not in entry:
        print(f"==> [{core}] {entry['script']} fetches its own source")
        return
    tree = ROOT / entry["source"]
    if (tree / ".git").is_dir():
        print(f"==> [{core}] {entry['source']} is already there")
        return
    git("init", "-q", str(tree))
    # origin is the published fork: a relative submodule URL (../PS5_Dynarmic) resolves against it
    git("remote", "add", "origin", f"https://github.com/mihawk-99/{entry['fork']}.git", cwd=tree)
    git("fetch", "-q", "--depth", "1", "--no-recurse-submodules", "origin", entry["revision"], cwd=tree)
    print(f"==> [{core}] {entry['fork']} {entry['revision'][:12]} in {entry['source']}")


def metadata_sources(core):
    """The files generate-core-metadata.py reads for CORE: its source and that folder's others."""
    spec = __import__("importlib.util").util.spec_from_file_location(
        "core_metadata", TOOLS / "generate-core-metadata.py")
    module = __import__("importlib.util").util.module_from_spec(spec)
    spec.loader.exec_module(module)
    files = []
    for stem, _name, source, _flags in module.CORES:
        if stem == core:
            folder = (ROOT / source).parent
            files += [path for path in sorted(folder.iterdir()) if path.is_file()]
    return files


def licence_texts(entry):
    """The licence texts stage-notices.py copies for this core, from its source tree."""
    table = json.loads((ROOT / "tooling/notices/components.json").read_text(encoding="utf-8"))
    paths = []
    for component in table["components"]:
        source = component.get("source", {})
        if source.get("kind") != "core" or source.get("build") != entry["stamp"]:
            continue
        for text in component.get("texts", []):
            choices = text.get("from", [])
            for choice in choices if isinstance(choices, list) else [choices]:
                if (ROOT / choice).exists():
                    paths.append(ROOT / choice)
                    break
            else:
                if "from" in text:
                    sys.exit(f"core-plan: {component['id']}'s licence text {choices} is missing")
    return paths


def collect(core, out):
    entry = find(core)
    out = Path(out).resolve()
    stage = ROOT / "build/cores/stage"
    files = [ROOT / "build/cores/stamps" / entry["stamp"]]
    for kind in ("cores", "info"):
        files += sorted((stage / kind).glob(f"{core}_libretro.*"))
    # the work folder a script names after its stamp or its core (build.json, abi.json)
    for folder in {entry["stamp"], core}:
        files += [ROOT / "build/cores" / folder / name for name in ("build.json", "abi.json")
                  if (ROOT / "build/cores" / folder / name).is_file()]
    files += metadata_sources(core)
    folders = [stage / "system"] if (stage / "system").is_dir() else []
    for path in licence_texts(entry):
        (folders if path.is_dir() else files).append(path)
    for path in files:
        if not path.is_file():
            sys.exit(f"core-plan: {core} did not leave {path.relative_to(ROOT)}")
        target = out / path.relative_to(ROOT)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    # folders: assets a core stages under build/cores/stage/system (PPSSPP, Dolphin; only this
    # job's core is there), and licence folders
    for path in folders:
        shutil.copytree(path, out / path.relative_to(ROOT), dirs_exist_ok=True)
    print(f"==> [{core}] {len(files)} files and {len(folders)} folders collected in {out}")


def main(argv):
    if argv[:1] == ["matrix"] and len(argv) == 1:
        print(json.dumps(plan()))
    elif argv[:1] == ["prefetch"] and len(argv) == 2:
        prefetch(argv[1])
    elif argv[:1] == ["collect"] and len(argv) == 3:
        collect(argv[1], argv[2])
    else:
        sys.exit("usage: core-plan.py matrix | prefetch CORE | collect CORE OUT")


if __name__ == "__main__":
    main(sys.argv[1:])
