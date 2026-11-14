package main

import (
	"io"
	"sync"
)

// Input for the sketch: what is typed in the "gdb-server" terminal,
// Cortex-Debug writes to the bridge's standard input. The bridge
// sends each character as 0x01 + character to the stub, but only while
// the program is running (after "c"). GDB itself then sends only 0x03, so
// 0x01 is unambiguous. While the program is stopped, input is collected.
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

// setRunning: true after "c" from GDB, false after a stop reply.
func (f *inputForwarder) setRunning(r bool) {
	f.mu.Lock()
	f.running = r
	f.mu.Unlock()
	f.cond.Broadcast()
}

// add takes typed characters. Line endings are normalized to "\n"
// (Windows sends "\r\n").
func (f *inputForwarder) add(data []byte) {
	f.mu.Lock()
	for _, b := range data {
		switch {
		case b == '\r':
			f.queue = append(f.queue, '\n')
			f.lastCR = true
			continue
		case b == '\n' && f.lastCR:
			// second half of "\r\n"
		default:
			f.queue = append(f.queue, b)
		}
		f.lastCR = false
	}
	f.mu.Unlock()
	f.cond.Broadcast()
}

// run sends collected characters as soon as the program is running.
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

// readInput reads the standard input until it ends.
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
