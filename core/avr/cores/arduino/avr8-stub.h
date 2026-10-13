/*
 * avr8-stub.h
 *
 *  Created on: 25. 3. 2015
 *      Author: Jan Dolinay
 *
 *  Header file for
 *  GDB Debugging Agent (GDB stub) for ATMega328 and ATMega1280 and ATMega2560
 * This code is actually "gdb stub" - code which runs in the debugged MCU and
 * communicates with the gdb debugger. It uses gdb RCP protocol.
 * The code needs to handle UART Receive interrupt and an interrupt for simulating
 * software interrupt (EXT INT).
 *
 * ArduDebug version: RAM breakpoints only; the software interrupt is INT0
 * (pin 2) on the ATmega328P/168/8, INT6 on the ATmega1280/2560 and the
 * ATmega32U4. The options of the original for flash breakpoints, loading
 * through GDB and other interrupt sources were removed, see
 * https://github.com/jdolinay/avr_debug
 *
 * AVR8_USER_BAUDRATE - serial port baud rate for communication with the debugger.
 *   If not provided: 115200 at 16 MHz, 57600 at 8 MHz.
 *
 * The following project were used (and combined) to create this stub:
 * 1) AVR-GDBServer (https://github.com/rouming/AVR-GDBServer)
 * 2) avr-stub from uOS - embedded operating system (https://code.google.com/p/uos-embedded/)
 * 3) AVR gdbstub (http://zevv.nl/play/code/avr-gdbstub/)
 *
 * The API is inspired by the GDB documentation for stubs: (quote)
 * The debugging stub for your architecture supplies these three subroutines:
 * set_debug_traps
 * This routine arranges for handle_exception to run when your program stops. You must call this subroutine explicitly in your program's startup code.
 * handle_exception
 * This is the central workhorse, but your program never calls it explicitly�the setup code arranges for handle_exception to run when a trap is triggered.
 * handle_exception takes control when your program stops during execution (for example, on a breakpoint), and mediates communications with gdb on the host machine. This is where the communications protocol is implemented; handle_exception acts as the gdb representative on the target machine. It begins by sending summary information on the state of your program, then continues to execute, retrieving and transmitting any information gdb needs, until you execute a gdb command that makes your program resume; at that point, handle_exception returns control to your own code on the target machine.
 * breakpoint
 * Use this auxiliary subroutine to make your program contain a breakpoint. Depending on the particular situation, this may be the only way for gdb to get control. For instance, if your target machine has some sort of interrupt button, you won't need to call this; pressing the interrupt button transfers control to handle_exception�in effect, to gdb. On some machines, simply receiving characters on the serial port may also trigger a trap; again, in that situation, you don't need to call breakpoint from your own program�simply running �target remote� from the host gdb session gets control.
 * Call breakpoint if none of these is true, or if you simply want to make certain your program stops at a predetermined point for the start of your debugging session.
 * (end quote)
 *
 * Note about porting to other AVR MCUs:
 * You need to change:
 * 1) UART code in uart_init, putChar and getChar
 * 2) Code for software interrupt emulation in gdb_enable_swinterrupt and gdb_disable_swinterrupt
 *
 * Port to Atmega 1280 (2560), Arduino Mega
 * USART routines work as is; ISR name changes.
 * PC is 3 bytes on Mega2560 and 2 bytes on 1280 as follows from Stack Pointer chapter in the datasheet:
 * The ATmega48A/PA/88A/PA/168A/PA/328/P Program Counter (PC) is 11/12/13/14 bits wide
 * The ATmega640/1280/1281/2560/2561 Program Counter (PC) is 15/16/17 bits wide
 *
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU Lesser General Public License for more details.
 *
 */

#ifndef DRV_DEBUG_H_
#define DRV_DEBUG_H_


#include <avr/io.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*  --------- Configuration ------------ */

/* USART used by the debugger; the names of its registers are built from
   this number. ArduDebug always uses USART0. */
#ifndef AVR8_UART_NUMBER
        #define AVR8_UART_NUMBER       0
#endif

/* Maximum number of breakpoints. GDB also sets temporary breakpoints, for
   example for "step into" or "run to line". */
#define AVR8_MAX_BREAKS       (8)

/**
 * Initialize the debugger driver
 */
void debug_init(void);

/**
 * Insert breakpoint at the position where this function is called.
 * To set-up breakpoints before compilation.
*/
void breakpoint(void);

/**
 * ArduDebug: pause handling. The first pause request from the debugger sets
 * debug_pause_req to 1; main() then calls debug_pause_point() before the next
 * loop() pass, so the program stops in the sketch rather than inside delay().
 * debug_pause_point() arms the stop (2), which then happens in the INT0 handler.
 */
extern volatile uint8_t debug_pause_req;
void debug_pause_point(void);

/**
 * ArduDebug: called for each character typed in the gdb-server terminal
 * while the program runs. The default does nothing; DebugSerial.cpp
 * overrides it to fill the receive buffer of Serial.
 */
void debug_rx_char(uint8_t c);

/* ArduDebug (ATmega32U4): the USB interrupt of the core calls this when the
   debugger sent data. */
void debug_usb_rx(void);
/* ArduDebug (ATmega32U4): 1 after a restart requested by the debugger */
extern uint8_t debug_restarted;
/* ArduDebug (ATmega32U4): the host closed the USB port */
void debug_usb_closed(void);


/**
 * Send text message to gdb console.
 * The function appends "\n" to the msg.
 * Note that GDB will queue the messages until "\n" (0x0a) is received; then
 * it displays the messages in console.
 */
void debug_message(const char* msg);

/**
 * Send program output to the debugger without appending "\n".
 * Unlike debug_message(), the text is sent exactly as given, so a line can
 * be continued by the next call. buf need not be null-terminated.
 * Only as many bytes as fit into one packet are sent (about 45); the
 * return value is the number of bytes sent, call again with the rest.
 * The ArduDebug bridge shows the text in the gdb-server terminal of the
 * Arduino IDE. (Added for ArduDebug.)
 */
uint8_t debug_write(const char* buf, uint8_t len);

#ifdef __cplusplus
}
#endif


#endif /* DRV_DEBUG_H_ */
