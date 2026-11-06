// ArduDebug für VS Code: Kompilieren, Hochladen, serieller Monitor und Debuggen
// eines Arduino-Sketches ohne Konfigurationsdateien. Maßgeblich ist immer die
// gerade aktive .ino-Datei, nicht der geöffnete Ordner (Einsteiger öffnen oft
// das ganze Sketchbook).
import * as crypto from 'crypto';
import * as path from 'path';
import * as vscode from 'vscode';
import { Boards } from './board';
import { Cli } from './cli';
import { adapterFactory, buildConfig, DEBUG_TYPE } from './debug';
import { writeIntelliSense } from './intellisense';
import { Monitor } from './monitor';

let cli: Cli;
let boards: Boards;
let monitor: Monitor;
let out: vscode.OutputChannel;
let buildRoot: string;

export function activate(ctx: vscode.ExtensionContext) {
  out = vscode.window.createOutputChannel('ArduDebug');
  cli = new Cli(ctx.extensionPath, out);
  boards = new Boards(cli, ctx.workspaceState);
  monitor = new Monitor(cli, ctx.globalState);
  buildRoot = path.join(ctx.globalStorageUri.fsPath, 'build');

  ctx.subscriptions.push(
    out,
    boards,
    monitor,
    vscode.debug.registerDebugAdapterDescriptorFactory(DEBUG_TYPE, adapterFactory),
    command('ardudebug.compile', (s) => build(s, false)),
    command('ardudebug.upload', (s) => build(s, true)),
    command('ardudebug.debug', debug),
    command('ardudebug.monitor', async (s) => monitor.open((await boards.resolve(s)).port)),
    command('ardudebug.selectBoard', (s) => boards.selectBoard(s)),
    command('ardudebug.selectPort', (s) => boards.selectPort(s)),
    vscode.commands.registerCommand('ardudebug.selectBaudrate', () => monitor.selectBaudrate()),
    vscode.window.onDidChangeActiveTextEditor(updateStatusBar),
    // Nach dem Debuggen gehört der Port wieder dem Monitor
    vscode.debug.onDidTerminateDebugSession((s) => {
      if (s.type === DEBUG_TYPE) setTimeout(() => monitor.resume(), 1000);
    })
  );
  updateStatusBar();
}

export function deactivate() {}

function updateStatusBar() {
  const sketch = currentSketch();
  boards.show(sketch);
  monitor.show(!!sketch);
}

// Ordner des Sketches der aktiven .ino-Datei
function currentSketch(): string | undefined {
  const doc = vscode.window.activeTextEditor?.document;
  return doc?.fileName.endsWith('.ino') ? path.dirname(doc.fileName) : undefined;
}

// Befehl mit gemeinsamer Fehlerbehandlung; ohne aktiven Sketch passiert nichts.
function command(id: string, fn: (sketch: string) => Promise<unknown>) {
  return vscode.commands.registerCommand(id, async () => {
    const sketch = currentSketch();
    if (!sketch) {
      vscode.window.showWarningMessage(vscode.l10n.t('Please open an .ino file first.'));
      return;
    }
    try {
      await vscode.workspace.saveAll(false);
      await cli.ensureCore();
      await fn(sketch);
    } catch (e: any) {
      out.show(true);
      vscode.window.showErrorMessage(`ArduDebug: ${e.message ?? e}`);
    }
  });
}

// Eigener Build-Ordner pro Sketch, damit der Sketch-Ordner sauber bleibt.
function buildPath(sketch: string): string {
  const id = crypto.createHash('md5').update(sketch).digest('hex').slice(0, 8);
  return path.join(buildRoot, `${path.basename(sketch)}-${id}`);
}

async function build(sketch: string, upload: boolean) {
  const sel = await boards.resolve(sketch);
  writeIntelliSense(cli, sketch, sel.fqbn).catch(() => {});
  if (!upload) {
    await progress(vscode.l10n.t('Compiling …'), () => cli.compile(sketch, sel.fqbn, buildPath(sketch), false));
    return;
  }
  await monitor.suspend();
  try {
    await progress(vscode.l10n.t('Uploading …'), () =>
      cli.compile(sketch, sel.fqbn, buildPath(sketch), false, sel.port)
    );
  } finally {
    monitor.resume();
  }
}

async function debug(sketch: string) {
  const sel = await boards.resolve(sketch);
  writeIntelliSense(cli, sketch, sel.fqbn).catch(() => {});
  const bp = buildPath(sketch);
  await monitor.suspend();
  try {
    await progress(vscode.l10n.t('Compiling and uploading for debugging …'), () =>
      cli.compile(sketch, sel.fqbn, bp, true, sel.port)
    );
    const info = await cli.debugInfo(sketch, sel.fqbn, bp, sel.port);
    const config = buildConfig(info, path.basename(sketch));
    const ws = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(sketch));
    if (!(await vscode.debug.startDebugging(ws, config))) {
      throw new Error(vscode.l10n.t('Could not start the debugger.'));
    }
  } catch (e) {
    monitor.resume(); // Debuggen kam nicht zustande, Monitor gleich wieder öffnen
    throw e;
  }
}

function progress<T>(title: string, task: () => Promise<T>) {
  return vscode.window.withProgress(
    { location: vscode.ProgressLocation.Notification, title: `ArduDebug: ${title}` },
    task
  );
}
