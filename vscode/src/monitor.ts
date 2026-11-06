// Serieller Monitor: startet "arduino-cli monitor" in einem VS-Code-Terminal.
// Vor dem Hochladen und Debuggen wird er geschlossen (der Port muss frei sein)
// und danach automatisch wieder geöffnet.
import * as vscode from 'vscode';
import { Cli } from './cli';

const BAUDRATES = [300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200, 230400, 250000, 500000, 1000000, 2000000];

export class Monitor {
  private terminal?: vscode.Terminal;
  private port?: string;
  private reopenPort?: string; // nach Hochladen/Debuggen wieder öffnen
  private item: vscode.StatusBarItem;
  private subs: vscode.Disposable[] = [];

  constructor(private cli: Cli, private state: vscode.Memento) {
    this.item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 49);
    this.item.command = 'ardudebug.selectBaudrate';
    this.item.tooltip = vscode.l10n.t('ArduDebug: select baud rate');
    this.updateItem();
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
    });
    this.terminal.show(true);
  }

  // Schließt den Monitor, merkt ihn zum Wiederöffnen vor und wartet kurz,
  // bis das Betriebssystem den Port freigegeben hat.
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
    if (this.terminal && this.port) this.open(this.port); // mit neuer Baudrate neu starten
  }

  private updateItem() {
    this.item.text = `$(pulse) ${this.baudrate} Baud`;
  }
}
