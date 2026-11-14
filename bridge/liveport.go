package main

import (
	"sync"
	"time"

	"go.bug.st/serial"
)

// livePort is the serial port to the board, replaceable: a Leonardo
// drops off USB on a restart and comes back. Reads and
// writes wait meanwhile and then carry on with the new port.
type livePort struct {
	mu       sync.Mutex
	cond     *sync.Cond
	p        serial.Port
	swapping bool
}

func newLivePort(p serial.Port) *livePort {
	l := &livePort{p: p}
	l.cond = sync.NewCond(&l.mu)
	return l
}

// current returns the port, if need be only after a running replacement.
func (l *livePort) current() serial.Port {
	l.mu.Lock()
	defer l.mu.Unlock()
	for l.swapping {
		l.cond.Wait()
	}
	return l.p
}

// retry reports after an error on p whether there is a new port by now.
func (l *livePort) retry(p serial.Port) bool {
	l.mu.Lock()
	defer l.mu.Unlock()
	for l.swapping {
		l.cond.Wait()
	}
	return l.p != p
}

func (l *livePort) Read(b []byte) (int, error) {
	for {
		p := l.current()
		n, err := p.Read(b)
		if err == nil || !l.retry(p) {
			return n, err
		}
	}
}

func (l *livePort) Write(b []byte) (int, error) {
	for {
		p := l.current()
		n, err := p.Write(b)
		if err == nil || !l.retry(p) {
			return n, err
		}
	}
}

func (l *livePort) Close() error {
	return l.current().Close()
}

// restart sends the restart command with send, waits until the port
// disappears, and opens it again after the board is back on USB.
func (l *livePort) restart(name string, send func(serial.Port)) (string, error) {
	l.mu.Lock()
	old := l.p
	l.swapping = true
	l.mu.Unlock()

	send(old)
	for deadline := time.Now().Add(2 * time.Second); time.Now().Before(deadline) && portPresent(name); {
		time.Sleep(50 * time.Millisecond)
	}
	old.Close()
	mark("port lost, board restarts")
	p, newName, err := reopen(name)
	if err == nil {
		drain(p)
	}

	l.mu.Lock()
	if err == nil {
		l.p = p
		mark("port open again on %s", newName)
	}
	l.swapping = false
	l.cond.Broadcast()
	l.mu.Unlock()
	return newName, err
}
