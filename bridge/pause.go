package main

import (
	"io"
	"sync"
	"time"
)

// Pausieren: Der Stub haelt nach einem Pause-Signal (0x03) erst vor dem
// naechsten loop()-Durchlauf an. Haengt das Programm woanders fest, kommt
// keine Halt-Meldung. Dann schickt die Bridge nach pauseTimeout ein zweites
// 0x03, und der Stub haelt sofort an.
var pauseTimeout = time.Second

type pauseGuard struct {
	mu    sync.Mutex
	timer *time.Timer
	ser   io.Writer
}

// interrupted: GDB hat Pause gedrueckt.
func (g *pauseGuard) interrupted() {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.timer != nil {
		g.timer.Stop()
	}
	g.timer = time.AfterFunc(pauseTimeout, func() {
		g.ser.Write([]byte{0x03})
	})
}

// stopped: Der Stub hat eine Halt-Meldung geschickt.
func (g *pauseGuard) stopped() {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.timer != nil {
		g.timer.Stop()
		g.timer = nil
	}
}

// forwardFromGDB reicht alles von GDB an den Stub weiter und erkennt dabei
// Pause-Signale. Ein 0x03 zaehlt nur ausserhalb von Paketen ($...#xx).
// onContinue wird aufgerufen, nachdem ein "c"-Paket (Fortsetzen) beim Stub ist,
// onReset nach "monitor reset" (qRcmd,7265736574). Solange onReset laeuft,
// wird nichts weitergereicht: GDBs naechste Pakete warten, bis das Board
// nach dem Reset wieder zuhoert.
func forwardFromGDB(stub io.Writer, gdb io.Reader, g *pauseGuard, onContinue, onReset func()) {
	buf := make([]byte, 512)
	inPacket, escape, checksum := false, false, 0
	var pkt []byte
	for {
		n, err := gdb.Read(buf)
		resumed, reset := false, false
		for _, b := range buf[:n] {
			switch {
			case checksum > 0:
				checksum--
			case inPacket && escape:
				escape = false
				pkt = appendShort(pkt, b)
			case inPacket && b == '}':
				escape = true
			case inPacket && b == '#':
				inPacket, checksum = false, 2
				if len(pkt) > 0 && (pkt[0] == 'c' || pkt[0] == 'C') {
					resumed = true
				}
				if string(pkt) == resetPacket {
					reset = true
				}
			case inPacket:
				pkt = appendShort(pkt, b)
			case b == '$':
				inPacket, pkt = true, pkt[:0]
			case b == 0x03:
				g.interrupted()
			}
		}
		if n > 0 {
			if _, werr := stub.Write(buf[:n]); werr != nil {
				return
			}
		}
		if resumed && onContinue != nil {
			onContinue()
		}
		if reset && onReset != nil {
			onReset()
		}
		if err != nil {
			return
		}
	}
}

// resetPacket ist GDBs "monitor reset" (Text "reset" in Hex).
const resetPacket = "qRcmd,7265736574"

// appendShort merkt sich nur den Anfang eines Pakets; mehr braucht die
// Erkennung nicht.
func appendShort(p []byte, b byte) []byte {
	if len(p) < 32 {
		p = append(p, b)
	}
	return p
}

// lockedWriter verhindert, dass zwei Goroutinen gleichzeitig auf den
// seriellen Port schreiben.
type lockedWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}
