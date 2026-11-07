// Debugging: builds a configuration for the CDT GDB adapter (gdbtarget) from the
// output of "arduino-cli debug --info". So everything comes from the same
// platform.txt as in the Arduino IDE, nothing is maintained twice.
import * as path from 'path';
import * as vscode from 'vscode';
import { GDBTargetDebugSession } from 'cdt-gdb-adapter';

export const DEBUG_TYPE = 'ardudebug';

export function buildConfig(info: any, sketchName: string): vscode.DebugConfiguration {
  const exe = process.platform === 'win32' ? '.exe' : '';
  const cortex = info.custom_configs?.['cortex-debug'] ?? {};
  // arduino-cli returns the prefix without a trailing dash like Cortex-Debug ("avr"),
  // platform.txt has it with dash ("avr-"). Accept both.
  const prefix = info.toolchain_prefix.endsWith('-') ? info.toolchain_prefix : info.toolchain_prefix + '-';

  // Take the launch commands from platform.txt, except the plain message ("echo").
  const initCommands: string[] = (cortex.overrideLaunchCommands ?? []).filter(
    (c: string) => !c.trim().startsWith('echo')
  );
  // Like runToEntryPoint in the IDE: stop in loop() on the first "Continue".
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
      // serverArgs contain ardudebug_mcu/_fcpu/_port, read by the bridge.
      serverParameters: cortex.serverArgs ?? [],
      // The bridge reports like OpenOCD: "Listening on port N for gdb connections"
      serverPortRegExp: 'Listening on port ([0-9]+)',
    },
  };
}

// Files of the Arduino core (main.cpp, wiring.c, HardwareSerial.cpp ...)
const CORE_FILE = /[\\/]cores[\\/]arduino[\\/]/;

// CDT adapter with one addition: if a step ends in the Arduino core, e.g. in
// main() after "Step Out" of loop(), another step is taken at once, before VS
// Code learns about it. The user never sees main.cpp and gets back to the start
// of loop(). (Stepping into core functions is already prevented by
// "skip -gfi" from platform.txt.)
class ArduDebugSession extends GDBTargetDebugSession {
  private autoSteps = 0;

  protected handleGDBStopped(result: any): void {
    const stepped = result.reason === 'end-stepping-range' || result.reason === 'function-finished';
    const file: string = result.frame?.fullname ?? result.frame?.file ?? '';
    if (stepped && CORE_FILE.test(file) && this.autoSteps < 50) {
      this.autoSteps++;
      this.gdb.sendCommand('-exec-step').catch(() => super.handleGDBStopped(result));
      return;
    }
    this.autoSteps = 0;
    super.handleGDBStopped(result);
  }
}

// The adapter runs inside the extension process, no other extension needed.
export const adapterFactory: vscode.DebugAdapterDescriptorFactory = {
  createDebugAdapterDescriptor() {
    return new vscode.DebugAdapterInlineImplementation(new ArduDebugSession() as any);
  },
};
