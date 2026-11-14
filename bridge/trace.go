package main

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"time"
)

// Trace of all traffic for troubleshooting.
// Lives in the temp folder (Linux: /tmp/ardudebug-bridge.log) and is
// created anew for every session.
var traceFile *os.File

func openTrace() {
	traceFile, _ = os.Create(filepath.Join(os.TempDir(), "ardudebug-bridge.log"))
}

// traced writes everything through and logs it with time and direction.
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

// mark writes a time mark to the log, to see where the time goes
// during startup.
func mark(format string, args ...any) {
	if traceFile != nil {
		fmt.Fprintf(traceFile, "%s MARK  %s\n", time.Now().Format("15:04:05.000"), fmt.Sprintf(format, args...))
	}
}
