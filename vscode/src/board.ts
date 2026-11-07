// Board and port selection. Goal: normally the user doesn't have to choose anything.
// Boards with their own USB chip (Uno, Mega, Leonardo, usually /dev/ttyACM*) are
// recognized by arduino-cli from their USB IDs. Behind a CH340/FTDI (Nano, clones,
// /dev/ttyUSB*) any board can be connected: there we ask, and the choice is only
// remembered for this sketch and this port, not for the port in general.
// Board and port are shown separately in the status bar and can be selected there.
import * as vscode from 'vscode';
import { Cli, CORE } from './cli';

interface Board {
  fqbn: string;
  name: string;
}
interface Selection {
  port?: string;
  board?: Board;
  manual?: boolean; // board chosen by the user (not detected)
}
interface Port {
  address: string;
  label: string;
  board?: Board; // board detected by arduino-cli, mapped to ArduDebug
}

export class Boards {
  private boardItem: vscode.StatusBarItem;
  private portItem: vscode.StatusBarItem;
  private boards?: Board[];

  // sketchState: last selection per sketch
  constructor(private cli: Cli, private sketchState: vscode.Memento) {
    this.boardItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 51);
    this.boardItem.command = 'ardudebug.selectBoard';
    this.boardItem.tooltip = vscode.l10n.t('ArduDebug: select board');
    this.portItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 50);
    this.portItem.command = 'ardudebug.selectPort';
    this.portItem.tooltip = vscode.l10n.t('ArduDebug: select port');
    this.allBoards().catch(() => {}); // preload the board list
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
    this.show(sketch);
  }

  // Shows the stored selection immediately and checks in the background
  // whether the port is still connected.
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

  // Returns board and port for the sketch; detects them automatically or asks.
  async resolve(sketch: string): Promise<{ port: string; fqbn: string }> {
    const sel = this.get(sketch);
    const ports = await this.serialPorts();

    // Port: the stored one, else a clearly identifiable one, else ask
    let port = ports.find((p) => p.address === sel.port);
    if (!port) {
      const known = ports.filter((p) => p.board);
      if (known.length === 1) port = known[0];
      else if (ports.length === 1) port = ports[0];
      else if (ports.length > 1) port = await this.pickPort(ports);
    }
    if (!port) throw new Error(vscode.l10n.t('No Arduino found. Is the board connected via USB?'));

    const next = await this.boardFor(port, sel);
    if (!next) throw new Error(vscode.l10n.t('No board selected.'));
    await this.set(sketch, next);
    return { port: port.address, fqbn: next.board!.fqbn };
  }

  // Board for a port:
  // 1. detected by arduino-cli (Uno, Mega, Leonardo)
  // 2. chosen by the user for this sketch, or last used on exactly this port
  // 3. otherwise ask
  private async boardFor(port: Port, sel: Selection): Promise<Selection | undefined> {
    if (port.board) return { port: port.address, board: port.board, manual: false };
    if (sel.board && (sel.manual || sel.port === port.address)) {
      return { port: port.address, board: sel.board, manual: sel.manual };
    }
    const board = await this.pickBoard(port.address);
    return board && { port: port.address, board, manual: true };
  }

  async selectBoard(sketch: string) {
    const sel = this.get(sketch);
    const board = await this.pickBoard(sel.port);
    if (board) await this.set(sketch, { ...sel, board, manual: true });
  }

  async selectPort(sketch: string) {
    // The picker opens immediately; the ports (arduino-cli needs about a
    // second for them) appear as soon as they are available.
    const port = await this.pickPort(this.serialPorts());
    if (!port) return;
    const next = await this.boardFor(port, this.get(sketch));
    if (next) await this.set(sketch, next);
  }

  private async serialPorts(): Promise<Port[]> {
    const res = await this.cli.json<any>(['board', 'list']);
    const list: any[] = Array.isArray(res) ? res : res.detected_ports ?? [];
    const boards = await this.allBoards();
    return list
      .filter((p) => p.port?.protocol === 'serial')
      .map((p) => {
        // arduino:avr:uno -> ardudebug:avr:uno, if ArduDebug knows the board
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
    type Item = vscode.QuickPickItem & { board: Board };
    const items: Thenable<Item[]> = this.allBoards().then((boards) => boards.map((b) => ({ label: b.name, board: b })));
    const pick = await vscode.window.showQuickPick<Item>(
      items,
      {
        placeHolder: port
          ? vscode.l10n.t('Which board is connected to {0}?', port)
          : vscode.l10n.t('Which board is connected?'),
      }
    );
    return pick?.board && (await this.pickOptions(pick.board));
  }

  // Asks for board options like "Processor" on the Nano (old/new bootloader).
  // LTO is set by the extension itself, so it is not asked for.
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

  private async pickPort(ports: Port[] | Promise<Port[]>): Promise<Port | undefined> {
    const items = Promise.resolve(ports).then((list) => {
      if (!list.length) throw new Error(vscode.l10n.t('No Arduino found. Is the board connected via USB?'));
      return list.map((p) => ({ label: p.address, description: p.board?.name ?? p.label, port: p }));
    });
    const pick = await vscode.window.showQuickPick(items, {
      placeHolder: vscode.l10n.t('Which port is the Arduino connected to?'),
    });
    return pick?.port;
  }

}
