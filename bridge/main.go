// ardudebug-bridge: Ersatz für OpenOCD im ArduDebug-Board-Paket.
// Die Arduino IDE (Cortex-Debug) startet das Programm wie OpenOCD.
// Es sucht den Arduino per USB-Kennung und verbindet GDB (TCP) mit dem Stub (seriell).
package main

import (
	"errors"
	"fmt"
	"net"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// USB-Hersteller-Kennungen: Arduino, Arduino.org, CH340-Klone, FTDI
var knownVIDs = []string{"2341", "2A03", "1A86", "0403"}

// gdbPort liest "gdb_port N" aus den Parametern, die Cortex-Debug übergibt.
func gdbPort(args []string) int {
	m := regexp.MustCompile(`gdb_port\s+(\d+)`).FindStringSubmatch(strings.Join(args, " "))
	if m != nil {
		if p, err := strconv.Atoi(m[1]); err == nil {
			return p
		}
	}
	return 50000
}

// findArduino sucht den ersten seriellen USB-Port mit bekannter Hersteller-Kennung.
func findArduino() (string, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", err
	}
	for _, p := range ports {
		if !p.IsUSB {
			continue
		}
		for _, vid := range knownVIDs {
			if strings.EqualFold(p.VID, vid) {
				return p.Name, nil
			}
		}
	}
	return "", fmt.Errorf("no Arduino found. Is the USB cable plugged in?")
}

func fail(msg string, err error) {
	fmt.Printf("Error: %s: %v\n", msg, err)
	os.Exit(1)
}

// openPort öffnet den seriellen Port. Ist er belegt, wird es portRetry lang
// erneut versucht (nur Windows, nach dem Beenden einer alten Bridge).
func openPort(name string) (serial.Port, error) {
	deadline := time.Now().Add(portRetry)
	for {
		ser, err := serial.Open(name, &serial.Mode{BaudRate: 115200})
		var pe *serial.PortError
		if err == nil || !errors.As(err, &pe) || pe.Code() != serial.PortBusy || time.Now().After(deadline) {
			return ser, err
		}
		time.Sleep(100 * time.Millisecond)
	}
}

// openFailed erklärt, warum der serielle Port nicht aufging, und beendet.
func openFailed(name string, err error) {
	var pe *serial.PortError
	if errors.As(err, &pe) {
		switch pe.Code() {
		case serial.PortBusy:
			fmt.Printf("Error: port %s is busy. Close the Serial Monitor and other programs using the port. "+
				"On Windows, also end any leftover ardudebug-bridge.exe in the Task Manager.\n", name)
			os.Exit(1)
		case serial.PermissionDenied:
			fmt.Printf("Error: no permission for port %s. On Linux, add your user to the dialout group "+
				"(sudo usermod -aG dialout $USER) and log in again.\n", name)
			os.Exit(1)
		}
	}
	fail("cannot open port "+name, err)
}

func main() {
	port := gdbPort(os.Args[1:])

	name, err := findArduino()
	if err != nil {
		fail("Arduino search", err)
	}

	// Öffnen löst einen Reset aus; danach dem Bootloader Zeit lassen.
	killOldBridges()
	ser, err := openPort(name)
	if err != nil {
		openFailed(name, err)
	}
	defer ser.Close()
	time.Sleep(2 * time.Second)

	ln, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(port))
	if err != nil {
		fail("TCP port", err)
	}

	// Meldungen im OpenOCD-Stil, auf die Cortex-Debug wartet.
	fmt.Printf("Info : ArduDebug: Arduino on %s\n", name)
	fmt.Printf("Info : avr.cpu: hardware has 4 breakpoints, 0 watchpoints\n")
	fmt.Printf("Info : Listening on port %d for gdb connections\n", port)

	conn, err := ln.Accept()
	if err != nil {
		fail("GDB connection", err)
	}
	defer conn.Close()

	// In beide Richtungen durchreichen; endet, sobald eine Seite schließt.
	openTrace()
	done := make(chan struct{}, 2)
	serOut := &lockedWriter{w: traced{ser, "GDB->"}}
	guard := &pauseGuard{ser: serOut}
	go func() { forwardFromGDB(serOut, conn, guard); done <- struct{}{} }()
	go func() {
		forwardFiltered(traced{conn, "<-AVR"}, ser, traced{os.Stdout, "OUT  "}, guard.stopped)
		done <- struct{}{}
	}()
	<-done
}
