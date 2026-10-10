// ardudebug-bridge: Ersatz für OpenOCD im ArduDebug-Board-Paket.
// Die Arduino IDE (Cortex-Debug) startet das Programm wie OpenOCD.
// Es sucht den Arduino per USB-Kennung und verbindet GDB (TCP) mit dem Stub (seriell).
package main

import (
	"fmt"
	"io"
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
	return "", fmt.Errorf("kein Arduino gefunden. Ist das USB-Kabel eingesteckt?")
}

func fail(msg string, err error) {
	fmt.Printf("Error: %s: %v\n", msg, err)
	os.Exit(1)
}

func main() {
	port := gdbPort(os.Args[1:])

	name, err := findArduino()
	if err != nil {
		fail("Arduino-Suche", err)
	}

	// Öffnen löst einen Reset aus; danach dem Bootloader Zeit lassen.
	ser, err := serial.Open(name, &serial.Mode{BaudRate: 115200})
	if err != nil {
		fail("Port "+name+" laesst sich nicht oeffnen (serieller Monitor offen?)", err)
	}
	defer ser.Close()
	time.Sleep(2 * time.Second)

	ln, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(port))
	if err != nil {
		fail("TCP-Port", err)
	}

	// Meldungen im OpenOCD-Stil, auf die Cortex-Debug wartet.
	fmt.Printf("Info : ArduDebug: Arduino an %s\n", name)
	fmt.Printf("Info : avr.cpu: hardware has 4 breakpoints, 0 watchpoints\n")
	fmt.Printf("Info : Listening on port %d for gdb connections\n", port)

	conn, err := ln.Accept()
	if err != nil {
		fail("GDB-Verbindung", err)
	}
	defer conn.Close()

	// In beide Richtungen durchreichen; endet, sobald eine Seite schließt.
	done := make(chan struct{}, 2)
	go func() { io.Copy(ser, conn); done <- struct{}{} }()
	go func() { forwardFiltered(conn, ser); done <- struct{}{} }()
	<-done
}
