package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

// Mitschnitt des gesamten Datenverkehrs fuer die Fehlersuche.
// Liegt im Temp-Ordner (Linux: /tmp/ardudebug-bridge.log) und wird
// bei jeder Sitzung neu angelegt.
var traceFile *os.File

func openTrace() {
	traceFile, _ = os.Create(filepath.Join(os.TempDir(), "ardudebug-bridge.log"))
}

// traced schreibt alles durch und protokolliert es mit Zeit und Richtung.
type traced struct {
	w   io.Writer
	tag string
}

func (t traced) Write(p []byte) (int, error) {
	if traceFile != nil {
		fmt.Fprintf(traceFile, "%s %s %q\n", time.Now().Format("15:04:05.000"), t.tag, p)
	}
	return t.w.Write(p)
}
