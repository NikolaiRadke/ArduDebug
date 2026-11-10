![ArduDebug](https://raw.githubusercontent.com/NikolaiRadke/ArduDebug/main/vscode/images/banner.png)

# ArduDebug for VS Code

Breakpoint debugging for the Arduino Uno, Nano, Leonardo and many more AVR boards, right in VS Code. **No extra hardware**, just the normal USB cable. **No configuration**: open a sketch, press **F5**, done.

ArduDebug brings everything it needs. You don't have to install the Arduino IDE, the Arduino CLI or any other extension.

## Features

* **Debugging** with breakpoints, single stepping, call stack and watch expressions
* **Verify** and **Upload** like in the Arduino IDE, all in one ArduDebug menu
* **Serial Monitor** with selectable baud rate
* **Automatic board and port detection**
* **Code completion** for your sketch (requires the C/C++ extension by Microsoft)
* English and German user interface

## Getting started

1. Install ArduDebug from the Marketplace. Or download the file for your system from the [GitHub releases](https://github.com/NikolaiRadke/ArduDebug/releases) and install it via *Extensions → … → Install from VSIX*.
2. Connect your Arduino via USB.
3. Open a sketch (`.ino` file).
4. Set a breakpoint by clicking left of a line number.
5. Press **F5**.

On the first start ArduDebug sets itself up, which takes a minute. After that, it compiles your sketch, uploads it and starts the debugger. The program stops at your breakpoint, and you can inspect variables, step through your code and continue.

## The ArduDebug menu

When an `.ino` file is open, you find the **ArduDebug button** (circuit board icon) in the top right corner of the editor. It opens a menu with everything ArduDebug can do:

* **Verify**: compiles the sketch
* **Upload**: compiles and uploads the sketch without debugging, so it runs at full speed
* **Debug**: compiles, uploads and starts debugging, same as **F5**
* **Serial Monitor**: opens the Serial Monitor in a terminal
* **Select Board**, **Select Port** and **Select Baud Rate**

All commands are also available in the Command Palette under *ArduDebug*.

## Debugging tips

* **Pause** stops at the line of your sketch the program is working on. The marked line is always the **next** line to run. So if the program was waiting in `delay()`, you see the line after the `delay()`: the wait ends at once when you pause.
* **Step** over the last line of `loop()` takes you straight back to the start of `loop()`. **Step Out** of `loop()` does the same.
* ArduDebug never shows the Arduino core (`main.cpp`, `wiring.c` and so on) while you step. Library code is shown if you step into it.
* Debug builds are always made without LTO, so all your variables are visible.

## Board, port and baud rate

ArduDebug shows the board, the port and the baud rate in the status bar at the bottom.

* The Uno, Mega and Leonardo are detected automatically.
* The Nano and most clones use a USB chip like the CH340 or FTDI, which does not tell which board is behind it. Here ArduDebug asks once which board is connected and remembers your choice for this sketch.
* Click on the board, the port or the baud rate to change it. The baud rate must match `Serial.begin()` in your sketch.

## Serial Monitor

The Serial Monitor closes automatically before uploading and reopens afterwards, so the port is never blocked.

During debugging, the serial port belongs to the debugger, and the Serial Monitor is closed. Your sketch can still use `Serial` as usual: the output appears in the **Debug Console**. The Serial Monitor reopens when the debug session ends.

## Supported boards

All boards of *Arduino AVR Boards* except the Gemma. Tested so far in VS Code and the Arduino IDE on Linux and Windows:

* **Arduino Uno**
* **Arduino Nano** (also with the old bootloader)
* **Arduino Leonardo**

macOS and the Mega 2560 have not been tested yet. If you can test them, please open an [issue](https://github.com/NikolaiRadke/ArduDebug/issues) and tell me what happened, whether it works or not.

## How it works

A small debug stub runs on the board together with your sketch and talks to the debugger over the USB cable. It is only included when you start debugging. Your sketch needs no debug code, and a normal upload contains no trace of it.

ArduDebug uses the Arduino CLI to compile and upload, a small bridge program to connect the debugger to the board, and the CDT GDB Debug Adapter to connect the debugger to VS Code. All of this is included.

The same board package also works in the Arduino IDE 2.x. See the [main project page](https://github.com/NikolaiRadke/ArduDebug) for details.

## License

ArduDebug is licensed under the MIT license, the board package under the LGPL 3.0. Included third party software: Arduino CLI (GPL 3.0), CDT GDB Debug Adapter (EPL 2.0).
