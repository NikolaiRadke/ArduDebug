// Debug-Teil: Aus der Antwort von "arduino-cli debug --info" wird eine
// Konfiguration für den CDT-GDB-Adapter (gdbtarget) gebaut. Die Angaben stammen
// damit aus derselben platform.txt wie in der Arduino IDE – nichts doppelt pflegen.
import * as path from 'path';
import * as vscode from 'vscode';
import { GDBTargetDebugSession } from 'cdt-gdb-adapter';

export const DEBUG_TYPE = 'ardudebug';

export function buildConfig(info: any, sketchName: string): vscode.DebugConfiguration {
  const exe = process.platform === 'win32' ? '.exe' : '';
  const cortex = info.custom_configs?.['cortex-debug'] ?? {};
  // arduino-cli liefert das Präfix wie Cortex-Debug ohne Bindestrich ("avr"),
  // aus der platform.txt kommt es mit ("avr-"). Beides zulassen.
  const prefix = info.toolchain_prefix.endsWith('-') ? info.toolchain_prefix : info.toolchain_prefix + '-';

  // Startbefehle aus der platform.txt übernehmen, die reine Meldung ("echo") nicht.
  const initCommands: string[] = (cortex.overrideLaunchCommands ?? []).filter(
    (c: string) => !c.trim().startsWith('echo')
  );
  // Wie runToEntryPoint in der IDE: beim ersten "Weiter" in loop() anhalten.
  if (cortex.runToEntryPoint) initCommands.push(`tbreak ${cortex.runToEntryPoint}`);

  return {
    type: DEBUG_TYPE,
    request: 'launch',
    name: `ArduDebug: ${sketchName}`,
    program: info.executable,
    gdb: path.join(info.toolchain_path, `${prefix}gdb${exe}`),
    initCommands,
    target: {
      server: info.server_path,
      // serverArgs enthalten ardudebug_mcu/_fcpu/_port, die Bridge liest sie aus.
      serverParameters: cortex.serverArgs ?? [],
      // Die Bridge meldet sich wie OpenOCD: "Listening on port N for gdb connections"
      serverPortRegExp: 'Listening on port ([0-9]+)',
    },
  };
}

// Der Adapter läuft direkt im Extension-Prozess, ohne fremde Extension.
export const adapterFactory: vscode.DebugAdapterDescriptorFactory = {
  createDebugAdapterDescriptor() {
    return new vscode.DebugAdapterInlineImplementation(new GDBTargetDebugSession() as any);
  },
};
