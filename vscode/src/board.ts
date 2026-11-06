// Board- und Port-Wahl. Ziel: Der Nutzer muss im Normalfall nichts wählen.
// Ein originales Board erkennt arduino-cli an der USB-Kennung; bei Klonen
// (CH340 usw.) wird einmal nach dem Board gefragt und die Wahl pro Sketch gemerkt.
import * as vscode from 'vscode';
import { Cli, CORE } from './cli';

interface Selection {
  fqbn?: string;
  name?: string;
  port?: string;
}

export class Boards {
  private item: vscode.StatusBarItem;

  constructor(private cli: Cli, private state: vscode.Memento) {
    this.item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
    this.item.command = 'ardudebug.selectBoard';
  }

  dispose() {
    this.item.dispose();
  }

  private get(sketch: string): Selection {
    return this.state.get<Selection>('board:' + sketch) ?? {};
  }

  private async set(sketch: string, sel: Selection) {
    await this.state.update('board:' + sketch, sel);
    this.show(sketch);
  }

  show(sketch?: string) {
    if (!sketch) {
      this.item.hide();
      return;
    }
    const s = this.get(sketch);
    this.item.text = `$(circuit-board) ${s.name ?? vscode.l10n.t('Board?')}  $(plug) ${s.port ?? 'Port?'}`;
    this.item.tooltip = vscode.l10n.t('ArduDebug: select board and port');
    this.item.show();
  }

  // Liefert Board und Port für den Sketch; erkennt sie automatisch oder fragt nach.
  async resolve(sketch: string): Promise<Required<Selection>> {
    const sel = this.get(sketch);
    const ports = await this.serialPorts();

    // Gemerkter Port weg (anderer USB-Anschluss)? Dann neu suchen.
    if (!sel.port || !ports.some((p) => p.address === sel.port)) {
      sel.port = undefined;
      const known = ports.find((p) => p.fqbn);
      if (known) sel.port = known.address;
      else if (ports.length === 1) sel.port = ports[0].address;
      else if (ports.length > 1) sel.port = await this.pickPort(ports);
    }
    if (!sel.port) throw new Error(vscode.l10n.t('No Arduino found. Is the board connected via USB?'));

    if (!sel.fqbn) {
      // arduino:avr:uno -> ardudebug:avr:uno, sofern ArduDebug das Board kennt
      const detected = ports.find((p) => p.address === sel.port)?.fqbn;
      const boards = await this.allBoards();
      const match = detected && boards.find((b) => b.fqbn === detected.replace(/^arduino:avr:/, CORE + ':'));
      const board = match ?? (await this.pickBoard(boards));
      if (!board) throw new Error(vscode.l10n.t('No board selected.'));
      sel.fqbn = board.fqbn;
      sel.name = board.name;
    }
    await this.set(sketch, sel);
    return sel as Required<Selection>;
  }

  async selectBoard(sketch: string) {
    const board = await this.pickBoard(await this.allBoards());
    if (board) await this.set(sketch, { ...this.get(sketch), fqbn: board.fqbn, name: board.name });
  }

  async selectPort(sketch: string) {
    const port = await this.pickPort(await this.serialPorts());
    if (port) await this.set(sketch, { ...this.get(sketch), port });
  }

  private async serialPorts(): Promise<{ address: string; label: string; fqbn?: string }[]> {
    const res = await this.cli.json<any>(['board', 'list']);
    const ports: any[] = Array.isArray(res) ? res : res.detected_ports ?? [];
    return ports
      .filter((p) => p.port?.protocol === 'serial')
      .map((p) => ({
        address: p.port.address,
        label: p.port.label ?? p.port.address,
        fqbn: p.matching_boards?.[0]?.fqbn,
      }));
  }

  private async allBoards(): Promise<{ fqbn: string; name: string }[]> {
    const res = await this.cli.json<any>(['board', 'listall', CORE]);
    return (res.boards ?? []).filter((b: any) => !b.is_hidden);
  }

  private async pickBoard(boards: { fqbn: string; name: string }[]) {
    const pick = await vscode.window.showQuickPick(
      boards.map((b) => ({ label: b.name, fqbn: b.fqbn })),
      { placeHolder: vscode.l10n.t('Which board is connected?') }
    );
    return pick && { fqbn: pick.fqbn, name: pick.label };
  }

  private async pickPort(ports: { address: string; label: string }[]) {
    const pick = await vscode.window.showQuickPick(
      ports.map((p) => ({ label: p.address, description: p.label })),
      { placeHolder: vscode.l10n.t('Which port is the Arduino connected to?') }
    );
    return pick?.label;
  }
}
