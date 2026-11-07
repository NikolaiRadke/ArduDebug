// Serial monitor: runs "arduino-cli monitor" in a VS Code terminal.
// It is closed before uploading and debugging (the port must be free) and
// reopened automatically afterwards.
import * as vscode from 'vscode';
import { Cli } from './cli';

const BAUDRATES = [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200, 230400, 250000, 500000, 1000000, 2000000];

export class Monitor {
  private terminal?: vscode.Terminal;
  private port?: string;
  private reopenPort?: string; // reopen after uploading/debugging
  private item: vscode.StatusBarItem;
  private subs: vscode.Disposable[] = [];

  constructor(private cli: Cli, private state: vscode.Memento) {
    this.item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 49);
    this.item.command = 'ardudebug.selectBaudrate';
    this.item.tooltip = vscode.l10n.t('ArduDebug: select baud rate');
    this.updateItem();
    this.closeLeftovers();
    this.subs.push(
      vscode.window.onDidCloseTerminal((t) => {
        if (t === this.terminal) this.terminal = undefined;
      })
    );
  }

  dispose() {
    this.item.dispose();
    this.subs.forEach((s) => s.dispose());
  }

  // Monitors from an earlier session (restored by VS Code) hold the port
  // without us knowing about them. So close them on startup.
  private closeLeftovers() {
    for (const t of vscode.window.terminals) {
      const opts = t.creationOptions as vscode.TerminalOptions;
      const ours =
        (opts.shellPath?.includes('arduino-cli') && opts.shellArgs?.[0] === 'monitor') ||
        ['Serial Monitor', 'Serieller Monitor'].includes(t.name);
      if (ours) t.dispose();
    }
  }

  get baudrate(): number {
    return this.state.get<number>('baudrate') ?? 9600;
  }

  show(visible: boolean) {
    visible ? this.item.show() : this.item.hide();
  }

  open(port: string) {
    this.terminal?.dispose();
    this.port = port;
    this.terminal = vscode.window.createTerminal({
      name: vscode.l10n.t('Serial Monitor'),
      shellPath: this.cli.exe,
      shellArgs: ['monitor', '-p', port, '--config', `baudrate=${this.baudrate}`, '--quiet'],
      iconPath: new vscode.ThemeIcon('output'),
      isTransient: true, // don't restore when VS Code restarts
    });
    this.terminal.show(true);
  }

  // Closes the monitor, remembers to reopen it and waits briefly until
  // the operating system has released the port.
  async suspend() {
    if (!this.terminal) return;
    this.reopenPort = this.port;
    this.terminal.dispose();
    this.terminal = undefined;
    await new Promise((r) => setTimeout(r, 500));
  }

  resume() {
    if (this.reopenPort) this.open(this.reopenPort);
    this.reopenPort = undefined;
  }

  async selectBaudrate() {
    const pick = await vscode.window.showQuickPick(
      BAUDRATES.map((b) => ({ label: String(b), picked: b === this.baudrate })),
      { placeHolder: vscode.l10n.t('Baud rate (must match Serial.begin() in the sketch)') }
    );
    if (!pick) return;
    await this.state.update('baudrate', Number(pick.label));
    this.updateItem();
    if (this.terminal && this.port) this.open(this.port); // restart with the new baud rate
  }

  private updateItem() {
    this.item.text = `$(pulse) ${this.baudrate} Baud`;
  }
}
