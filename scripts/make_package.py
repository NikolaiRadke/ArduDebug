#!/usr/bin/env python3
# Baut die ArduDebug-Archive und die package_index.json fuer einen lokalen Test.
# Nur fuer die Entwicklung, Endnutzer brauchen kein Python.
import hashlib, json, shutil, tarfile
from pathlib import Path

VERSION = "0.1.0"
SRC = Path.home() / "Dropbox/Erfinderschuppen/ardudebug/core/avr"
OUT = Path.home() / "Dropbox/Erfinderschuppen/ardudebug/dist"
BASE_URL = f"https://github.com/NikolaiRadke/ArduDebug/releases/download/v{VERSION}"
HOST = "x86_64-linux-gnu"
REPO_URL = "https://github.com/NikolaiRadke/ArduDebug"

def pack(name, entries):
    """Packt (Quelle, Pfad im Archiv)-Paare und liefert die Angaben fuer den Index."""
    path = OUT / name
    with tarfile.open(path, "w:bz2") as tar:
        for src, arcname in entries:
            tar.add(src, arcname=arcname)
    data = path.read_bytes()
    return {"url": f"{BASE_URL}/{name}", "archiveFileName": name,
            "checksum": "SHA-256:" + hashlib.sha256(data).hexdigest(), "size": str(len(data))}

def find_deps(obj):
    """Sucht die Tool-Abhaengigkeiten des Original-Cores in installed.json."""
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
    """Liest alle Board-Namen (<id>.name=...) aus der boards.txt."""
    names = []
    for line in (SRC / "boards.txt").read_text().splitlines():
        key, _, value = line.partition("=")
        parts = key.strip().split(".")
        if len(parts) == 2 and parts[1] == "name":
            names.append({"name": value.strip()})
    return names


OUT.mkdir(exist_ok=True)
base = find_deps(json.loads((SRC / "installed.json").read_text()))
deps = base["toolsDependencies"]
NAME = f"ArduDebug AVR Boards (basiert auf Arduino AVR {base['version']})"

# Core: Kopie ohne tools/ und installed.json, Tool-Pfade auf installierte Tools umbiegen
stage = OUT / "stage" / f"ardudebug-avr-{VERSION}"
shutil.rmtree(OUT / "stage", ignore_errors=True)
shutil.copytree(SRC, stage, ignore=shutil.ignore_patterns("tools", "installed.json"))
pt = stage / "platform.txt"
pt.write_text(pt.read_text()
    .replace("{runtime.platform.path}/tools/bridge", "{runtime.tools.ardudebug-bridge.path}")
    .replace("{runtime.platform.path}/tools/gdb", "{runtime.tools.ardudebug-gdb.path}"))
core = pack(f"ardudebug-avr-{VERSION}.tar.bz2", [(stage, stage.name)])

# Pro System: Kennung im Index, Namenszusatz der Archive, Dateiendung
HOSTS = [("x86_64-linux-gnu", "linux64", ""), ("x86_64-mingw32", "windows64", ".exe")]
bridge_systems, gdb_systems = [], []
for host, tag, ext in HOSTS:
    b = pack(f"ardudebug-bridge-{VERSION}-{tag}.tar.bz2", [
        (SRC / f"tools/bridge/ardudebug-bridge{ext}", f"ardudebug-bridge/ardudebug-bridge{ext}"),
        (SRC / "tools/bridge/dummy.cfg", "ardudebug-bridge/dummy.cfg")])
    g = pack(f"ardudebug-gdb-{VERSION}-{tag}.tar.bz2", [
        (SRC / f"tools/gdb/bin/avr-gdb{ext}", f"ardudebug-gdb/bin/avr-gdb{ext}")])
    bridge_systems.append(dict(host=host, **b))
    gdb_systems.append(dict(host=host, **g))

own_tools = [{"packager": "ardudebug", "name": n, "version": VERSION}
             for n in ("ardudebug-bridge", "ardudebug-gdb")]
index = {"packages": [{
    "name": "ardudebug", "maintainer": "Nikolai Radke", "websiteURL": REPO_URL,
    "email": "kontakt@nikolairadke.de", "help": {"online": REPO_URL},
    "platforms": [{"name": NAME, "architecture": "avr", "version": VERSION,
                   "category": "Arduino", "boards": board_names(),
                   "toolsDependencies": deps + own_tools, **core}],
    "tools": [{"name": "ardudebug-bridge", "version": VERSION, "systems": bridge_systems},
              {"name": "ardudebug-gdb", "version": VERSION, "systems": gdb_systems}]}]}
(OUT / "package_ardudebug_index.json").write_text(json.dumps(index, indent=2))
print("Fertig:", OUT / "package_ardudebug_index.json")
print("Abhaengigkeiten:", ", ".join(f"{d['name']} {d['version']}" for d in deps + own_tools))
