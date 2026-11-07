#!/usr/bin/env python3
# Builds a new release of the ArduDebug board package for the Arduino boards
# manager and updates package_ardudebug_index.json in the repository.
# For development only, end users don't need Python.
#
#   1. Set VERSION.
#   2. python3 scripts/make_package.py
#   3. Create the GitHub release V<VERSION> and attach all files the script
#      lists at the end (core, bridge and avr-gdb for all systems).
#   4. Only then commit and push package_ardudebug_index.json.
#
# Every release is complete: core, bridge and avr-gdb, all with the same version.
# Older versions stay in the index, so users can go back in the boards manager.
import hashlib, json, shutil, tarfile
from pathlib import Path

VERSION = "1.8.8-3"

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "core" / "avr"
TOOLS = ROOT / "tools"              # ready built programs per system: tools/<tag>/
DUMMY = ROOT / "bridge" / "dummy.cfg"
OUT = ROOT / "dist"
INDEX = ROOT / "package_ardudebug_index.json"
REPO_URL = "https://github.com/NikolaiRadke/ArduDebug"
NAME = "ArduDebug AVR Boards"

# Per system: host in the index, folder in tools/ and file name suffix, file extension
HOSTS = [("x86_64-linux-gnu",    "linux64",      ""),
         ("x86_64-mingw32",      "windows64",    ".exe"),
         ("x86_64-apple-darwin", "macos-x86_64", ""),
         ("arm64-apple-darwin",  "macos-arm64",  "")]

new_files = []


def pack(name, version, entries):
    """Packs (source, path in archive) pairs and returns the details for the index."""
    path = OUT / name
    with tarfile.open(path, "w:bz2") as tar:
        for src, arcname in entries:
            tar.add(src, arcname=arcname)
    data = path.read_bytes()
    new_files.append(path)
    return {"url": f"{REPO_URL}/releases/download/V{version}/{name}", "archiveFileName": name,
            "checksum": "SHA-256:" + hashlib.sha256(data).hexdigest(), "size": str(len(data))}


def find_deps(obj):
    """Finds the tool dependencies of the original core in installed.json."""
    if isinstance(obj, dict):
        if "toolsDependencies" in obj:
            return obj
        obj = list(obj.values())
    if isinstance(obj, list):
        for item in obj:
            found = find_deps(item)
            if found:
                return found
    return None


def board_names():
    """Reads all board names (<id>.name=...) from boards.txt."""
    names = []
    for line in (SRC / "boards.txt").read_text().splitlines():
        key, _, value = line.partition("=")
        parts = key.strip().split(".")
        if len(parts) == 2 and parts[1] == "name":
            names.append({"name": value.strip()})
    return names


def build_core():
    """Core archive: copy without tools/ and installed.json. platform.txt already
    points to the installed tools ({runtime.tools.ardudebug-bridge.path} etc.)."""
    stage = OUT / "stage" / f"ardudebug-avr-{VERSION}"
    shutil.rmtree(OUT / "stage", ignore_errors=True)
    shutil.copytree(SRC, stage, ignore=shutil.ignore_patterns("tools", "installed.json"))
    core = pack(f"ardudebug-avr-{VERSION}.tar.bz2", VERSION, [(stage, stage.name)])
    shutil.rmtree(OUT / "stage", ignore_errors=True)

    # Check: does the core archive contain the debug changes?
    with tarfile.open(OUT / f"ardudebug-avr-{VERSION}.tar.bz2") as t:
        hs0 = t.extractfile(f"ardudebug-avr-{VERSION}/cores/arduino/HardwareSerial0.cpp").read()
    print("Check HardwareSerial0.cpp:", "OK" if b"!defined(ARDUDEBUG)" in hs0 else "MISSING!")
    return core


def build_tools():
    """Archives of bridge and avr-gdb for all systems."""
    bridge, gdb = [], []
    for host, tag, ext in HOSTS:
        b = pack(f"ardudebug-bridge-{VERSION}-{tag}.tar.bz2", VERSION, [
            (TOOLS / tag / f"ardudebug-bridge{ext}", f"ardudebug-bridge/ardudebug-bridge{ext}"),
            (DUMMY, "ardudebug-bridge/dummy.cfg")])
        g = pack(f"ardudebug-gdb-{VERSION}-{tag}.tar.bz2", VERSION, [
            (TOOLS / tag / f"avr-gdb{ext}", f"ardudebug-gdb/bin/avr-gdb{ext}")])
        bridge.append(dict(host=host, **b))
        gdb.append(dict(host=host, **g))
    return [{"name": "ardudebug-bridge", "version": VERSION, "systems": bridge},
            {"name": "ardudebug-gdb", "version": VERSION, "systems": gdb}]


def main():
    OUT.mkdir(exist_ok=True)
    index = json.loads(INDEX.read_text())
    package = index["packages"][0]

    print(f"Building bridge and avr-gdb {VERSION}")
    package["tools"] = [t for t in package["tools"] if t["version"] != VERSION] + build_tools()

    print(f"Building core {VERSION}")
    deps = find_deps(json.loads((SRC / "installed.json").read_text()))["toolsDependencies"]
    own_tools = [{"packager": "ardudebug", "name": n, "version": VERSION}
                 for n in ("ardudebug-bridge", "ardudebug-gdb")]
    platform = {"name": NAME, "architecture": "avr", "version": VERSION,
                "category": "Arduino", "boards": board_names(),
                "toolsDependencies": deps + own_tools, **build_core()}

    # Replace an existing entry of the same version (rebuild), keep all others
    package["platforms"] = [p for p in package["platforms"] if p["version"] != VERSION]
    package["platforms"].append(platform)

    INDEX.write_text(json.dumps(index, indent=2) + "\n")
    print("\nVersions in the index:", ", ".join(p["version"] for p in package["platforms"]))
    print("Dependencies:", ", ".join(f"{d['name']} {d['version']}" for d in deps + own_tools))
    print(f"\nAttach to the GitHub release V{VERSION}:")
    for f in new_files:
        print("  ", f)
    print(f"Then commit and push {INDEX.name}.")


if __name__ == "__main__":
    main()
