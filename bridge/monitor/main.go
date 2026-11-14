// ardudebug-monitor: pluggable monitor for the ArduDebug board package.
// Without a debug session it works like Arduino's serial monitor. When the
// bridge starts a debug session, it hands the serial port over to the bridge
// and shows the sketch's Serial output that the bridge passes on; typed input
// goes to the bridge. At the end of the session it opens the port again.
// The IDE talks to it through the pluggable monitor protocol: commands on
// stdin, JSON answers on stdout, the data on a TCP connection.
package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"sync"
	"time"

	"ardudebug-bridge/handoff"

	"go.bug.st/serial"
)

// param describes one setting of the port, like in Arduino's serial monitor.
type param struct {
	Label    string   `json:"label,omitempty"`
	Type     string   `json:"type,omitempty"`
	Values   []string `json:"value,omitempty"`
	Selected string   `json:"selected,omitempty"`
}

type portDescription struct {
	Protocol string            `json:"protocol"`
	Params   map[string]*param `json:"configuration_parameters"`
}

type message struct {
	EventType       string           `json:"eventType"`
	Message         string           `json:"message,omitempty"`
	Error           bool             `json:"error,omitempty"`
	ProtocolVersion int              `json:"protocolVersion,omitempty"`
	PortDescription *portDescription `json:"port_description,omitempty"`
}

var settings = map[string]*param{
	"baudrate": {Label: "Baudrate", Type: "enum", Selected: "9600", Values: []string{
		"300", "600", "750", "1200", "2400", "4800", "9600", "19200", "31250", "38400", "57600",
		"74880", "115200", "230400", "250000", "460800", "500000", "921600", "1000000", "2000000"}},
	"parity":    {Label: "Parity", Type: "enum", Selected: "none", Values: []string{"none", "even", "odd", "mark", "space"}},
	"bits":      {Label: "Data bits", Type: "enum", Selected: "8", Values: []string{"5", "6", "7", "8", "9"}},
	"stop_bits": {Label: "Stop bits", Type: "enum", Selected: "1", Values: []string{"1", "1.5", "2"}},
	"rts":       {Label: "RTS", Type: "enum", Selected: "on", Values: []string{"on", "off"}},
	"dtr":       {Label: "DTR", Type: "enum", Selected: "on", Values: []string{"on", "off"}},
}

func mode() *serial.Mode {
	baud, _ := strconv.Atoi(settings["baudrate"].Selected)
	bits, _ := strconv.Atoi(settings["bits"].Selected)
	parity := map[string]serial.Parity{"none": serial.NoParity, "even": serial.EvenParity,
		"odd": serial.OddParity, "mark": serial.MarkParity, "space": serial.SpaceParity}[settings["parity"].Selected]
	stop := map[string]serial.StopBits{"1": serial.OneStopBit, "1.5": serial.OnePointFiveStopBits,
		"2": serial.TwoStopBits}[settings["stop_bits"].Selected]
	return &serial.Mode{BaudRate: baud, DataBits: bits, Parity: parity, StopBits: stop,
		InitialStatusBits: &serial.ModemOutputBits{
			DTR: settings["dtr"].Selected == "on", RTS: settings["rts"].Selected == "on"}}
}

var (
	outMu sync.Mutex
	mu    sync.Mutex // guards everything below

	portName string
	ser      serial.Port  // nil while the bridge has the port
	client   net.Conn     // data connection to the IDE
	ctl      net.Listener // waits for the bridge
	bridge   net.Conn     // the bridge, during a debug session
	isOpen   bool
)

func send(m message) {
	data, _ := json.MarshalIndent(m, "", "  ")
	outMu.Lock()
	fmt.Println(string(data))
	outMu.Unlock()
}

func ok(event string) { send(message{EventType: event, Message: "OK"}) }

func fail(event, msg string) { send(message{EventType: event, Error: true, Message: msg}) }

// reconnectTimeout: how long the board may take to come back after the
// session (a Leonardo restarts and comes back on USB).
var reconnectTimeout = 15 * time.Second

func main() {
	r := bufio.NewReader(os.Stdin)
	hello := false
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			shutdown()
			return
		}
		line = strings.TrimSpace(line)
		cmd, args, _ := strings.Cut(line, " ")
		cmd = strings.ToUpper(cmd)
		if !hello && cmd != "HELLO" && cmd != "QUIT" {
			fail("command_error", fmt.Sprintf("First command must be HELLO, but got '%s'", cmd))
			continue
		}
		switch cmd {
		case "HELLO":
			if !regexp.MustCompile(`^\d+ "[^"]+"$`).MatchString(args) {
				fail("hello", "Invalid HELLO command")
				continue
			}
			hello = true
			send(message{EventType: "hello", Message: "OK", ProtocolVersion: 1})
		case "DESCRIBE":
			mu.Lock()
			send(message{EventType: "describe", Message: "OK",
				PortDescription: &portDescription{Protocol: "serial", Params: settings}})
			mu.Unlock()
		case "CONFIGURE":
			name, value, _ := strings.Cut(args, " ")
			if err := configure(name, value); err != nil {
				fail("configure", err.Error())
			} else {
				ok("configure")
			}
		case "OPEN":
			address, port, found := strings.Cut(args, " ")
			if !found {
				fail("open", "Invalid OPEN command")
			} else if err := open(address, port); err != nil {
				fail("open", err.Error())
			} else {
				ok("open")
			}
		case "CLOSE":
			if closeAll() {
				ok("close")
			} else {
				fail("close", "port already closed")
			}
		case "QUIT":
			shutdown()
			ok("quit")
			return
		default:
			fail("command_error", fmt.Sprintf("Command %s not supported", cmd))
		}
	}
}

// configure changes a setting. An open port takes it at once; while the
// bridge has the port, it applies when the port is opened again.
func configure(name, value string) error {
	mu.Lock()
	defer mu.Unlock()
	p, found := settings[name]
	if !found {
		return fmt.Errorf("could not find parameter named %s", name)
	}
	if !slices.Contains(p.Values, value) {
		return fmt.Errorf("invalid value for parameter %s: %s", name, value)
	}
	old := p.Selected
	p.Selected = value
	if ser == nil {
		return nil
	}
	var err error
	switch name {
	case "dtr":
		err = ser.SetDTR(value == "on")
	case "rts":
		err = ser.SetRTS(value == "on")
	default:
		err = ser.SetMode(mode())
	}
	if err != nil {
		p.Selected = old
	}
	return err
}

func open(address, port string) error {
	mu.Lock()
	defer mu.Unlock()
	if isOpen {
		return fmt.Errorf("port already opened: %s", port)
	}
	p, err := serial.Open(port, mode())
	if err != nil {
		return err
	}
	c, err := net.Dial("tcp", address)
	if err != nil {
		p.Close()
		return err
	}
	portName, ser, client, isOpen = port, p, c, true
	// Wait for the bridge. Without the file the bridge doesn't find us; that
	// is no reason to fail, the monitor then works like the normal one.
	if l, err := net.Listen("tcp", "127.0.0.1:0"); err == nil {
		ctl = l
		os.WriteFile(handoff.File(port), []byte(strconv.Itoa(l.Addr().(*net.TCPAddr).Port)), 0o644)
		go acceptBridge(l)
	}
	go fromBoard(p)
	go fromIDE(c)
	return nil
}

// fromBoard passes what the board sends on to the IDE. It ends quietly when
// the port was handed to the bridge or closed by the IDE.
func fromBoard(p serial.Port) {
	buf := make([]byte, 1024)
	for {
		n, err := p.Read(buf)
		if n > 0 {
			mu.Lock()
			c := client
			mu.Unlock()
			if c != nil {
				c.Write(buf[:n])
			}
		}
		if err != nil {
			mu.Lock()
			mine := ser == p
			mu.Unlock()
			if mine {
				lost("lost connection with the port")
			}
			return
		}
	}
}

// fromIDE passes typed input on: to the board, or to the bridge during a
// debug session.
func fromIDE(c net.Conn) {
	buf := make([]byte, 1024)
	for {
		n, err := c.Read(buf)
		if n > 0 {
			mu.Lock()
			p, b := ser, bridge
			mu.Unlock()
			switch {
			case b != nil:
				b.Write(buf[:n])
			case p != nil:
				p.Write(buf[:n])
			}
		}
		if err != nil {
			mu.Lock()
			mine := client == c
			mu.Unlock()
			if mine {
				lost("lost TCP/IP connection with the client!")
			}
			return
		}
	}
}

// acceptBridge serves one debug session after the other.
func acceptBridge(l net.Listener) {
	for {
		b, err := l.Accept()
		if err != nil {
			return
		}
		session(b)
	}
}

// session hands the port to the bridge, shows what the bridge sends and
// opens the port again when the bridge is done.
func session(b net.Conn) {
	defer b.Close()
	b.SetReadDeadline(time.Now().Add(2 * time.Second))
	r := bufio.NewReader(b)
	if line, err := r.ReadString('\n'); err != nil || line != handoff.Release {
		return
	}
	b.SetReadDeadline(time.Time{})
	mu.Lock()
	if ser == nil || client == nil {
		mu.Unlock()
		return
	}
	ser.Close()
	ser, bridge = nil, b
	c := client
	mu.Unlock()
	if _, err := io.WriteString(b, handoff.OK); err != nil {
		reopen(b)
		return
	}
	io.Copy(c, r) // the sketch's output, until the bridge closes
	reopen(b)
}

// reopen opens the serial port again after a debug session. The bridge may
// still hold it for a moment, and a Leonardo first restarts.
func reopen(b net.Conn) {
	mu.Lock()
	bridge = nil
	name := portName
	mu.Unlock()
	var err error
	for deadline := time.Now().Add(reconnectTimeout); time.Now().Before(deadline); time.Sleep(200 * time.Millisecond) {
		mu.Lock()
		if !isOpen {
			mu.Unlock()
			return
		}
		var p serial.Port
		if p, err = serial.Open(name, mode()); err == nil {
			ser = p
			mu.Unlock()
			go fromBoard(p)
			return
		}
		mu.Unlock()
		var pe *serial.PortError
		if errors.As(err, &pe) && pe.Code() == serial.PermissionDenied {
			break
		}
	}
	lost(fmt.Sprintf("cannot open %s again after debugging: %v", name, err))
}

// lost reports a connection that broke without a CLOSE from the IDE.
func lost(msg string) {
	if closeAll() {
		fail("port_closed", msg)
	}
}

// closeAll closes the port and all connections; false if already closed.
func closeAll() bool {
	mu.Lock()
	defer mu.Unlock()
	if !isOpen {
		return false
	}
	isOpen = false
	if ctl != nil {
		ctl.Close()
		os.Remove(handoff.File(portName))
		ctl = nil
	}
	if bridge != nil {
		bridge.Close()
		bridge = nil
	}
	if ser != nil {
		ser.Close()
		ser = nil
	}
	if client != nil {
		client.Close()
		client = nil
	}
	return true
}

func shutdown() { closeAll() }
