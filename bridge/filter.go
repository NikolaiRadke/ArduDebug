package main

import (
	"bufio"
	"encoding/hex"
	"io"
)

// forwardFiltered passes the data from the stub on to GDB. Output packets
// of the program ($O<hex>#xx) are intercepted and written as text to the
// bridge's terminal, the way the serial monitor would show them.
// Late acknowledgements (0x02) of the stub don't go to GDB but to onReady.
func forwardFiltered(gdb io.Writer, stub io.Reader, out io.Writer, onStop, onReady func()) {
	r := bufio.NewReader(stub)
	for {
		b, err := r.ReadByte()
		if err != nil {
			return
		}
		if b == readyMark {
			if onReady != nil {
				onReady()
			}
			continue
		}
		if b != '$' {
			gdb.Write([]byte{b}) // e.g. '+' or '-'
			continue
		}
		// read the packet up to '#', then the two checksum characters
		pkt := []byte{'$'}
		for b != '#' {
			if b, err = r.ReadByte(); err != nil {
				return
			}
			pkt = append(pkt, b)
		}
		for i := 0; i < 2; i++ {
			if b, err = r.ReadByte(); err != nil {
				return
			}
			pkt = append(pkt, b)
		}
		payload := pkt[1 : len(pkt)-3]
		if len(payload) > 0 && (payload[0] == 'T' || payload[0] == 'S') {
			onStop() // stop reply of the stub
		}
		if text, ok := outputText(payload); ok {
			out.Write(text)
			continue
		}
		gdb.Write(pkt)
	}
}

// outputText recognizes "O" + hex text. "OK" is not an output packet.
func outputText(p []byte) ([]byte, bool) {
	if len(p) < 3 || p[0] != 'O' || (len(p)-1)%2 != 0 {
		return nil, false
	}
	text, err := hex.DecodeString(string(p[1:]))
	return text, err == nil
}
