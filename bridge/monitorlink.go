package main

import (
	"bufio"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"

	"ardudebug-bridge/handoff"
)

// claimMonitor asks a running ardudebug-monitor for the serial port. It
// closes the port and from then on shows the sketch's Serial output that
// the bridge sends. Returns nil if no monitor has the port open.
func claimMonitor(port string) net.Conn {
	data, err := os.ReadFile(handoff.File(port))
	if err != nil {
		return nil
	}
	c, err := net.DialTimeout("tcp", "127.0.0.1:"+strings.TrimSpace(string(data)), time.Second)
	if err != nil {
		return nil // left over from a monitor that didn't end cleanly
	}
	c.SetDeadline(time.Now().Add(3 * time.Second))
	c.Write([]byte(handoff.Release))
	// read exactly the answer: anything after it is already typed input
	answer := make([]byte, len(handoff.OK))
	if _, err := io.ReadFull(c, answer); err != nil || string(answer) != handoff.OK {
		c.Close()
		return nil
	}
	c.SetDeadline(time.Time{})
	return c
}

// monitorSet: the Serial Monitors connected during the session. Output goes
// to all of them, input from each goes to the sketch.
type monitorSet struct {
	mu    sync.Mutex
	conns []net.Conn
	input func([]byte)
	ln    net.Listener
	file  string
}

var monitors = &monitorSet{}

// add takes a monitor connection.
func (m *monitorSet) add(c net.Conn) {
	m.mu.Lock()
	m.conns = append(m.conns, c)
	m.mu.Unlock()
	mark("Serial Monitor connected")
	go func() {
		buf := make([]byte, 256)
		for {
			n, err := c.Read(buf)
			if n > 0 {
				m.mu.Lock()
				f := m.input
				m.mu.Unlock()
				if f != nil {
					f(buf[:n])
				}
			}
			if err != nil {
				m.remove(c)
				return
			}
		}
	}()
}

func (m *monitorSet) remove(c net.Conn) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for i, x := range m.conns {
		if x == c {
			m.conns = append(m.conns[:i], m.conns[i+1:]...)
			c.Close()
			return
		}
	}
}

// setInput sets where typed characters go.
func (m *monitorSet) setInput(f func([]byte)) {
	m.mu.Lock()
	m.input = f
	m.mu.Unlock()
}

// send gives output to all monitors. A monitor that went away doesn't stop
// the output. Returns false if no monitor took it.
func (m *monitorSet) send(p []byte) bool {
	m.mu.Lock()
	conns := append([]net.Conn(nil), m.conns...)
	m.mu.Unlock()
	sent := false
	for _, c := range conns {
		c.SetWriteDeadline(time.Now().Add(time.Second))
		if _, err := c.Write(p); err != nil {
			m.remove(c)
		} else {
			sent = true
		}
	}
	return sent
}

// listen lets a monitor that the IDE opens during the session attach itself
// (see package handoff).
func (m *monitorSet) listen(port string) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return
	}
	m.mu.Lock()
	m.ln, m.file = ln, handoff.BridgeFile(port)
	m.mu.Unlock()
	os.WriteFile(m.file, []byte(strconv.Itoa(ln.Addr().(*net.TCPAddr).Port)), 0o644)
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			c.SetReadDeadline(time.Now().Add(2 * time.Second))
			line, err := bufio.NewReader(c).ReadString('\n')
			if err != nil || line != handoff.Attach {
				c.Close()
				continue
			}
			c.SetReadDeadline(time.Time{})
			if _, err := io.WriteString(c, handoff.OK); err != nil {
				c.Close()
				continue
			}
			m.add(c)
		}
	}()
}

// closeAll ends the session for the monitors: they open the port again.
// Call it after the serial port is closed.
func (m *monitorSet) closeAll() {
	m.mu.Lock()
	defer m.mu.Unlock()
	if m.ln != nil {
		m.ln.Close()
		os.Remove(m.file)
		m.ln = nil
	}
	for _, c := range m.conns {
		c.Close()
	}
	m.conns = nil
}

// teeWriter writes the sketch's output to the Serial Monitor, or to the
// bridge's terminal (gdb-server) while no monitor is connected.
type teeWriter struct {
	out *os.File
}

func (t teeWriter) Write(p []byte) (int, error) {
	if monitors.send(p) {
		return len(p), nil
	}
	return t.out.Write(p)
}
