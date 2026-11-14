package main

import (
	"io"
	"sync"
	"time"
)

// Pausing: after a pause signal (0x03) the stub only stops before the
// next pass of loop(). If the program is stuck somewhere else, no
// stop reply comes. Then the bridge sends a second 0x03 after pauseTimeout,
// and the stub stops right away.
var pauseTimeout = time.Second

type pauseGuard struct {
	mu    sync.Mutex
	timer *time.Timer
	ser   io.Writer
}

// interrupted: GDB pressed pause.
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

// stopped: the stub sent a stop reply.
func (g *pauseGuard) stopped() {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.timer != nil {
		g.timer.Stop()
		g.timer = nil
	}
}

// forwardFromGDB passes everything from GDB on to the stub and detects
// pause signals. A 0x03 only counts outside of packets ($...#xx).
// onContinue is called after a "c" packet (continue) has reached the stub.
// "monitor reset" (qRcmd,7265736574) doesn't reach the stub: the bridge
// does the reset itself in onReset. While onReset runs, nothing is
// passed on; GDB's next packets wait until the board listens again.
func forwardFromGDB(stub io.Writer, gdb io.Reader, g *pauseGuard, onContinue, onReset func()) {
	buf := make([]byte, 512)
	inPacket, escape, checksum := false, false, 0
	var pkt []byte
	var out, carry []byte
	pktStart, dropPkt := 0, false
	for {
		n, err := gdb.Read(buf)
		resumed, reset := false, false
		// A packet started in the last read is moved back to the front.
		out = append(out[:0], carry...)
		carry = carry[:0]
		pktStart = 0
		for _, b := range buf[:n] {
			out = append(out, b)
			switch {
			case checksum > 0:
				checksum--
				if checksum == 0 && dropPkt {
					out, dropPkt = out[:pktStart], false // don't pass on the reset packet
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
		// Hold back an incomplete packet until it is complete,
		// so that a reset packet never reaches the stub half done.
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

// resetPacket is GDB's "monitor reset" (text "reset" in hex).
const resetPacket = "qRcmd,7265736574"

// appendShort only keeps the start of a packet; detection doesn't
// need more.
func appendShort(p []byte, b byte) []byte {
	if len(p) < 32 {
		p = append(p, b)
	}
	return p
}

// lockedWriter prevents two goroutines from writing to the
// serial port at the same time.
type lockedWriter struct {
	mu sync.Mutex
	w  io.Writer
}

func (l *lockedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	return l.w.Write(p)
}
