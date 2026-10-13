![TinyRTOS](http://www.nikolairadke.de/aiduino/ardudebug_banner_4.png)
# ArduDebug

Breakpoint debugging for the Arduino Uno, Nano, Leonardo and many more AVR boards in the Arduino IDE 2.x, with the IDE's own debug button and **no extra hardware**, just the normal USB cable. Based on Arduino AVR Boards V1.8.8.  

🆕 What's new?  
* **13.10.2026** Release **V1.8.8-0**: the first stable release.  
    -- More news? Check the [newsblog](https://github.com/NikolaiRadke/ArduDebug/blob/main/NEWS.md).

## Supported boards

All boards of *Arduino AVR Boards* can be selected under *Tools → Board → ArduDebug AVR Boards* (except Gemma: its ATtiny85 has no serial connection to the computer). Only some of them have been tested so far:

| Board | Chip | Status |
|---|---|---|
| Arduino Uno | ATmega328P | tested |
| Arduino Nano | ATmega328P | tested |
| Arduino Leonardo | ATmega32U4 | tested |
| Micro, Esplora, Yún, LilyPad USB and other ATmega32U4 boards | ATmega32U4 | untested |
| Mini, Pro, Ethernet, Fio, BT, LilyPad and other ATmega328P/168 boards | ATmega328P/168 | untested |
| Arduino NG or older | ATmega8/168 | untested |
| Arduino Mega 2560, Mega ADK | ATmega2560 | untested |
| Arduino Mega with ATmega1280 (*Tools → Processor*) | ATmega1280 | untested |

## Testers wanted

ArduDebug is tested on Linux and Windows with the Uno and the Nano, and on Linux with the Leonardo. Help is welcome for:

- **macOS** (Apple Silicon and Intel): the package includes macOS builds, but they have never been tried.
- **Leonardo on Windows**.
- **Mega 2560** and other untested boards from the list above.
- **8 MHz boards** (Pro 3.3 V, LilyPad, Fio): they use 57600 baud, which has never been tried.

If you can test one of these, please open an [issue](https://github.com/NikolaiRadke/ArduDebug/issues) and tell me what happened, whether it works or not.

## How it works

- **avr8-stub** (by Jan Dolinay) runs on the board and speaks the GDB protocol over the serial line, on the Leonardo and other ATmega32U4 boards directly over USB.
- The stub is built into the core and only included when *Sketch → Optimize for Debugging* is enabled. Your sketch needs no debug code.
- A small **bridge** program, started by the IDE, finds the board automatically and connects GDB to it.
  
![Screenshot](http://www.nikolairadke.de/aiduino/ardudebug_screenshot.png)

## Serial while debugging

In debug builds the serial port belongs to the debugger. Your sketch can still use `Serial` as usual: the output appears in the **gdb-server** tab at the bottom of the IDE instead of the Serial Monitor, and what you type there (followed by Enter) arrives in `Serial.read()`.

- Each line you type ends with a newline (`\n`).
- Input typed while the program is stopped is delivered as soon as it continues.
- The receive buffer holds 63 characters, like the normal `Serial`.
- The Serial Plotter is not available during debugging.
- The baud rate in `Serial.begin()` is ignored.
- A debug build always talks to the debugger, even without a debug session: the Serial Monitor shows only garbage. To use the Serial Monitor, upload with *Optimize for Debugging* turned off.

## Peripherals view

While the program is stopped, the **Cortex Peripherals** section of the debug sidebar shows the hardware registers of the chip (ATmega328P, ATmega32U4 or ATmega2560): ports, timers, ADC, SPI, TWI, EEPROM, external interrupts and more, with every bit named. You can also change them: set `PORTB` to `0x20` (Uno) or `PORTC` to `0x80` (Leonardo) and the LED on pin 13 lights up, while the program is still stopped.

- `USART0` is read-only on Uno and Mega: it is the connection to the debugger.
- Data registers like `UDR0` and `SPDR` are not shown, because reading them changes the hardware state. The USB registers of the ATmega32U4 are not shown either.

## Installation

### Requirements

- Arduino IDE 2.3 or newer (Linux or Windows)
- A supported board and a USB cable – nothing else

### Install ArduDebug

1. Open *File → Preferences*.
2. Add this URL to *Additional boards manager URLs* (one URL per line if there are already others):
```
   https://raw.githubusercontent.com/NikolaiRadke/ArduDebug/main/package_ardudebug_index.json
```
3. Open *Tools → Board → Boards Manager*, search for **ArduDebug** and click *Install*.
   ArduDebug brings everything it needs, including its own GDB.

### First test

1. Select *Tools → Board → ArduDebug AVR Boards → Arduino Uno* (or your board) and the port.
2. Open the *Blink* example and enable *Sketch → Optimize for Debugging*.
3. Upload the sketch.
4. Click left of a line number in `loop()` to set a breakpoint (red dot).
5. Click the debug button. The program stops at the start of `loop()`; press *Continue* (F5) to run to your breakpoint.

> [!TIP]
> - Global variables are easiest to inspect in the *Watch* view: click *+* and enter the variable name.
> - To see your global variables in the *Variables* view (section *Global*) and change them there, select *Tools → LTO → Disabled*. The sketch gets slightly larger; switch back to *Enabled* for the final upload.
> - Close the Serial Monitor before debugging, and stop the debug session before uploading.

### Updating

Version numbers follow the core ArduDebug is based on: **1.8.8-X** is built on *Arduino AVR Boards 1.8.8*, X counts the ArduDebug releases.

Updates appear in the Boards Manager. If a new version doesn't show up, the IDE is still using a cached package list: delete `package_ardudebug_index.json` in your Arduino15 folder and restart the IDE.

- Linux: `~/.arduino15`
- Windows: `%LOCALAPPDATA%\Arduino15`

### Troubleshooting

- **"Port … busy" or "Access denied":** another program is using the port. Close the Serial Monitor. On Windows, also check the Task Manager (*Details*) for a leftover `ardudebug-bridge.exe` and end it.
- **Upload fails during debugging:** stop the debug session first, then upload.
- **Breakpoints are hollow grey circles:** either the debug session isn't running properly (see above), or breakpoints are deactivated – click *Activate Breakpoints* in the Breakpoints view.
- **Linux: no access to the port:** add your user to the `dialout` group (`sudo usermod -aG dialout $USER`) and log in again.
- **Reporting a problem:** please attach `ardudebug-bridge.log` from your temp folder (Linux/macOS: `/tmp`, Windows: `%TEMP%`). It records the communication of the last debug session.

## Limitations

- The stub needs about 3 KB of flash and up to about 270 bytes of RAM in debug builds. Normal builds are unaffected. On boards with an ATmega168 (16 KB flash, 1 KB RAM) this leaves less room for the sketch.
- In debug builds, some pins belong to the debugger:
  - Uno, Nano and other ATmega328P/168 boards: pins 0 and 1 (serial) and pin 2 (INT0). `attachInterrupt()` works on pin 3, but not on pin 2.
  - Leonardo and other ATmega32U4 boards: pin 7 (INT6). `Serial1` on pins 0 and 1 is free.
  - Mega: pins 0 and 1 (serial). The debugger uses INT6, which has no pin on the Mega.
- Up to 8 breakpoints at a time.
- While breakpoints are set, the program runs much slower: the debugger checks the position after every instruction. Timing-critical code may behave differently.
- Breakpoints inside interrupt routines, e.g. a function attached with `attachInterrupt()`, are never hit: while an interrupt routine runs, the debugger cannot stop the program.
- No watchpoints (stopping when a variable changes).
- While the program is stopped or single-stepping, interrupts do not run: millis() does not count on, and Servo pulses or tone() stop. Hardware PWM (analogWrite) keeps running.
- Local variables are often shown as *optimized out*, because debug builds are still optimized for size.
- Stepping past the end of `loop()` leads into the core's `main.cpp`.
- Sketches with their own `main()` instead of `setup()`/`loop()` can be debugged as long as they call `init()`. After clicking the debug button, the program stops inside `init()`; press *Continue* (F5) to run to your breakpoint. *Pause* stops wherever the program is.
- When the debug session starts or you click *Restart*, the board is reset and your sketch starts from the beginning. When the session ends, the board is reset again and the sketch runs normally. Without a debug session, a debug build runs normally.

## Repository layout

| Folder | Content |
|---|---|
| `core/avr` | The board core, based on Arduino AVR Boards 1.8.8, with avr8-stub |
| `bridge` | Source of the bridge program (Go) |
| `scripts` | Build script for the Boards Manager package |

## License

- The release packages contain binaries of **avr-gdb** (GNU GDB 17.2, GPL-3.0-or-later):
  - Linux and macOS: built from the [GNU GDB](https://www.gnu.org/software/gdb/) sources with statically linked [GMP](https://gmplib.org/) and [MPFR](https://www.mpfr.org/) (LGPL), see `.github/workflows/gdb.yml`.
  - Windows: from the [AVR-GCC builds by Zak Kemble](https://github.com/ZakKemble/avr-gcc-build) (16.1.0).
- ATmega328P.svd, ATmega32U4.svd and ATmega2560.svd are generated from Microchip’s device files (ATDF, Apache-2.0).

## Credits

- [avr8-stub / avr_debug](https://github.com/jdolinay/avr_debug) by Jan Dolinay
- [Arduino AVR Boards](https://github.com/arduino/ArduinoCore-avr) by Arduino
- [go.bug.st/serial](https://github.com/bugst/go-serial) by Cristian Maglie (BSD-3-Clause)

## 💙 Support ArduDebug

ArduDebug is free and open source. If it saved your day, consider buying me a coffee! ☕

[![GitHub Sponsors](https://img.shields.io/github/sponsors/NikolaiRadke?style=for-the-badge&logo=github&color=ea4aaa)](https://github.com/sponsors/NikolaiRadke)
[![Ko-fi](https://img.shields.io/badge/Ko--fi-Buy%20me%20a%20coffee-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/nikolairadke)

Every contribution helps keep this project alive! 🚀
