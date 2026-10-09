# ArduDebug

Breakpoint debugging for the Arduino Uno in the Arduino IDE 2.x, with the IDE's own debug button and **no extra hardware** – just the normal USB cable.

> **Status:** early proof of concept, tested on Linux. Windows support is in progress.

## Supported boards

| Board | Chip | Status |
|---|---|---|
| Arduino Uno | ATmega328P | tested |
| Arduino Nano (incl. old bootloader) | ATmega328P | tested |
| Arduino Mega 2560 | ATmega2560 | compiles, not yet tested |
| Leonardo, Micro and other ATmega32U4 boards | ATmega32U4 | not supported by avr8-stub |

## How it works

- **avr8-stub** (by Jan Dolinay) runs on the Uno and speaks the GDB protocol over the serial line.
- The stub is built into the core and only included when *Sketch → Optimize for Debugging* is enabled. Your sketch needs no debug code.
- A small **bridge** program, started by the IDE, finds the Uno automatically and connects GDB to it.

## Installation

*Coming soon:* ArduDebug will be installable via the Boards Manager with an additional URL.

## Usage

1. Select *Tools → Board → ArduDebug AVR Boards → Arduino Uno*.
2. Enable *Sketch → Optimize for Debugging* and upload your sketch.
3. Set breakpoints and press the debug button.

Tips:
- If breakpoints appear as hollow grey circles, they are deactivated. Click *Activate Breakpoints* in the Breakpoints view.
- Global variables are easiest to inspect in the *Watch* view.

## Limitations

- The stub needs about 4 KB of flash in debug builds. Normal builds are unaffected.
- `Serial` cannot be used in debug builds yet, because the stub uses the serial port.
- Stepping past the end of `loop()` leads into the core's `main.cpp`.

## Repository layout

| Folder | Content |
|---|---|
| `core/avr` | The board core, based on Arduino AVR Boards 1.8.8, with avr8-stub |
| `bridge` | Source of the bridge program (Go) |
| `scripts` | Build script for the Boards Manager package |

## License

- `core/avr`: LGPL-3.0-or-later (see `COPYING.LESSER` and `COPYING`). Based on the Arduino AVR core (LGPL-2.1-or-later) and avr8-stub (LGPL-3.0-or-later).
- `bridge` and `scripts`: MIT (see `LICENSE` in each folder).

## Credits

- [avr8-stub / avr_debug](https://github.com/jdolinay/avr_debug) by Jan Dolinay
- [Arduino AVR Boards](https://github.com/arduino/ArduinoCore-avr) by Arduino
- [go.bug.st/serial](https://github.com/bugst/go-serial) by Cristian Maglie (BSD-3-Clause)
