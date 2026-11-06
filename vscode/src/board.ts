// Board- und Port-Wahl. Ziel: Der Nutzer muss im Normalfall nichts wählen.
// Ein originales Board erkennt arduino-cli an der USB-Kennung. Bei Klonen
// (CH340 usw.) wird einmal nach dem Board gefragt; die Wahl wird pro Port gemerkt.
// Board und Port stehen getrennt in der Statusleiste und sind dort einzeln wählbar.
import * as vscode from 'vscode';
import { Cli, CORE } from './cli';

interface Board {
  fqbn: string;
  name: string;
}
interface Selection {
  port?: string;
  board?: Board;
}
interface Port {
  address: string;
  label: string;
  board?: Board; // von arduino-cli erkanntes Board, auf ArduDebug umgesetzt
}

export class Boards {
  private boardItem: vscode.StatusBarItem;
  private portItem: vscode.StatusBarItem;
  private boards?: Board[];

  // sketchState: letzte Wahl pro Sketch; globalState: Board pro Port (Klone)
  constructor(private cli: Cli, private sketchState: vscode.Memento, private globalState: vscode.Memento) {
    this.boardItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 51);
    this.boardItem.command = 'ardudebug.selectBoard';
    this.boardItem.tooltip = vscode.l10n.t('ArduDebug: select board');
    this.portItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
    this.portItem.command = 'ardudebug.selectPort';
    this.portItem.tooltip = vscode.l10n.t('ArduDebug: select port');
  }

  dispose() {
    this.boardItem.dispose();
    this.portItem.dispose();
  }

  private get(sketch: string): Selection {
    return this.sketchState.get<Selection>('selection:' + sketch) ?? {};
  }

  private async set(sketch: string, sel: Selection) {
    await this.sketchState.update('selection:' + sketch, sel);
    if (sel.port && sel.board) await this.remember(sel.port, sel.board);
    this.show(sketch);
  }

  private remembered(port: string): Board | undefined {
    return this.globalState.get<Record<string, Board>>('portBoards', {})[port];
  }

  private async remember(port: string, board: Board) {
    const map = this.globalState.get<Record<string, Board>>('portBoards', {});
    await this.globalState.update('portBoards', { ...map, [port]: board });
  }

  // Zeigt die gespeicherte Wahl sofort an und prüft im Hintergrund, ob der
  // Port noch angeschlossen ist.
  show(sketch?: string) {
    if (!sketch) {
      this.boardItem.hide();
      this.portItem.hide();
      return;
    }
    const sel = this.get(sketch);
    this.boardItem.text = `$(circuit-board) ${sel.board?.name ?? vscode.l10n.t('Board?')}`;
    this.portItem.text = `$(plug) ${sel.port ?? vscode.l10n.t('Port?')}`;
    this.boardItem.show();
    this.portItem.show();
    if (sel.port) {
      this.serialPorts()
        .then((ports) => {
          if (!ports.some((p) => p.address === sel.port)) {
            this.portItem.text = `$(debug-disconnect) ${sel.port} (${vscode.l10n.t('not connected')})`;
          }
        })
        .catch(() => {});
    }
  }

  // Liefert Board und Port für den Sketch; erkennt sie automatisch oder fragt nach.
  async resolve(sketch: string): Promise<{ port: string; fqbn: string }> {
    const sel = this.get(sketch);
    const ports = await this.serialPorts();

    // Port: gemerkter, sonst eindeutig erkennbarer, sonst fragen
    let port = ports.find((p) => p.address === sel.port);
    if (!port) {
      const known = ports.filter((p) => p.board);
      if (known.length === 1) port = known[0];
      else if (ports.length === 1) port = ports[0];
      else if (ports.length > 1) port = await this.pickPort(ports);
    }
    if (!port) throw new Error(vscode.l10n.t('No Arduino found. Is the board connected via USB?'));

    const board = await this.boardFor(port, sel);
    if (!board) throw new Error(vscode.l10n.t('No board selected.'));
    await this.set(sketch, { port: port.address, board });
    return { port: port.address, fqbn: board.fqbn };
  }

  // Board für einen Port: erkannt > zuletzt für diesen Sketch und Port > für den Port gemerkt > fragen
  private async boardFor(port: Port, sel: Selection): Promise<Board | undefined> {
    if (port.board) return port.board;
    if (sel.board && sel.port === port.address) return sel.board;
    return this.remembered(port.address) ?? (await this.pickBoard(port.address));
  }

  async selectBoard(sketch: string) {
    const sel = this.get(sketch);
    const board = await this.pickBoard(sel.port);
    if (board) await this.set(sketch, { ...sel, board });
  }

  async selectPort(sketch: string) {
    const ports = await this.serialPorts();
    if (!ports.length) throw new Error(vscode.l10n.t('No Arduino found. Is the board connected via USB?'));
    const port = await this.pickPort(ports);
    if (!port) return;
    const board = await this.boardFor(port, this.get(sketch));
    if (board) await this.set(sketch, { port: port.address, board });
  }

  private async serialPorts(): Promise<Port[]> {
    const res = await this.cli.json<any>(['board', 'list']);
    const list: any[] = Array.isArray(res) ? res : res.detected_ports ?? [];
    const boards = await this.allBoards();
    return list
      .filter((p) => p.port?.protocol === 'serial')
      .map((p) => {
        // arduino:avr:uno -> ardudebug:avr:uno, sofern ArduDebug das Board kennt
        const fqbn = p.matching_boards?.[0]?.fqbn?.replace(/^arduino:avr:/, CORE + ':');
        return {
          address: p.port.address,
          label: p.port.label ?? p.port.address,
          board: boards.find((b) => b.fqbn === fqbn),
        };
      });
  }

  private async allBoards(): Promise<Board[]> {
    if (!this.boards) {
      const res = await this.cli.json<any>(['board', 'listall', CORE]);
      this.boards = (res.boards ?? [])
        .filter((b: any) => !b.is_hidden)
        .map((b: any) => ({ fqbn: b.fqbn, name: b.name }));
    }
    return this.boards!;
  }

  private async pickBoard(port?: string): Promise<Board | undefined> {
    const pick = await vscode.window.showQuickPick(
      (await this.allBoards()).map((b) => ({ label: b.name, board: b })),
      {
        placeHolder: port
          ? vscode.l10n.t('Which board is connected to {0}?', port)
          : vscode.l10n.t('Which board is connected?'),
      }
    );
    return pick?.board && (await this.pickOptions(pick.board));
  }

  // Board-Optionen wie "Prozessor" beim Nano (alter/neuer Bootloader) abfragen.
  // LTO setzt die Extension selbst, danach wird nicht gefragt.
  private async pickOptions(board: Board): Promise<Board | undefined> {
    const details = await this.cli.json<any>(['board', 'details', '-b', board.fqbn]);
    const opts: string[] = [];
    let name = board.name;
    for (const o of details.config_options ?? []) {
      if (o.option === 'lto' || (o.values ?? []).length < 2) continue;
      const items: (vscode.QuickPickItem & { value: string })[] = o.values.map((v: any) => ({
        label: v.value_label,
        value: v.value,
        picked: v.selected,
      }));
      const pick = await vscode.window.showQuickPick(items, { placeHolder: `${board.name}: ${o.option_label}` });
      if (!pick) return undefined;
      opts.push(`${o.option}=${pick.value}`);
      name += ` (${pick.label})`;
    }
    return { fqbn: opts.length ? `${board.fqbn}:${opts.join(',')}` : board.fqbn, name };
  }

  private async pickPort(ports: Port[]): Promise<Port | undefined> {
    const pick = await vscode.window.showQuickPick(
      ports.map((p) => ({
        label: p.address,
        description: p.board?.name ?? this.remembered(p.address)?.name ?? p.label,
        port: p,
      })),
      { placeHolder: vscode.l10n.t('Which port is the Arduino connected to?') }
    );
    return pick?.port;
  }
}
