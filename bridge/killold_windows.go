//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// Unter Windows kann eine übrig gebliebene Bridge den COM-Port festhalten.
// Nach dem Beenden gibt Windows den Port nicht immer sofort frei.
const portRetry = 2 * time.Second

// killOldBridges beendet andere laufende ardudebug-bridge.exe (nicht sich selbst).
func killOldBridges() {
	cmd := exec.Command("taskkill", "/F", "/IM", "ardudebug-bridge.exe",
		"/FI", fmt.Sprintf("PID ne %d", os.Getpid()))
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	_ = cmd.Run()
}
