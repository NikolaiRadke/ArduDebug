package main

import (
	"io"
	"sync"
)

// Eingaben fuer den Sketch: Was im Terminal "gdb-server" getippt wird,
// schreibt Cortex-Debug in die Standardeingabe der Bridge. Die Bridge
// schickt jedes Zeichen als 0x01 + Zeichen an den Stub, aber nur, solange
// das Programm laeuft (nach "c"). GDB selbst schickt dann nur 0x03, damit
// ist 0x01 eindeutig. Waehrend das Programm steht, wird gesammelt.
const inputMark = 0x01

type inputForwarder struct {
	mu      sync.Mutex
	cond    *sync.Cond
	running bool
	queue   []byte
	lastCR  bool
	ser     io.Writer
}

func newInputForwarder(ser io.Writer) *inputForwarder {
	f := &inputForwarder{ser: ser}
	f.cond = sync.NewCond(&f.mu)
	return f
}

// setRunning: true nach "c" von GDB, false nach einer Halt-Meldung.
func (f *inputForwarder) setRunning(r bool) {
	f.mu.Lock()
	f.running = r
	f.mu.Unlock()
	f.cond.Broadcast()
}

// add nimmt getippte Zeichen an. Zeilenenden werden zu "\n"
// vereinheitlicht (Windows schickt "\r\n").
func (f *inputForwarder) add(data []byte) {
	f.mu.Lock()
	for _, b := range data {
		switch {
		case b == '\r':
			f.queue = append(f.queue, '\n')
			f.lastCR = true
			continue
		case b == '\n' && f.lastCR:
			// zweite Haelfte von "\r\n"
		default:
			f.queue = append(f.queue, b)
		}
		f.lastCR = false
	}
	f.mu.Unlock()
	f.cond.Broadcast()
}

// run schickt gesammelte Zeichen, sobald das Programm laeuft.
func (f *inputForwarder) run() {
	for {
		f.mu.Lock()
		for !(f.running && len(f.queue) > 0) {
			f.cond.Wait()
		}
		data := f.queue
		f.queue = nil
		f.mu.Unlock()

		out := make([]byte, 0, 2*len(data))
		for _, b := range data {
			out = append(out, inputMark, b)
		}
		if _, err := f.ser.Write(out); err != nil {
			return
		}
	}
}

// readInput liest die Standardeingabe, bis sie endet.
func readInput(r io.Reader, f *inputForwarder) {
	buf := make([]byte, 256)
	for {
		n, err := r.Read(buf)
		if n > 0 {
			f.add(buf[:n])
		}
		if err != nil {
			return
		}
	}
}
