# Simulator test

Checks avr8-stub without hardware: the test sketch is built like a debug
build in the IDE and runs in [simavr](https://github.com/buserror/simavr),
while `harness` talks to the stub over UART0 like GDB does. It checks pause,
breakpoints, `millis()` running while breakpoints are set, single steps and
continuing. Boards: Uno (ATmega328P), ATmega328P at 8 MHz (57600 baud),
ATmega168 at 16 and 8 MHz, ATmega8, Mega (ATmega2560, also with code above 128 KB) and
Mega with ATmega1280, and a sketch busy with `Serial` output. The Leonardo
(USB) cannot be simulated.

    sudo apt install build-essential git libelf-dev
    bash tests/sim/run.sh

The first run downloads and builds simavr into `tests/sim/build/` (the
simavr packages of the Linux distributions are too old: they handle
interrupts wrongly). avr-gcc is taken from the Arduino IDE.

Passing here does not replace a test on the real board.
