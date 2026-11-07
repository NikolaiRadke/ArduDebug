// Everything ArduDebug needs from arduino-cli. arduino-cli uses the same data
// directory as the Arduino IDE (~/.arduino15 or %LOCALAPPDATA%\Arduino15), so an
// ArduDebug core already installed there is shared.
import { spawn } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

export const INDEX_URL =
  'https://raw.githubusercontent.com/NikolaiRadke/ArduDebug/main/package_ardudebug_index.json';
export const CORE = 'ardudebug:avr';
// Board package version this extension needs. Older installed versions are
// upgraded automatically, newer ones are kept.
export const CORE_VERSION = '1.8.8-2';

export class Cli {
  constructor(private extPath: string, private out: vscode.OutputChannel) {}

  // Bundled arduino-cli for this system (bin/linux-x64/arduino-cli,
  // bin/win32-x64/arduino-cli.exe, ...), otherwise the one from PATH (for development).
  get exe(): string {
    const name = process.platform === 'win32' ? 'arduino-cli.exe' : 'arduino-cli';
    const bundled = path.join(this.extPath, 'bin', `${process.platform}-${process.arch}`, name);
    if (!fs.existsSync(bundled)) return name;
    // VSIX files don't reliably keep the executable bit.
    if (process.platform !== 'win32') fs.chmodSync(bundled, 0o755);
    return bundled;
  }

  run(args: string[], log = true): Promise<string> {
    if (log) this.out.appendLine(`> arduino-cli ${args.join(' ')}`);
    return new Promise((resolve, reject) => {
      const p = spawn(this.exe, args);
      let stdout = '';
      let stderr = '';
      p.stdout.on('data', (d) => {
        stdout += d;
        if (log) this.out.append(d.toString());
      });
      p.stderr.on('data', (d) => {
        stderr += d;
        if (log) this.out.append(d.toString());
      });
      p.on('error', (e) => reject(new Error(vscode.l10n.t('Cannot start arduino-cli: {0}', e.message))));
      p.on('close', (code) =>
        code === 0 ? resolve(stdout) : reject(new Error(stderr.trim() || vscode.l10n.t('arduino-cli: error {0}', String(code))))
      );
    });
  }

  async json<T = any>(args: string[]): Promise<T> {
    return JSON.parse(await this.run([...args, '--format', 'json'], false));
  }

  private coreReady = false;

  // Installs the core with bridge and avr-gdb via the boards manager if needed.
  // Checked only on the first command, not afterwards (saves half a second).
  async ensureCore(): Promise<void> {
    if (this.coreReady) return;
    await this.installCore();
    this.coreReady = true;
  }

  private async installCore(): Promise<void> {
    const list = await this.json<any>(['core', 'list']);
    const platforms: any[] = Array.isArray(list) ? list : list.platforms ?? [];
    const installed: string | undefined = platforms.find((p) => p.id === CORE)?.installed_version;
    if (installed && compareVersions(installed, CORE_VERSION) >= 0) return;
    await vscode.window.withProgress(
      { location: vscode.ProgressLocation.Notification, title: vscode.l10n.t('Setting up ArduDebug …') },
      async () => {
        await this.run(['core', 'update-index', '--additional-urls', INDEX_URL]);
        await this.run(['core', 'install', `${CORE}@${CORE_VERSION}`, '--additional-urls', INDEX_URL]);
      }
    );
  }

  // Compile (optionally upload). For debugging always without LTO and with
  // "Optimize for Debugging" (-DARDUDEBUG), otherwise variables are missing.
  compile(sketch: string, fqbn: string, buildPath: string, debug: boolean, port?: string) {
    const args = ['compile', '-b', debug ? withOption(fqbn, 'lto', 'off') : fqbn, '--build-path', buildPath];
    if (debug) args.push('--optimize-for-debug');
    if (port) args.push('-u', '-p', port);
    args.push(sketch);
    return this.run(args);
  }

  debugInfo(sketch: string, fqbn: string, buildPath: string, port?: string) {
    const args = ['debug', '--info', '-b', withOption(fqbn, 'lto', 'off'), '--build-path', buildPath];
    if (port) args.push('-p', port);
    args.push(sketch);
    return this.json<any>(args);
  }
}

// Adds a board option to the FQBN (ardudebug:avr:uno -> ardudebug:avr:uno:lto=off,
// ardudebug:avr:nano:cpu=atmega328old -> ...:cpu=atmega328old,lto=off).
export function withOption(fqbn: string, key: string, value: string): string {
  const parts = fqbn.split(':');
  const opts = (parts[3] ?? '').split(',').filter((o) => o && !o.startsWith(key + '='));
  opts.push(`${key}=${value}`);
  return parts.slice(0, 3).join(':') + ':' + opts.join(',');
}

// Compares versions like 1.8.8-0 and 1.8.8-1 number by number.
export function compareVersions(a: string, b: string): number {
  const pa = a.split(/[.-]/).map(Number);
  const pb = b.split(/[.-]/).map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] ?? 0) - (pb[i] ?? 0);
    if (d) return d;
  }
  return 0;
}
