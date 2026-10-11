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
// onContinue wird aufgerufen, nachdem ein "c"-Paket (Fortsetzen) beim Stub ist.
// "monitor reset" (qRcmd,7265736574) erreicht den Stub nicht: Die Bridge
// erledigt den Reset selbst in onReset. Solange onReset laeuft, wird nichts
// weitergereicht; GDBs naechste Pakete warten, bis das Board wieder zuhoert.
func forwardFromGDB(stub io.Writer, gdb io.Reader, g *pauseGuard, onContinue, onReset func()) {
	buf := make([]byte, 512)
	inPacket, escape, checksum := false, false, 0
	var pkt []byte
	var out, carry []byte
	pktStart, dropPkt := 0, false
	for {
		n, err := gdb.Read(buf)
		resumed, reset := false, false
		// Ein angefangenes Paket aus dem letzten Lesen kommt wieder nach vorn.
		out = append(out[:0], carry...)
		carry = carry[:0]
		pktStart = 0
		for _, b := range buf[:n] {
			out = append(out, b)
			switch {
			case checksum > 0:
				checksum--
				if checksum == 0 && dropPkt {
					out, dropPkt = out[:pktStart], false // Reset-Paket nicht weitergeben
				}
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
					reset, dropPkt = true, true
				}
			case inPacket:
				pkt = appendShort(pkt, b)
			case b == '$':
				inPacket, pkt, pktStart = true, pkt[:0], len(out)-1
			case b == 0x03:
				g.interrupted()
			}
		}
		// Ein noch unvollstaendiges Paket zurueckhalten, bis es komplett ist,
		// damit ein Reset-Paket nie halb beim Stub ankommt.
		if (inPacket || checksum > 0) && err == nil {
			carry = append(carry, out[pktStart:]...)
			out = out[:pktStart]
		}
		if len(out) > 0 {
			if _, werr := stub.Write(out); werr != nil {
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
