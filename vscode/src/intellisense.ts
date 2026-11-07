// Code completion: writes a c_cpp_properties.json so the C/C++ extension finds
// Arduino.h and friends. The values come from the build properties of the
// selected board. Without the C/C++ extension this has no effect, but no harm.
// TODO: use the custom configuration provider of the C/C++ extension instead of a file.
import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';
import { Cli } from './cli';

export async function writeIntelliSense(cli: Cli, sketch: string, fqbn: string) {
  const ws = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(sketch));
  if (!ws) return;
  const file = path.join(ws.uri.fsPath, '.vscode', 'c_cpp_properties.json');

  // Don't overwrite a file the user created.
  if (fs.existsSync(file) && !fs.readFileSync(file, 'utf8').includes('"ArduDebug"')) return;

  const props = parseProps(await cli.run(['compile', '--show-properties', '-b', fqbn, sketch], false));
  const userDir = (await cli.run(['config', 'get', 'directories.user'], false)).trim();
  const config = {
    configurations: [
      {
        name: 'ArduDebug',
        compilerPath: path.join(props['compiler.path'], props['compiler.cpp.cmd']),
        compilerArgs: [`-mmcu=${props['build.mcu']}`],
        includePath: [
          props['build.core.path'],
          props['build.variant.path'],
          path.join(props['runtime.platform.path'], 'libraries', '**'),
          path.join(userDir, 'libraries', '**'),
        ],
        forcedInclude: [path.join(props['build.core.path'], 'Arduino.h')],
        defines: [
          `F_CPU=${props['build.f_cpu']}`,
          `ARDUINO=${props['runtime.ide.version']}`,
          `ARDUINO_${props['build.board']}`,
          'ARDUINO_ARCH_AVR',
        ],
        cStandard: 'gnu11',
        cppStandard: 'gnu++11',
      },
    ],
    version: 4,
  };
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, JSON.stringify(config, null, 2));
}

function parseProps(text: string): Record<string, string> {
  const props: Record<string, string> = {};
  for (const line of text.split(/\r?\n/)) {
    const i = line.indexOf('=');
    if (i > 0) props[line.slice(0, i)] = line.slice(i + 1);
  }
  return props;
}
