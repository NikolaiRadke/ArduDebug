// ardudebug-bridge: replacement for OpenOCD in the ArduDebug board package.
// The Arduino IDE (Cortex-Debug) starts the program like OpenOCD.
// It finds the Arduino by its USB ID and connects GDB (TCP) to the stub (serial).
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

// USB vendor IDs: Arduino, Arduino.org, CH340 clones, FTDI,
// CP210x clones, SparkFun (LilyPad USB), Adafruit (Circuit Playground)
var knownVIDs = []string{"2341", "2A03", "1A86", "0403", "10C4", "1B4F", "239A"}

// USB IDs (VID:PID) of the boards with native USB (ATmega32U4), as
// the sketch reports them. For the debugger they restart via the stub,
// not via DTR. From boards.txt of Arduino AVR Boards 1.8.8.
var nativeUSBIDs = []string{
	"2341:8036", "2A03:8036", // Leonardo
	"2A03:8040",              // Leonardo ETH
	"2341:8037", "2A03:8037", // Micro
	"2341:803C", "2A03:803C", // Esplora
	"2341:8041", "2A03:8041", // Yun
	"2A03:8050",              // Yun Mini
	"2A03:8056",              // Industrial 101
	"2A03:8001",              // Linino One
	"2341:8038", "2A03:8038", // Robot Control
	"2341:8039", "2A03:8039", // Robot Motor
	"1B4F:9208", // LilyPad Arduino USB
	"239A:8011", // Circuit Playground
}

// gdbPort reads "gdb_port N" from the arguments Cortex-Debug passes.
func gdbPort(args []string) int {
	m := regexp.MustCompile(`gdb_port\s+(\d+)`).FindStringSubmatch(strings.Join(args, " "))
	if m != nil {
		if p, err := strconv.Atoi(m[1]); err == nil {
			return p
		}
	}
	return 50000
}

// idePort reads "ardudebug_port PORT" from the arguments: the port
// the user selected in the IDE (empty if not passed).
func idePort(args []string) string {
	m := regexp.MustCompile(`ardudebug_port\s+(\S+)`).FindStringSubmatch(strings.Join(args, " "))
	if m != nil && !strings.Contains(m[1], "{") && !strings.HasPrefix(m[1], "ardudebug_") {
		return m[1]
	}
	return ""
}

// baudRate of the connection to the stub: 115200, 57600 for boards below 16 MHz
// (as in the stub, see GDB_USART_BAUDRATE).
var baudRate = 115200

// ideFCPU reads "ardudebug_fcpu F" (build.f_cpu, e.g. 16000000L); 0 if
// not passed.
func ideFCPU(args []string) int {
	m := regexp.MustCompile(`ardudebug_fcpu\s+(\d+)`).FindStringSubmatch(strings.Join(args, " "))
	if m == nil {
		return 0
	}
	f, _ := strconv.Atoi(m[1])
	return f
}

// ideMCU reads "ardudebug_mcu MCU" (build.mcu of the selected board).
func ideMCU(args []string) string {
	m := regexp.MustCompile(`ardudebug_mcu\s+(\S+)`).FindStringSubmatch(strings.Join(args, " "))
	if m != nil && !strings.Contains(m[1], "{") {
		return m[1]
	}
	return ""
}

// drain discards what is still coming from the board: a Leonardo can have
// bytes from an earlier session in its USB buffer (e.g. an old
// acknowledgement) that only arrive after opening.
func drain(ser serial.Port) {
	ser.SetReadTimeout(20 * time.Millisecond)
	defer ser.SetReadTimeout(serial.NoTimeout)
	buf := make([]byte, 64)
	for deadline := time.Now().Add(100 * time.Millisecond); time.Now().Before(deadline); {
		n, err := ser.Read(buf)
		logData("DRAIN", buf[:n])
		if err != nil || n == 0 {
			return
		}
	}
}

// Which boards findBoard considers.
const (
	anyBoard    = iota
	nativeUSB   // only with native USB (Leonardo, Micro)
	usbSerialIC // only with a USB serial chip (Uno, Nano)
)

// findBoard looks for the board: preferably the port prefer (from the IDE),
// otherwise the first serial USB port with a known vendor ID
// and the matching kind. native reports whether the board has native USB.
func findBoard(prefer string, kind int) (name string, native bool, err error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", false, err
	}
	isNative := func(p *enumerator.PortDetails) bool {
		for _, id := range nativeUSBIDs {
			if strings.EqualFold(p.VID+":"+p.PID, id) {
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

// openPort opens the serial port. If it is busy, an ardudebug-monitor may
// have it: it hands it over. Otherwise it retries for portRetry (Windows
// only, after ending an old bridge).
func openPort(name string) (serial.Port, error) {
	deadline := time.Now().Add(portRetry)
	claimed := false
	for {
		ser, err := serial.Open(name, &serial.Mode{BaudRate: baudRate})
		var pe *serial.PortError
		busy := err != nil && errors.As(err, &pe) && pe.Code() == serial.PortBusy
		if busy && !claimed {
			claimed = true
			if c := claimMonitor(name); c != nil {
				mark("Serial Monitor handed over %s", name)
				monitors.add(c)
				continue
			}
		}
		if !busy || time.Now().After(deadline) {
			return ser, err
		}
		time.Sleep(100 * time.Millisecond)
	}
}

// announce sends the marker byte 0x02 ("debugger is coming") every 10 ms
// after the reset. The stub stops at the first one right after debug_init()
// and waits for GDB, so the sketch only starts with the debugger. Unknown
// characters also make the bootloader start the sketch right away.
// The returned function stops sending and waits for it.
func announce(ser io.Writer) func() {
	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		t := time.NewTicker(10 * time.Millisecond)
		defer t.Stop()
		sent := 0
		for {
			select {
			case <-stop:
				mark("announcement: %d marker bytes sent", sent)
				return
			case <-t.C:
				if _, err := ser.Write([]byte{0x02}); err != nil {
					mark("announcement: write error after %d bytes: %v", sent, err)
					<-stop
					return
				}
				sent++
			}
		}
	}()
	// Don't wait forever: if the board doesn't read along (Leonardo without a
	// debug build), writing blocks at some point.
	return func() {
		close(stop)
		select {
		case <-done:
		case <-time.After(200 * time.Millisecond):
		}
	}
}

// readyMark is the stub's acknowledgement of the marker byte: it is now
// before setup() and waits for GDB.
const readyMark = 0x02

// restartMark asks the stub of a Leonardo to restart.
const restartMark = 0x04

// readyTimeout: the longest the bridge waits for the acknowledgement.
var readyTimeout = 3 * time.Second

// waitReady reads the port until the acknowledgement comes (true) or readyTimeout
// has passed (false). Other characters, e.g. from the bootloader, are dropped.
// lost reports that the port has disappeared: a Leonardo restarts
// when the debugger announces itself, and comes back on USB.
func waitReady(ser serial.Port, name string) (ready, lost bool) {
	ser.SetReadTimeout(50 * time.Millisecond)
	defer ser.SetReadTimeout(serial.NoTimeout)
	deadline := time.Now().Add(readyTimeout)
	nextCheck := time.Now()
	buf := make([]byte, 64)
	for time.Now().Before(deadline) {
		n, err := ser.Read(buf)
		if err != nil {
			mark("read error while waiting: %v", err)
			return false, true
		}
		logData("WAIT ", buf[:n])
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

// portPresent: is the port still in the system?
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

// reconnectTimeout: how long a Leonardo may take for the restart.
var reconnectTimeout = 10 * time.Second

// reopen waits until the board is back on USB after a restart
// and opens the port again. Other connected Arduinos stay
// untouched: preferably the old name, otherwise a board with native USB.
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

// openFailed explains why the serial port didn't open, and exits.
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
	if f := ideFCPU(os.Args[1:]); f > 0 && f < 16000000 {
		baudRate = 57600
	}
	mark("IDE port: %q, MCU: %q, baud %d", idePort(os.Args[1:]), mcu, baudRate)
	// A Leonardo build looks only for boards with native USB, an Uno build only
	// for those without, so that an additionally connected board stays
	// untouched. Without this (old platform.txt) the first one, as before.
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

	// A running ardudebug-monitor hands the port over (see openPort) and
	// shows the sketch's Serial output during the session. One that the IDE
	// opens later attaches itself. Closed last, after the serial port, so
	// the monitors can open the port again right away.
	ideName := name
	monitors.listen(ideName)
	defer monitors.closeAll()

	// Opening causes a reset. The marker byte ends the bootloader and
	// stops the stub before setup(); it acknowledges with 0x02.
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
	// Leonardo: restarts for the debugger and comes back on USB. More
	// than one restart in a row points to an error; after the
	// third the bridge gives up.
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

	// End of the session (GDB disconnects or the IDE ends the bridge): the
	// sketch should run on fresh without the debugger. An Uno is reset
	// via DTR for that; a Leonardo restarts by itself as soon as the
	// port is closed.
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
		monitors.closeAll()
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

	// OpenOCD-style messages that Cortex-Debug waits for.
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

	// Pass through in both directions; ends as soon as one side closes.
	done := make(chan struct{}, 2)
	serLocked := &lockedWriter{w: live} // GDB and input both write here
	serOut := traced{serLocked, "GDB->"}
	guard := &pauseGuard{ser: serOut}
	input := newInputForwarder(traced{serLocked, "IN   "})
	go input.run()
	go readInput(os.Stdin, input)
	monitors.setInput(input.add) // typed in the Serial Monitor
	resume := func() { input.setRunning(true) }
	readyCh := make(chan struct{}, 1)
	onReady := func() {
		select {
		case readyCh <- struct{}{}:
		default:
		}
	}
	stop := func() { guard.stopped(); input.setRunning(false) }
	// "monitor reset" (Restart): the bridge answers GDB with OK itself
	// and resets the board via DTR/RTS, as when uploading. A
	// watchdog reset in the stub would send the old Nano bootloader into a
	// reset loop. Then the marker byte again, so that the stub stops before
	// setup(); GDB waits until then.
	reset := func() {
		input.setRunning(false)
		traced{conn, "<-BRG"}.Write([]byte("+$OK#9a"))
		if native {
			// Leonardo: the stub restarts via the watchdog (0x04),
			// the board comes back on USB.
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
		select { // discard an old acknowledgement
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
		forwardFiltered(traced{conn, "<-AVR"}, live, traced{teeWriter{os.Stdout}, "OUT  "}, stop, onReady)
		done <- struct{}{}
	}()
	<-done
	finish()
	live.Close()
	monitors.closeAll()
}
