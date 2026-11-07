#!/usr/bin/env python3
# Builds the VS Code extension as VSIX files with a bundled arduino-cli.
# arduino-cli is placed in vscode/bin/<system>-<cpu>/, the extension picks the
# right one at startup. For development only, end users don't need Python.
#
#   dist/github/       one VSIX each for Linux, Windows and macOS (Intel + Apple)
#                      -> attach to the GitHub release
#   dist/marketplace/  one VSIX per platform target (--target)
#                      -> npx vsce publish --packagePath dist/marketplace/*.vsix
#
# Usage:  python3 scripts/package_vscode.py
import hashlib, io, json, shutil, subprocess, sys, tarfile, urllib.request, zipfile
from pathlib import Path

CLI_VERSION = "1.5.1"
CLI_URL = f"https://github.com/arduino/arduino-cli/releases/download/v{CLI_VERSION}"

# Folder in vscode/bin/ (= process.platform-process.arch in Node) -> arduino-cli archive
TARGETS = {
    "linux-x64":    f"arduino-cli_{CLI_VERSION}_Linux_64bit.tar.gz",
    "win32-x64":    f"arduino-cli_{CLI_VERSION}_Windows_64bit.zip",
    "darwin-x64":   f"arduino-cli_{CLI_VERSION}_macOS_64bit.tar.gz",
    "darwin-arm64": f"arduino-cli_{CLI_VERSION}_macOS_ARM64.tar.gz",
}

# GitHub: one VSIX per operating system -> included arduino-cli variants.
# Without platform target, otherwise VS Code rejects e.g. the Intel variant on an
# Apple Silicon Mac. The Marketplace gets a separate VSIX with target per variant.
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
    """Downloads a file from the arduino-cli releases, with cache."""
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists():
        print(f"  downloading {name}")
        with urllib.request.urlopen(f"{CLI_URL}/{name}") as r:
            path.write_bytes(r.read())
    return path.read_bytes()


def checksums():
    """Checksums of the release files: {file name: sha256}"""
    text = download(f"{CLI_VERSION}-checksums.txt").decode()
    return {line.split()[1].lstrip("*"): line.split()[0] for line in text.splitlines() if line.strip()}


def extract_cli(archive_name, data, bin_dir):
    """Extracts the arduino-cli binary from the archive into bin_dir."""
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
    """Packages the extension with the arduino-cli variants from folders."""
    shutil.rmtree(EXT / "bin", ignore_errors=True)
    for folder in folders:
        archive = TARGETS[folder]
        data = download(archive)
        if hashlib.sha256(data).hexdigest() != sums.get(archive):
            sys.exit(f"Checksum of {archive} does not match!")
        extract_cli(archive, data, EXT / "bin" / folder)
    cmd = ["npx", "vsce", "package", "--skip-license", "-o", str(vsix)]
    if target:
        cmd += ["--target", target]
    run(cmd)


def check_node():
    """vsce needs Node 22.12 or newer (require() of ES modules)."""
    out = subprocess.run(["node", "-v"], capture_output=True, text=True,
                         shell=sys.platform == "win32").stdout.strip()
    major, minor = (int(x) for x in out.lstrip("v").split(".")[:2])
    if (major, minor) < (22, 12):
        sys.exit(f"Node {out} is too old, vsce needs at least v22.12.")


def main():
    check_node()
    version = json.loads((EXT / "package.json").read_text())["version"]
    sums = checksums()

    print("Compiling extension")
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
    print(f"\nDone, the VSIX files are in {OUT}/github and {OUT}/marketplace")


if __name__ == "__main__":
    main()
