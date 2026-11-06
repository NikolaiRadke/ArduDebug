// Alles, was ArduDebug von arduino-cli braucht. arduino-cli nutzt dasselbe
// Datenverzeichnis wie die Arduino IDE (~/.arduino15 bzw. %LOCALAPPDATA%\Arduino15),
// ein dort schon installierter ArduDebug-Core wird also mitbenutzt.
import { spawn } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

export const INDEX_URL =
  'https://raw.githubusercontent.com/NikolaiRadke/ArduDebug/main/package_ardudebug_index.json';
export const CORE = 'ardudebug:avr';

export class Cli {
  constructor(private extPath: string, private out: vscode.OutputChannel) {}

  // Mitgeliefertes arduino-cli für dieses System (bin/linux-x64/arduino-cli,
  // bin/win32-x64/arduino-cli.exe, ...), sonst das aus dem PATH (zum Entwickeln).
  get exe(): string {
    const name = process.platform === 'win32' ? 'arduino-cli.exe' : 'arduino-cli';
    const bundled = path.join(this.extPath, 'bin', `${process.platform}-${process.arch}`, name);
    if (!fs.existsSync(bundled)) return name;
    // VSIX-Dateien bewahren das Ausfuehrungsrecht nicht zuverlaessig.
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

  // Einmalig: Core samt Bridge und avr-gdb über den Boardverwalter holen.
  async ensureCore(): Promise<void> {
    const list = await this.json<any>(['core', 'list']);
    const platforms: any[] = Array.isArray(list) ? list : list.platforms ?? [];
    if (platforms.some((p) => p.id === CORE)) return;
    await vscode.window.withProgress(
      { location: vscode.ProgressLocation.Notification, title: vscode.l10n.t('Setting up ArduDebug …') },
      async () => {
        await this.run(['core', 'update-index', '--additional-urls', INDEX_URL]);
        await this.run(['core', 'install', CORE, '--additional-urls', INDEX_URL]);
      }
    );
  }

  // Kompilieren (optional mit Hochladen). Für das Debuggen immer ohne LTO und
  // mit "Für Debugging optimieren" (-DARDUDEBUG), sonst fehlen Variablen.
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

// Hängt eine Board-Option an den FQBN an (ardudebug:avr:uno -> ardudebug:avr:uno:lto=off,
// ardudebug:avr:nano:cpu=atmega328old -> ...:cpu=atmega328old,lto=off).
export function withOption(fqbn: string, key: string, value: string): string {
  const parts = fqbn.split(':');
  const opts = (parts[3] ?? '').split(',').filter((o) => o && !o.startsWith(key + '='));
  opts.push(`${key}=${value}`);
  return parts.slice(0, 3).join(':') + ':' + opts.join(',');
}
