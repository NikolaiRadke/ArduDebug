#!/usr/bin/env python3
# Baut die ArduDebug-Archive und die package_index.json fuer einen lokalen Test.
# Nur fuer die Entwicklung, Endnutzer brauchen kein Python.
import hashlib, json, shutil, tarfile
from pathlib import Path

VERSION = "0.1.0"
SRC = Path.home() / "Dropbox/Erfinderschuppen/ardudebug/core/avr"
OUT = Path.home() / "Dropbox/Erfinderschuppen/ardudebug/dist"
BASE_URL = "http://localhost:8765"
HOST = "x86_64-linux-gnu"

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
            return obj["toolsDependencies"]
        obj = list(obj.values())
    if isinstance(obj, list):
        for item in obj:
            found = find_deps(item)
            if found:
                return found
    return None

OUT.mkdir(exist_ok=True)
deps = find_deps(json.loads((SRC / "installed.json").read_text()))

# Core: Kopie ohne tools/ und installed.json, Tool-Pfade auf installierte Tools umbiegen
stage = OUT / "stage" / f"ardudebug-avr-{VERSION}"
shutil.rmtree(OUT / "stage", ignore_errors=True)
shutil.copytree(SRC, stage, ignore=shutil.ignore_patterns("tools", "installed.json"))
pt = stage / "platform.txt"
pt.write_text(pt.read_text()
    .replace("{runtime.platform.path}/tools/bridge", "{runtime.tools.ardudebug-bridge.path}")
    .replace("{runtime.platform.path}/tools/gdb", "{runtime.tools.ardudebug-gdb.path}"))
core = pack(f"ardudebug-avr-{VERSION}.tar.bz2", [(stage, stage.name)])

bridge = pack(f"ardudebug-bridge-{VERSION}-linux64.tar.bz2", [
    (SRC / "tools/bridge/ardudebug-bridge", "ardudebug-bridge/ardudebug-bridge"),
    (SRC / "tools/bridge/dummy.cfg", "ardudebug-bridge/dummy.cfg")])
gdb = pack(f"ardudebug-gdb-{VERSION}-linux64.tar.bz2", [
    (SRC / "tools/gdb/bin/avr-gdb", "ardudebug-gdb/bin/avr-gdb")])

own_tools = [{"packager": "ardudebug", "name": n, "version": VERSION}
             for n in ("ardudebug-bridge", "ardudebug-gdb")]
index = {"packages": [{
    "name": "ardudebug", "maintainer": "Nikolai", "websiteURL": "https://example.com",
    "email": "", "help": {"online": "https://example.com"},
    "platforms": [{"name": "ArduDebug AVR Boards", "architecture": "avr", "version": VERSION,
                   "category": "Arduino", "boards": [{"name": "Arduino Uno"}],
                   "toolsDependencies": deps + own_tools, **core}],
    "tools": [{"name": "ardudebug-bridge", "version": VERSION, "systems": [dict(host=HOST, **bridge)]},
              {"name": "ardudebug-gdb", "version": VERSION, "systems": [dict(host=HOST, **gdb)]}]}]}
(OUT / "package_ardudebug_index.json").write_text(json.dumps(index, indent=2))
print("Fertig:", OUT / "package_ardudebug_index.json")
print("Abhaengigkeiten:", ", ".join(f"{d['name']} {d['version']}" for d in deps + own_tools))
