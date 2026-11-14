// Package handoff lets the bridge take the serial port over from a running
// ardudebug-monitor and hand it back at the end of the debug session.
//
// The monitor listens on a local TCP port and writes its number to File(port).
// The bridge connects and sends Release. The monitor closes the serial port
// and answers OK. From then on the connection carries plain bytes: the
// sketch's Serial output from the bridge to the monitor, typed input the
// other way. When the bridge closes the connection, the monitor opens the
// serial port again.
//
// The other way round: a monitor that the IDE opens during a session finds
// the port busy. The bridge listens too and writes its TCP port to
// BridgeFile(port). The monitor connects, sends Attach, gets OK, and the
// connection works as above. A Leonardo needs this: it drops off USB when
// it restarts for the debugger, and the IDE then opens the monitor anew.
package handoff

import (
	"os"
	"path/filepath"
	"strings"
)

// Release asks the monitor to close the serial port, Attach asks the bridge
// for the Serial output, OK confirms either.
const (
	Release = "RELEASE\n"
	Attach  = "ATTACH\n"
	OK      = "OK\n"
)

// File returns the path of the file with the monitor's TCP port for the
// serial port (e.g. /dev/ttyACM0 or COM5).
func File(port string) string {
	return path("ardudebug-monitor-", port)
}

// BridgeFile returns the path of the file with the bridge's TCP port during
// a debug session on the serial port.
func BridgeFile(port string) string {
	return path("ardudebug-bridge-", port)
}

func path(prefix, port string) string {
	name := strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '.' || r == '_' {
			return r
		}
		return '_'
	}, filepath.Base(port))
	return filepath.Join(os.TempDir(), prefix+name)
}
