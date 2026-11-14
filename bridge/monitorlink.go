package main

import (
	"io"
	"net"
	"os"
	"strings"
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

// teeWriter writes to the bridge's terminal and, if there is one, to the
// monitor. A monitor that went away doesn't stop the output.
type teeWriter struct {
	out     *os.File
	monitor net.Conn
}

func (t teeWriter) Write(p []byte) (int, error) {
	if t.monitor != nil {
		t.monitor.SetWriteDeadline(time.Now().Add(time.Second))
		t.monitor.Write(p)
	}
	return t.out.Write(p)
}
