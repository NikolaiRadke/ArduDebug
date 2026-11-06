#!/usr/bin/env python3
# Baut die VS-Code-Extension als VSIX-Dateien mit mitgeliefertem arduino-cli.
# arduino-cli liegt in vscode/bin/<system>-<cpu>/, die Extension waehlt beim
# Start das passende. Nur fuer die Entwicklung, Endnutzer brauchen kein Python.
#
#   dist/github/       je eine VSIX fuer Linux, Windows und macOS (Intel + Apple)
#                      -> ans GitHub-Release haengen
#   dist/marketplace/  je eine VSIX pro Plattformkennung (--target)
#                      -> npx vsce publish --packagePath dist/marketplace/*.vsix
#
# Aufruf:  python3 scripts/package_vscode.py
import hashlib, io, json, shutil, subprocess, sys, tarfile, urllib.request, zipfile
from pathlib import Path

CLI_VERSION = "1.5.1"
CLI_URL = f"https://github.com/arduino/arduino-cli/releases/download/v{CLI_VERSION}"

# Ordner in vscode/bin/ (= process.platform-process.arch in Node) -> Archiv von arduino-cli
TARGETS = {
    "linux-x64":    f"arduino-cli_{CLI_VERSION}_Linux_64bit.tar.gz",
    "win32-x64":    f"arduino-cli_{CLI_VERSION}_Windows_64bit.zip",
    "darwin-x64":   f"arduino-cli_{CLI_VERSION}_macOS_64bit.tar.gz",
    "darwin-arm64": f"arduino-cli_{CLI_VERSION}_macOS_ARM64.tar.gz",
}

# GitHub: eine VSIX pro Betriebssystem -> enthaltene arduino-cli-Varianten.
# Ohne Plattformkennung, sonst lehnt VS Code z. B. die Intel-Variante auf einem
# Apple-Mac ab. Der Marketplace bekommt je Variante eine eigene VSIX mit Kennung.
GITHUB = {
    "linux":   ["linux-x64"],
    "windows": ["win32-x64"],
    "macos":   ["darwin-x64", "darwin-arm64"],
}

ROOT = Path(__file__).resolve().parent.parent
EXT = ROOT / "vscode"
OUT = ROOT / "dist"
CACHE = OUT / "cli-cache"


def download(name):
    """Laedt eine Datei von den arduino-cli-Releases, mit Zwischenspeicher."""
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists():
        print(f"  lade {name}")
        with urllib.request.urlopen(f"{CLI_URL}/{name}") as r:
            path.write_bytes(r.read())
    return path.read_bytes()


def checksums():
    """Pruefsummen der Release-Dateien: {Dateiname: sha256}"""
    text = download(f"{CLI_VERSION}-checksums.txt").decode()
    return {line.split()[1].lstrip("*"): line.split()[0] for line in text.splitlines() if line.strip()}


def extract_cli(archive_name, data, bin_dir):
    """Holt die arduino-cli-Datei aus dem Archiv und legt sie nach bin_dir."""
    bin_dir.mkdir(parents=True)
    if archive_name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            (bin_dir / "arduino-cli.exe").write_bytes(z.read("arduino-cli.exe"))
    else:
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            target = bin_dir / "arduino-cli"
            target.write_bytes(t.extractfile("arduino-cli").read())
            target.chmod(0o755)


def run(cmd):
    print("  >", " ".join(cmd))
    subprocess.run(cmd, cwd=EXT, check=True, shell=sys.platform == "win32")


def package(folders, vsix, sums, target=None):
    """Packt die Extension mit den arduino-cli-Varianten aus folders."""
    shutil.rmtree(EXT / "bin", ignore_errors=True)
    for folder in folders:
        archive = TARGETS[folder]
        data = download(archive)
        if hashlib.sha256(data).hexdigest() != sums.get(archive):
            sys.exit(f"Pruefsumme von {archive} stimmt nicht!")
        extract_cli(archive, data, EXT / "bin" / folder)
    cmd = ["npx", "vsce", "package", "--skip-license", "-o", str(vsix)]
    if target:
        cmd += ["--target", target]
    run(cmd)


def main():
    version = json.loads((EXT / "package.json").read_text())["version"]
    sums = checksums()

    print("Extension kompilieren")
    run(["npm", "ci"])
    run(["npm", "run", "compile"])

    print("\nGitHub")
    (OUT / "github").mkdir(parents=True, exist_ok=True)
    for os_name, folders in GITHUB.items():
        package(folders, OUT / "github" / f"ardudebug-{os_name}-{version}.vsix", sums)

    print("\nMarketplace")
    (OUT / "marketplace").mkdir(parents=True, exist_ok=True)
    for target in TARGETS:
        package([target], OUT / "marketplace" / f"ardudebug-{target}-{version}.vsix", sums, target)

    shutil.rmtree(EXT / "bin", ignore_errors=True)
    print(f"\nFertig, die VSIX-Dateien liegen in {OUT}/github und {OUT}/marketplace")


if __name__ == "__main__":
    main()
