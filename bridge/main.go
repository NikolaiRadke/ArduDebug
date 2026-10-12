// ardudebug-bridge: Ersatz für OpenOCD im ArduDebug-Board-Paket.
// Die Arduino IDE (Cortex-Debug) startet das Programm wie OpenOCD.
// Es sucht den Arduino per USB-Kennung und verbindet GDB (TCP) mit dem Stub (seriell).
package main

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// USB-Hersteller-Kennungen: Arduino, Arduino.org, CH340-Klone, FTDI
var knownVIDs = []string{"2341", "2A03", "1A86", "0403"}

// USB-Produktkennungen der Boards mit eigenem USB (ATmega32U4): Leonardo,
// Micro. Sie starten fuer den Debugger ueber den Stub neu, nicht ueber DTR.
var nativeUSBPIDs = []string{"8036", "8037"}

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

// idePort liest "ardudebug_port PORT" aus den Parametern: der Port, den
// der Nutzer in der IDE gewaehlt hat (leer, falls nicht uebergeben).
func idePort(args []string) string {
	m := regexp.MustCompile(`ardudebug_port\s+(\S+)`).FindStringSubmatch(strings.Join(args, " "))
	if m != nil && !strings.Contains(m[1], "{") && !strings.HasPrefix(m[1], "ardudebug_") {
		return m[1]
	}
	return ""
}

// ideMCU liest "ardudebug_mcu MCU" (build.mcu des gewaehlten Boards).
func ideMCU(args []string) string {
	m := regexp.MustCompile(`ardudebug_mcu\s+(\S+)`).FindStringSubmatch(strings.Join(args, " "))
	if m != nil && !strings.Contains(m[1], "{") {
		return m[1]
	}
	return ""
}

// drain verwirft, was noch vom Board kommt: Ein Leonardo kann Bytes aus
// einer frueheren Sitzung in seinem USB-Puffer haben (etwa eine alte
// Quittung), die erst nach dem Oeffnen ankommen.
func drain(ser serial.Port) {
	ser.SetReadTimeout(20 * time.Millisecond)
	defer ser.SetReadTimeout(serial.NoTimeout)
	buf := make([]byte, 64)
	for deadline := time.Now().Add(100 * time.Millisecond); time.Now().Before(deadline); {
		if n, err := ser.Read(buf); err != nil || n == 0 {
			return
		}
	}
}

// Welche Boards findBoard in Frage kommen.
const (
	anyBoard    = iota
	nativeUSB   // nur mit eigenem USB (Leonardo, Micro)
	usbSerialIC // nur mit USB-Seriell-Chip (Uno, Nano)
)

// findBoard sucht das Board: bevorzugt den Port prefer (aus der IDE),
// sonst den ersten seriellen USB-Port mit bekannter Hersteller-Kennung
// und passender Art (kind). native meldet, ob das Board eigenes USB hat.
func findBoard(prefer string, kind int) (name string, native bool, err error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", false, err
	}
	isNative := func(p *enumerator.PortDetails) bool {
		for _, pid := range nativeUSBPIDs {
			if strings.EqualFold(p.PID, pid) {
				return true
			}
		}
		return false
	}
	isArduino := func(p *enumerator.PortDetails) bool {
		for _, vid := range knownVIDs {
			if p.IsUSB && strings.EqualFold(p.VID, vid) {
				return true
			}
		}
		return false
	}
	fits := func(p *enumerator.PortDetails) bool {
		switch kind {
		case nativeUSB:
			return isNative(p)
		case usbSerialIC:
			return !isNative(p)
		}
		return true
	}
	for _, p := range ports {
		if p.Name == prefer && fits(p) {
			return p.Name, isNative(p), nil
		}
	}
	for _, p := range ports {
		if isArduino(p) && fits(p) {
			return p.Name, isNative(p), nil
		}
	}
	return "", false, fmt.Errorf("no Arduino found. Is the USB cable plugged in?")
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

// announce schickt nach dem Reset alle 10 ms das Kennbyte 0x02 ("Debugger
// kommt"). Der Stub haelt beim ersten davon direkt nach debug_init() an und
// wartet auf GDB; der Sketch beginnt also erst mit dem Debugger. Unbekannte
// Zeichen lassen ausserdem den Bootloader sofort den Sketch starten.
// Die zurueckgegebene Funktion beendet das Senden und wartet darauf.
func announce(ser io.Writer) func() {
	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		t := time.NewTicker(10 * time.Millisecond)
		defer t.Stop()
		for {
			select {
			case <-stop:
				return
			case <-t.C:
				ser.Write([]byte{0x02})
			}
		}
	}()
	// Nicht ewig warten: Liest das Board nicht mit (Leonardo ohne Debug-
	// Build), blockiert das Schreiben irgendwann.
	return func() {
		close(stop)
		select {
		case <-done:
		case <-time.After(200 * time.Millisecond):
		}
	}
}

// readyMark ist die Quittung des Stubs auf das Kennbyte: Er steht jetzt vor
// setup() und wartet auf GDB.
const readyMark = 0x02

// restartMark bittet den Stub eines Leonardo, neu zu starten.
const restartMark = 0x04

// readyTimeout: So lange wartet die Bridge hoechstens auf die Quittung.
var readyTimeout = 3 * time.Second

// waitReady liest den Port, bis die Quittung kommt (true) oder readyTimeout
// verstrichen ist (false). Andere Zeichen, etwa vom Bootloader, verfallen.
// lost meldet, dass der Port verschwunden ist: Ein Leonardo startet neu,
// wenn sich der Debugger meldet, und meldet sich am USB neu an.
func waitReady(ser serial.Port, name string) (ready, lost bool) {
	ser.SetReadTimeout(50 * time.Millisecond)
	defer ser.SetReadTimeout(serial.NoTimeout)
	deadline := time.Now().Add(readyTimeout)
	nextCheck := time.Now()
	buf := make([]byte, 64)
	for time.Now().Before(deadline) {
		n, err := ser.Read(buf)
		if err != nil {
			return false, true
		}
		for _, b := range buf[:n] {
			if b == readyMark {
				return true, false
			}
		}
		if time.Now().After(nextCheck) {
			nextCheck = time.Now().Add(250 * time.Millisecond)
			if !portPresent(name) {
				return false, true
			}
		}
	}
	return false, false
}

// portPresent: Ist der Port noch im System?
func portPresent(name string) bool {
	ports, err := serial.GetPortsList()
	if err != nil {
		return true
	}
	for _, p := range ports {
		if p == name {
			return true
		}
	}
	return false
}

// reconnectTimeout: So lange darf ein Leonardo für den Neustart brauchen.
var reconnectTimeout = 10 * time.Second

// reopen wartet, bis sich das Board nach einem Neustart wieder am USB
// meldet, und öffnet den Port erneut. Andere angeschlossene Arduinos bleiben
// unberuehrt: bevorzugt der alte Name, sonst ein Board mit eigenem USB.
func reopen(prefer string) (serial.Port, string, error) {
	deadline := time.Now().Add(reconnectTimeout)
	var lastErr error
	for time.Now().Before(deadline) {
		time.Sleep(200 * time.Millisecond)
		name, _, err := findBoard(prefer, nativeUSB)
		if err != nil {
			lastErr = err
			continue
		}
		ser, err := openPort(name)
		if err != nil {
			lastErr = err
			continue
		}
		return ser, name, nil
	}
	return nil, "", lastErr
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
	openTrace()
	mark("bridge started")
	port := gdbPort(os.Args[1:])

	mcu := ideMCU(os.Args[1:])
	mark("IDE port: %q, MCU: %q", idePort(os.Args[1:]), mcu)
	// Ein Leonardo-Build sucht nur Boards mit eigenem USB, ein Uno-Build nur
	// solche ohne, damit ein zusaetzlich angeschlossenes Board unberuehrt
	// bleibt. Ohne Angabe (alte platform.txt) wie bisher das erste.
	kind := anyBoard
	switch {
	case mcu == "atmega32u4":
		kind = nativeUSB
	case mcu != "":
		kind = usbSerialIC
	}
	name, native, err := findBoard(idePort(os.Args[1:]), kind)
	if err != nil {
		fail("Arduino search", err)
	}
	mark("Arduino found on %s", name)

	// Öffnen löst einen Reset aus. Das Kennbyte beendet den Bootloader und
	// haelt den Stub vor setup() an; er quittiert mit 0x02.
	killOldBridges()
	ser, err := openPort(name)
	if err != nil {
		openFailed(name, err)
	}
	mark("port open")
	if native {
		drain(ser)
	}
	stopAnnounce := announce(ser)
	ready, lost := waitReady(ser, name)
	stopAnnounce()
	// Leonardo: startet für den Debugger neu und meldet sich neu an. Mehr
	// als ein Neustart hintereinander deutet auf einen Fehler; nach dem
	// dritten gibt die Bridge auf.
	for tries := 0; lost && tries < 3; tries++ {
		ser.Close()
		mark("port lost, board restarts")
		ser, name, err = reopen(name)
		if err != nil {
			fail("board did not come back after the restart", err)
		}
		mark("port open again on %s", name)
		drain(ser)
		stopAnnounce = announce(ser)
		ready, lost = waitReady(ser, name)
		stopAnnounce()
	}
	live := newLivePort(ser)
	defer live.Close()

	// Ende der Sitzung (GDB trennt oder die IDE beendet die Bridge): Der
	// Sketch soll ohne Debugger frisch weiterlaufen. Ein Uno wird dafuer
	// ueber DTR zurueckgesetzt; ein Leonardo startet selbst neu, sobald der
	// Port geschlossen ist.
	var finishOnce sync.Once
	finish := func() {
		finishOnce.Do(func() {
			mark("session ends")
			if !native {
				p := live.current()
				p.SetDTR(false)
				p.SetRTS(false)
				time.Sleep(50 * time.Millisecond)
				p.SetDTR(true)
				p.SetRTS(true)
				time.Sleep(50 * time.Millisecond)
			}
		})
	}
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, os.Interrupt, syscall.SIGTERM, syscall.SIGHUP)
	go func() {
		<-sigs
		finish()
		live.Close()
		os.Exit(0)
	}()
	mark("stub ready: %v", ready)
	if !ready {
		fmt.Printf("Warn : no answer from the board. Was the sketch uploaded with " +
			"Sketch -> Optimize for Debugging turned on?\n")
	}

	ln, err := net.Listen("tcp", "127.0.0.1:"+strconv.Itoa(port))
	if err != nil {
		fail("TCP port", err)
	}

	// Meldungen im OpenOCD-Stil, auf die Cortex-Debug wartet.
	fmt.Printf("Info : ArduDebug: Arduino on %s\n", name)
	fmt.Printf("Info : avr.cpu: hardware has 4 breakpoints, 0 watchpoints\n")
	fmt.Printf("Info : Listening on port %d for gdb connections\n", port)
	mark("listening on port %d", port)

	conn, err := ln.Accept()
	if err != nil {
		fail("GDB connection", err)
	}
	defer conn.Close()
	mark("GDB connected")

	// In beide Richtungen durchreichen; endet, sobald eine Seite schließt.
	done := make(chan struct{}, 2)
	serLocked := &lockedWriter{w: live} // GDB und Eingaben schreiben beide hierhin
	serOut := traced{serLocked, "GDB->"}
	guard := &pauseGuard{ser: serOut}
	input := newInputForwarder(traced{serLocked, "IN   "})
	go input.run()
	go readInput(os.Stdin, input)
	resume := func() { input.setRunning(true) }
	readyCh := make(chan struct{}, 1)
	onReady := func() {
		select {
		case readyCh <- struct{}{}:
		default:
		}
	}
	stop := func() { guard.stopped(); input.setRunning(false) }
	// "monitor reset" (Neu starten): Die Bridge antwortet GDB selbst mit OK
	// und setzt das Board ueber DTR/RTS zurueck, wie beim Hochladen. Ein
	// Watchdog-Reset im Stub wuerde den alten Nano-Bootloader in eine
	// Reset-Schleife schicken. Danach wieder das Kennbyte, damit der Stub vor
	// setup() anhaelt; GDB wartet so lange.
	reset := func() {
		input.setRunning(false)
		traced{conn, "<-BRG"}.Write([]byte("+$OK#9a"))
		if native {
			// Leonardo: Der Stub startet ueber den Watchdog neu (0x04),
			// das Board meldet sich am USB neu an.
			newName, err := live.restart(name, func(p serial.Port) {
				traced{p, "RST  "}.Write([]byte{restartMark})
			})
			if err == nil {
				name = newName
			}
		} else {
			p := live.current()
			p.SetDTR(false)
			p.SetRTS(false)
			time.Sleep(250 * time.Millisecond)
			p.SetDTR(true)
			p.SetRTS(true)
		}
		select { // alte Quittung verwerfen
		case <-readyCh:
		default:
		}
		stopAnnounce := announce(traced{serLocked, "RST  "})
		select {
		case <-readyCh:
		case <-time.After(readyTimeout):
		}
		stopAnnounce()
	}
	go func() { forwardFromGDB(serOut, conn, guard, resume, reset); done <- struct{}{} }()
	go func() {
		forwardFiltered(traced{conn, "<-AVR"}, live, traced{os.Stdout, "OUT  "}, stop, onReady)
		done <- struct{}{}
	}()
	<-done
	finish()
}
