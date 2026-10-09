![TinyRTOS](http://www.nikolairadke.de/aiduino/ardudebug_banner_4.png)
# ArduDebug

Breakpoint debugging for the Arduino Uno/Nano (and maybe more) in the Arduino IDE 2.x, with the IDE's own debug button and **no extra hardware**, just the normal USB cable.

🆕 What's new?  
* **09.10.2026** Release **V0.1.1** with Linux und Windows support.  
    -- More news? Check the [newsblog](https://github.com/NikolaiRadke/ArduDebug/blob/main/NEWS.md).

## Supported boards

| Board | Chip | Status |
|---|---|---|
| Arduino Uno | ATmega328P | tested |
| Arduino Nano (incl. old bootloader) | ATmega328P | tested |
| Arduino Mega 2560 | ATmega2560 | compiles, not yet tested |
| Leonardo, Micro and other ATmega32U4 boards | ATmega32U4 | not supported by avr8-stub |

## How it works

- **avr8-stub** (by Jan Dolinay) runs on Uno/Nano and speaks the GDB protocol over the serial line.
- The stub is built into the core and only included when *Sketch → Optimize for Debugging* is enabled. Your sketch needs no debug code.
- A small **bridge** program, started by the IDE, finds the Uno automatically and connects GDB to it.
  
![Screenshot](http://www.nikolairadke.de/aiduino/ardudebug_screenshot.png)

## Installation

### Requirements

- Arduino IDE 2.3 or newer (Linux or Windows)
- An Arduino Uno or Nano and a USB cable – nothing else

### Install ArduDebug

1. Open *File → Preferences*.
2. Add this URL to *Additional boards manager URLs* (one URL per line if there are already others):
```
   https://raw.githubusercontent.com/NikolaiRadke/ArduDebug/main/package_ardudebug_index.json
```
3. Open *Tools → Board → Boards Manager*, search for **ArduDebug** and click *Install*.
   ArduDebug brings everything it needs, including its own GDB.

### First test

1. Select *Tools → Board → ArduDebug AVR Boards → Arduino Uno* (or *Nano*) and the port.
2. Open the *Blink* example and enable *Sketch → Optimize for Debugging*.
3. Upload the sketch.
4. Click left of a line number in `loop()` to set a breakpoint (red dot).
5. Click the debug button. The program stops at the start of `loop()`; press *Continue* (F5) to run to your breakpoint.

> [!TIP]
> - Global variables are easiest to inspect in the *Watch* view: click *+* and enter the variable name.
> - Close the Serial Monitor before debugging, and stop the debug session before uploading.

### Updating

Updates appear in the Boards Manager. If a new version doesn't show up, the IDE is still using a cached package list: delete `package_ardudebug_index.json` in your Arduino15 folder and restart the IDE.

- Linux: `~/.arduino15`
- Windows: `%LOCALAPPDATA%\Arduino15`

### Troubleshooting

- **"Port … busy" or "Access denied":** another program is using the port. Close the Serial Monitor. On Windows, also check the Task Manager (*Details*) for a leftover `ardudebug-bridge.exe` and end it.
- **Upload fails during debugging:** stop the debug session first, then upload.
- **Breakpoints are hollow grey circles:** either the debug session isn't running properly (see above), or breakpoints are deactivated – click *Activate Breakpoints* in the Breakpoints view.
- **Linux: no access to the port:** add your user to the `dialout` group (`sudo usermod -aG dialout $USER`) and log in again.

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

## 💙 Support ArduDebug

ArduDebug is free and open source. If it saved your day, consider buying me a coffee! ☕

[![GitHub Sponsors](https://img.shields.io/github/sponsors/NikolaiRadke?style=for-the-badge&logo=github&color=ea4aaa)](https://github.com/sponsors/NikolaiRadke)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20me%20a%20coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/nikolairadke)

Every contribution helps keep this project alive! 🚀
