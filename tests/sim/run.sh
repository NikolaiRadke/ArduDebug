#!/bin/bash
# Simulator test for avr8-stub: builds debug builds of a test sketch with the
# ArduDebug core and runs them in simavr, with a small program acting as GDB.
# Needs: gcc, make, git, libelf-dev, avr-gcc (Arduino IDE's or from PATH).
# Usage: tests/sim/run.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=$HERE/../../core/avr
B=$HERE/build
SIMAVR_COMMIT=557b07441f02a41a8bc7f95edcb16c24b2f9eeda
mkdir -p "$B"

# avr-gcc: the newest one of the Arduino IDE, otherwise from PATH
GCC=$(ls -d ~/.arduino15/packages/arduino/tools/avr-gcc/*/bin 2>/dev/null | sort -V | tail -1)
[ -n "$GCC" ] && PATH=$GCC:$PATH
command -v avr-gcc >/dev/null || { echo "avr-gcc not found"; exit 2; }

# simavr: the Linux packages (1.6) handle interrupts wrongly, build a current one
if [ ! -f "$B/simavr/simavr/obj-$(gcc -dumpmachine)/libsimavr.a" ]; then
	echo "Building simavr ..."
	rm -rf "$B/simavr"
	git clone -q https://github.com/buserror/simavr.git "$B/simavr"
	git -C "$B/simavr" checkout -q $SIMAVR_COMMIT
	make -C "$B/simavr/simavr" -j4 RELEASE=1 >"$B/simavr.log" 2>&1 || { echo "simavr build failed, see $B/simavr.log"; exit 2; }
fi
OBJ=$B/simavr/simavr/obj-$(gcc -dumpmachine)
mkdir -p "$B/inc"; ln -sfn "$B/simavr/simavr/sim" "$B/inc/simavr"
gcc -O2 -Wall -I"$B/inc" -I"$OBJ" -o "$B/harness" "$HERE/harness.c" "$OBJ/libsimavr.a" -lelf

# debug build like the IDE does it (Optimize for Debugging, LTO on)
build() {	# name sketch mcu variant board f_cpu
	local O=$B/$1 SRC=$CORE/cores/arduino
	rm -rf "$O"; mkdir -p "$O"
	local FL="-c -g -Os -DARDUDEBUG -fno-optimize-sibling-calls -w -ffunction-sections -fdata-sections -flto -mmcu=$3 -DF_CPU=${6}L -DARDUINO=10819 -DARDUINO_$5 -DARDUINO_ARCH_AVR -I$SRC -I$CORE/variants/$4"
	for f in "$SRC"/*.c "$SRC"/*.cpp; do
		case $f in
		*.c) avr-gcc $FL -std=gnu11 -fno-fat-lto-objects "$f" -o "$O/$(basename "$f").o" ;;
		*) avr-g++ $FL -std=gnu++11 -fpermissive -fno-exceptions -fno-threadsafe-statics "$f" -o "$O/$(basename "$f").o" ;;
		esac
	done
	avr-gcc-ar rcs "$O/core.a" "$O"/*.o
	avr-g++ $FL -std=gnu++11 -fno-exceptions -fno-threadsafe-statics "$HERE/$2" -o "$O/sketch.o"
	avr-gcc -w -Os -g -flto -fuse-linker-plugin -Wl,--gc-sections -mmcu=$3 -o "$O/s.elf" "$O/sketch.o" "$O/core.a" -lm
}
sym() { avr-nm "$1" | awk -v s="$2" '$3 == s { sub(/^0+/, "", $1); print $1 }'; }

fail=0
test_case() {	# name sketch mcu variant board f_cpu [high]
	echo "=== $1"
	build "$1" "$2" "$3" "$4" "$5" "$6"
	local E=$B/$1/s.elf
	local T=$(sym "$E" _Z6targetv) M=$(sym "$E" timer0_millis)
	M=$(printf '%x' $((0x$M & 0xffff)))
	# high: a second breakpoint 128 KB lower, same lower 16 bits: must not hit
	local N=; [ "$7" = high ] && N=$(printf '%x' $((0x$T - 0x20000)))
	"$B/harness" "$E" "$3" "$6" "$T" "$M" $N 2>/dev/null || fail=1
}
test_case uno        sketch.cpp      atmega328p standard AVR_UNO      16000000
test_case pro-8mhz   sketch.cpp      atmega328p standard AVR_PRO       8000000
test_case nano-168   sketch.cpp      atmega168  standard AVR_NANO     16000000
test_case pro-168-8  sketch.cpp      atmega168  standard AVR_PRO       8000000
test_case mega       sketch.cpp      atmega2560 mega     AVR_MEGA2560 16000000
test_case mega-high  sketch_high.cpp atmega2560 mega     AVR_MEGA2560 16000000 high
test_case mega1280   sketch.cpp      atmega1280 mega     AVR_MEGA     16000000
test_case uno-print  sketch_print.cpp atmega328p standard AVR_UNO     16000000
test_case mega-print sketch_print.cpp atmega2560 mega     AVR_MEGA2560 16000000
[ $fail = 0 ] && echo "ALL PASSED" || echo "SOME TESTS FAILED"
exit $fail
