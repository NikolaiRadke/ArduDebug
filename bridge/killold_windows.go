//go:build windows

package main

import (
	"fmt"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// On Windows a leftover bridge can hold on to the COM port.
// After it ends, Windows doesn't always release the port right away.
const portRetry = 2 * time.Second

// killOldBridges ends other running ardudebug-bridge.exe (not itself).
func killOldBridges() {
	cmd := exec.Command("taskkill", "/F", "/IM", "ardudebug-bridge.exe",
		"/FI", fmt.Sprintf("PID ne %d", os.Getpid()))
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: 0x08000000} // CREATE_NO_WINDOW
	_ = cmd.Run()
}
