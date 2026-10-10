package main

import (
	"bufio"
	"encoding/hex"
	"io"
	"os"
)

// forwardFiltered reicht die Daten vom Stub an GDB weiter. Ausgabepakete
// des Programms ($O<hex>#xx) werden abgefangen und als Text ins Terminal
// der Bridge geschrieben, so wie der serielle Monitor sie zeigen wuerde.
func forwardFiltered(gdb io.Writer, stub io.Reader) {
	r := bufio.NewReader(stub)
	for {
		b, err := r.ReadByte()
		if err != nil {
			return
		}
		if b != '$' {
			gdb.Write([]byte{b}) // z. B. '+' oder '-'
			continue
		}
		// Paket bis '#' lesen, dann die zwei Pruefsummen-Zeichen
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
		if text, ok := outputText(payload); ok {
			os.Stdout.Write(text)
			continue
		}
		gdb.Write(pkt)
	}
}

// outputText erkennt "O" + Hex-Text. "OK" ist kein Ausgabepaket.
func outputText(p []byte) ([]byte, bool) {
	if len(p) < 3 || p[0] != 'O' || (len(p)-1)%2 != 0 {
		return nil, false
	}
	text, err := hex.DecodeString(string(p[1:]))
	return text, err == nil
}
