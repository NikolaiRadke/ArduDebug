/*
 * avr8-stub.c
 *
 *  Created on: 25. 3. 2015
 *      Author: Jan Dolinay
 *
 *  GDB Debugging Agent (GDB stub) for ATMega328
 *
 *  ArduDebug version: RAM breakpoints only, for the ATmega328P/328/168/8
 *  (UART), ATmega1280/2560 (UART0) and ATmega32U4 (USB). The original
 *  supports more chips and modes (flash breakpoints, loading the program
 *  through GDB); that code was removed here, see the original project:
 *  https://github.com/jdolinay/avr_debug
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
 * Modified for ArduDebug (https://github.com/NikolaiRadke/ArduDebug)
 * by Nikolai Radke:
 *  - 2026-10-09: Bounds check in packet receive (handle_exception).
 *    Newer GDB versions send a qSupported packet longer than
 *    AVR8_MAX_BUFF, which overflowed the buffer and corrupted the
 *    saved registers.
 *  - 2026-10-10: debug_write() sends output without appending "\n".
 *  - 2026-10-10: Ctrl-C while RAM breakpoints are active really stops the
 *    target now. Before, it ran on after reporting the stop, and GDB
 *    crashed on the next step (internal-error in infrun.c).
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <util/delay.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "avr8-stub.h"

#if defined(__AVR_ATmega8__)
/* ArduDebug: the ATmega8 has the same external interrupt registers under
   older names (the bits INT0/INTF0/ISC0x are the same) */
	#define EIMSK	GICR
	#define EIFR	GIFR
	#define EICRA	MCUCR
#endif



typedef uint8_t bool_t;
#define FALSE 0
#define TRUE 1


/* Configuration */

/* Baudrate configuration - based on Optiboot code */

/* Set default value if baudrate is not provided */
#ifndef AVR8_USER_BAUDRATE
#if F_CPU >= 16000000L
	#define GDB_USART_BAUDRATE   115200L
#elif F_CPU >= 8000000L
	/* ArduDebug: 115200 would be 3.5 % off at 8 MHz, 57600 only 2.1 %.
	   The bridge uses the same rule (build.f_cpu). */
	#define GDB_USART_BAUDRATE   57600L
#elif F_CPU >= 1000000L
	#define GDB_USART_BAUDRATE   9600L
#elif F_CPU >= 128000L
	#define GDB_USART_BAUDRATE   4800L
#else
	#define GDB_USART_BAUDRATE 	1200L
#endif
#else
	#define GDB_USART_BAUDRATE AVR8_USER_BAUDRATE
#endif

/* Calculate the prescaler value for UART */
#define GDB_BAUD_PRESCALE (( (F_CPU + GDB_USART_BAUDRATE * 4L) / ((GDB_USART_BAUDRATE * 8L))) - 1 )
#define BAUD_ACTUAL (F_CPU/(8 * ((GDB_BAUD_PRESCALE)+1)))

/* Check if the baudrate will work */
#if BAUD_ACTUAL <= GDB_USART_BAUDRATE
  #define BAUD_ERROR (( 100*(GDB_USART_BAUDRATE - BAUD_ACTUAL) ) / GDB_USART_BAUDRATE)
  #if BAUD_ERROR >= 5
    #error Baudrate off by more than -5%
  #elif BAUD_ERROR >= 3
    #warning Baud rate off by more than -3%
  #endif
#else
  #define BAUD_ERROR (( 100*(BAUD_ACTUAL - GDB_USART_BAUDRATE) ) / GDB_USART_BAUDRATE)
  #if BAUD_ERROR >= 5
    #error Baud rate off by more than 5%
  #elif BAUD_ERROR >= 3
    #warning Baud rate off by more than 3%
  #endif
#endif

/* check for slow baudrate */
#if GDB_BAUD_PRESCALE > 250
	#error Unachievable baud rate (too slow)
#endif
/* check for fast baudrate */
#if (GDB_BAUD_PRESCALE - 1) < 3
#if BAUD_ERROR != 0 	/* permit high bitrates (i.e. 1Mbps@16MHz) if error is zero */
	#error Unachievable baud rate (too fast)
#endif
#endif


/*
 * Macros used in this file which change value based on options set in header
 */

/* The software interrupt: a low-level external interrupt on a pin that the
 * stub drives low. It fires after every instruction while it is enabled
 * (single steps, RAM breakpoints).
 * AVR8_SWINT_PIN 	- the pin used for generating SW interrupt
 * AVR8_SWINT_INTMASK 	- bit in EIMSK to enable the interrupt and in EIFR to clear the flag
 * AVR8_SWINT_SC        - mask that clears the sense control bits (low level)
 * AVR8_SWINT_EICR      - the external interrupt control register
 * AVR8_SWINT_DDR       - data direction register
 * AVR8_SWINT_PORT      - port register
 * AVR8_SWINT_VECT      - the interrupt vector
 */
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
	/* ArduDebug: Mega: INT6 on PE6, which is not on any pin of the board */
	#define	AVR8_SWINT_PIN		(PE6)
	#define AVR8_SWINT_INTMASK	(INTF6)
	#define AVR8_SWINT_SC		(~(_BV(ISC61) | _BV(ISC60)))
	#define AVR8_SWINT_EICR		EICRB
	#define AVR8_SWINT_DDR		DDRE
	#define AVR8_SWINT_PORT		PORTE
	#define AVR8_SWINT_VECT		INT6_vect
#elif defined(__AVR_ATmega32U4__)
	/* ArduDebug: Leonardo/Micro. INT0 is SCL there, so INT6 on PE6 (pin 7). */
	#define	AVR8_SWINT_PIN		(PE6)
	#define AVR8_SWINT_INTMASK	(INT6)
	#define AVR8_SWINT_SC		(~(_BV(ISC61) | _BV(ISC60)))
	#define AVR8_SWINT_EICR		EICRB
	#define AVR8_SWINT_DDR		DDRE
	#define AVR8_SWINT_PORT		PORTE
	#define AVR8_SWINT_VECT		INT6_vect
#else	/* Arduino Uno */
	#define	AVR8_SWINT_PIN		(PD2)
	#define AVR8_SWINT_INTMASK	(INT0)
	#define AVR8_SWINT_SC		(~(_BV(ISC01) | _BV(ISC00)))
	#define AVR8_SWINT_EICR		EICRA
	#define AVR8_SWINT_DDR		DDRD
	#define AVR8_SWINT_PORT		PORTD
	#define AVR8_SWINT_VECT		INT0_vect
#endif

/*
 * Other defines and macros
 */
#define ARRAY_SIZE(arr) (sizeof(arr)/sizeof((arr)[0]))
#define MIN(i1, i2) (i1 < i2 ? i1 : i2);

/** Size of the buffer we use for receiving messages from gdb in HEX.
  IMPORTANT: There is a decimal value below which must be in sync!
  AVR8_MAX_BUFF_HEX MUST BE IN HEX!!!
  because it is used in a macro and sent directly to GDB.
  The size must not be lower than 79 bytes (see gdb_read_registers)*/
#define AVR8_MAX_BUFF_HEX	60
/* This is decimal equivalent of AVR8_MAX_BUFF_HEX, which we used in code to allocate buffers etc.
   Keep this in sync with AVR8_MAX_BUFF_HEX!!! */
#define	AVR8_MAX_BUFF		(96)



/* Reason the program stopped sent to GDB:
 * These are similar to unix signal numbers.
   See signum.h on unix systems for the values. */
#define GDB_SIGINT  2      /* Interrupt (ANSI). - user interrupted the program (UART ISR) */
#define GDB_SIGTRAP 5      /* Trace trap (POSIX). - stopped by this stub, e.g. on a breakpoint */


/* SRAM_OFFSET is hard-coded in GDB, it is there to map the separate address space of AVR (Harvard)
* to linear space used by GDB. So when GDB wants to read from RAM address 0 it asks our stub for
* address 0x00800000.
* FLASH_OFFSET is always 0
* MEM_SPACE_MASK is used to clear the RAM offset bit. It should not affect the highest possible
* address in flash which is 17-bit for Atmega2560, that is why 0xfffE0000.
*  */
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)

	#define MEM_SPACE_MASK 0x00fe0000
	#define FLASH_OFFSET   0x00000000
	#define SRAM_OFFSET    0x00800000
	
	/* AVR puts garbage in high bits of return address on stack.
   	   Mask them out */
	// Atmega 1280 PC is 16 bit according to data sheet; Program memory is addressed by words, not bytes.
	// Atmega 2560 PC is 17 bits. We mask the 3rd (HIGH-HIGH) byte of PC in this case, not the 2nd (HIGH) byte.
#if defined(__AVR_ATmega2560__)
	#define RET_ADDR_MASK  0x01
#else
	#define RET_ADDR_MASK  0xff
#endif

#else
	/* This is for ATmega328 */
	#define MEM_SPACE_MASK 0x00ff0000
	#define FLASH_OFFSET   0x00000000
	#define SRAM_OFFSET    0x00800000	/* GDB works with linear address space; RAM address from GBD will be (real addresss + 0x00800000)*/

	/* AVR puts garbage in high bits of return address on stack.
   	   Mask them out
 	 The original version used mask 0x1f, which does not work for code above 16 kB!
	 In 1/2017 changed to 0x3f. The PC is 14 bits on Atmega328 (13 bits on Atmega168), we must
	 out the higher byte, so the mask is actually: 0x3fFF to keep 14 bits.
	 In the code this is used when reading 2 B from RAM to mask the first byte adr+0; adr+1 is not masked
	*/
#if defined(__AVR_ATmega8__)
	#define RET_ADDR_MASK  0x0f	/* ArduDebug: 12-bit PC */
#else
	#define RET_ADDR_MASK  0x3f
#endif
#endif

/* UART definitions */

/* The number in AVR8_UART_NUMBER is used to generate generic registry names for the USART.
   The code in this stub then uses these generic names and the USART used by this stub
   can be changed simply by changing the AVR8_UART_NUMBER value in avr8-stub.h
*/

/* Check if AVR8_UART_NUMBER is valid for selected MCU */
#if defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__) 
	/* ATmega1280 and ATmega2560 have 4 USARTs */
	#if AVR8_UART_NUMBER < 0 || AVR8_UART_NUMBER > 3
		#error For ATmega1280 or ATmega2560 only UART 0 to 3 can be used. Plese change the AVR8_UART_NUMBER to a number between 0 and 3.
	#endif
#elif defined(__AVR_ATmega328__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
	/* There is only USART0 on ATmega328 (ArduDebug: and on the ATmega168) */
	#if AVR8_UART_NUMBER != 0
		#error For ATmega328 only UART0 can be used. Plese change the AVR8_UART_NUMBER to 0.
	#endif
#elif defined(__AVR_ATmega32U4__)
	/* ArduDebug: no UART; the stub talks over the USB serial port (CDC) */
	#define AVR8_USB_CDC	1
#elif defined(__AVR_ATmega8__)
	/* ArduDebug: one USART; its registers have no number (UCSRA, UDR...) */
	#define AVR8_UART_PLAIN	1
#else
	#error Selected MCU is not supported by this stub
#endif
#ifndef AVR8_USB_CDC
	#define AVR8_USB_CDC	0
#endif


/* A helper macro is needed to expand AVR8_UART_NUMBER macro to the actual number,
   because macro arguments used with ## operator are not expanded, so 
   we can't pass AVR8_UART_NUMBER directly to this macro, but must do so via the 
   next macro - this expands the AVR8_UART_NUMBER to its value. */
#define 	MAKE_UART_NAME_(name1, number, name2) name1 ## number ## name2
/* create name of the register or bit based on */
#define		MAKE_UART_NAME(name1 , num, name2 ) MAKE_UART_NAME_(name1, num, name2 )

/* Create the actual registry and bit names for UART */
#ifndef AVR8_UART_PLAIN
#define 	UCSRA	MAKE_UART_NAME(UCSR, AVR8_UART_NUMBER, A)
#define 	UCSRB	MAKE_UART_NAME(UCSR, AVR8_UART_NUMBER, B)
#define		UCSRC	MAKE_UART_NAME(UCSR, AVR8_UART_NUMBER, C)
#define		UBRRH	MAKE_UART_NAME(UBRR, AVR8_UART_NUMBER, H)
#define		UBRRL	MAKE_UART_NAME(UBRR, AVR8_UART_NUMBER, L)
#define		RXEN	MAKE_UART_NAME(RXEN, AVR8_UART_NUMBER, )
#define		TXEN	MAKE_UART_NAME(TXEN, AVR8_UART_NUMBER, )
#define		UCSZ0	MAKE_UART_NAME(UCSZ, AVR8_UART_NUMBER, 0)
#define		UCSZ1	MAKE_UART_NAME(UCSZ, AVR8_UART_NUMBER, 1)
#define		RXCIE	MAKE_UART_NAME(RXCIE, AVR8_UART_NUMBER, )
#define		UDRE	MAKE_UART_NAME(UDRE, AVR8_UART_NUMBER, )
#define		UDR		MAKE_UART_NAME(UDR, AVR8_UART_NUMBER, )
#define		RXC		MAKE_UART_NAME(RXC, AVR8_UART_NUMBER, )
#define		U2X		MAKE_UART_NAME(U2X, AVR8_UART_NUMBER, )
#endif

/* Define the USART vector name */
#if defined(__AVR_ATmega328__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
	#define	UART_ISR_VECTOR	USART_RX_vect
#elif defined(__AVR_ATmega8__)
	#define	UART_ISR_VECTOR	USART_RXC_vect
#else
	/* the other MCUs have USART number in the ISR name */
	#define	MAKE_UART_VECT_(number)		USART ## number ## _RX_vect
	#define	MAKE_UART_VECT(num)			MAKE_UART_VECT_(num)
	#define	UART_ISR_VECTOR	MAKE_UART_VECT(AVR8_UART_NUMBER)	
#endif


/* To insert size of the buffer into PacketSize reply*/
#define STR(s) #s
#define STR_VAL(s) STR(s)

/* Data type to hold address of breakpoint.
   Devices with up to 16 bit PC will use uint16_t for breakpoint but Atmega 2560
   needs bigger data type */
#if defined(__AVR_ATmega2560__)
typedef uint32_t Address_t;
#else
typedef uint16_t Address_t;
#endif




/**
 * Data used by this driver.
 */
struct gdb_context
{
    uint8_t singlestep_enabled; // made it the first field so that addressing this field is easy!
	uint16_t sp;
	Address_t pc; /* PC is 17-bit on ATmega2560*/


	/* RAM only breakpoints */

	/* On ATmega2560 the PC is 17-bit. We could use uint32_t for all MCUs, but
	 * that would be a waste of RAM and also all the manipulation with 32-bit will
	 * be much slower than with 16-bit variable, to we use Address_t which changes
	 * to 32 bits for ATmega2560.  */
	Address_t breaks [AVR8_MAX_BREAKS+1];	/* Breakpoints */

	uint8_t breakpoint_enabled;		/* At least one BP is set */
	uint8_t breaks_cnt;				/* number of valid breakpoints inserted */
	uint8_t buff[AVR8_MAX_BUFF+1];
	uint8_t buff_sz;
	uint8_t target_running;		/* 0 if target program is stopped */
};


/* Convert number 0-15 to hex */
#define nib2hex(i) (uint8_t)((i) > 9 ? 'a' - 10 + (i) : '0' + (i))

/* ArduDebug: shared helpers instead of the nib2hex()/hex2nib() code repeated
   at every place. Both work little endian, like GDB expects the registers. */
static uint8_t hex2nib(uint8_t hex);
__attribute__((noinline))
static uint8_t *mem2hex(uint8_t *dst, const uint8_t *src, uint8_t n)
{
	while (n--) {
		uint8_t b = *src++;
		*dst++ = nib2hex(b >> 4);
		*dst++ = nib2hex(b & 0xf);
	}
	return dst;
}
__attribute__((noinline))
static const uint8_t *hex2mem(uint8_t *dst, const uint8_t *src, uint8_t n)
{
	while (n--) {
		uint8_t b = hex2nib(*src++) << 4;
		*dst++ = b | hex2nib(*src++);
	}
	return src;
}


/*  Prototypes of internal functions */

/* UART routines
 * Names taken from GDB documentation for stub; int replaced by uint8_t */
static uint8_t getDebugChar(void);		/* Read a single character from the serial port */
static void putDebugChar(uint8_t c);	/* Write a single character to serial port */
#if !AVR8_USB_CDC
static void uart_init(void);			/* Our function to initialize UART */
#endif
#if AVR8_USB_CDC
static void debug_usb_clear(void);
#endif
static void handle_exception(void);	/* Function called when the program stops */
static inline void gdb_enable_swinterrupt();
static inline void gdb_disable_swinterrupt();

static uint8_t hex2nib(uint8_t hex);
static uint8_t parse_hex(const uint8_t *buff, uint32_t *hex);
static void gdb_send_buff(const uint8_t *buff, uint8_t sz);

/* ArduDebug: set to 1 by the first pause request (Ctrl-C) while the program
   runs. main() checks it before each loop() pass and calls debug_pause_point(),
   which sets it to 2; the INT0 handler then stops and sets it back to 0. */
volatile uint8_t debug_pause_req;
/* ArduDebug: stack pointer inside debug_pause_point(); the INT0 handler only
   stops once the program has returned from there (SP above this value). */
static volatile uint16_t debug_pause_sp;
/* ArduDebug: Cortex-Debug 1.5.1 keeps only the first symbol per address. The
   first variable in .data and in .bss shares its address with the section
   symbols and would be missing from the "Global" list. These two bytes take
   the first place in each section (plain .data/.bss come before .data.* and
   .bss.* in the linker script). debug_init() uses them so they are kept.
   The .bss byte doubles as the flag "debugger announced", see debug_init(). */
static volatile uint8_t debug_data_pad __attribute__((section(".data"))) = 1;
static volatile uint8_t debug_bss_pad __attribute__((section(".bss")));

/* ArduDebug: receives each character typed in the gdb-server terminal.
   Called with interrupts disabled. This default drops it; DebugSerial.cpp
   provides the real one when the sketch uses Serial. */
__attribute__((weak)) void debug_rx_char(uint8_t c)
{
	(void)c;
}
static void gdb_send_reply(const char *reply);	/* ArduDebug: reply is in flash */
static bool_t gdb_parse_packet(const uint8_t *buff);
static void gdb_send_state(uint8_t signo);
static void gdb_write_registers(const uint8_t *buff);
static void gdb_read_registers(void);
static void gdb_write_memory(const uint8_t *buff);
static void gdb_read_memory(const uint8_t *buff);
static void gdb_insert_remove_breakpoint(const uint8_t *buff);
static bool_t gdb_insert_breakpoint(Address_t rom_addr);
static void gdb_remove_breakpoint(Address_t rom_addr);





static inline void restore_regs (void);
static inline void save_regs1 (void);
static inline void save_regs2 (void);

#if defined(__AVR_ATmega2560__)
static uint8_t safe_pgm_read_byte(uint32_t rom_addr_b);
#endif

/* Helpers for determining required size of our stack and for printing to console*/



/* Debugging using the logic analyzer */


/* Code used only if flash writing is needed */

/* Global variables */

/* Our context and pointer to this context.
 * The pointer is used in the original code which "creates" the struct on stack in ISR.
 * I keep it so as not to change all the functions even though the context is saved to regs array. */
static struct gdb_context ctx;
static struct gdb_context *gdb_ctx;

/* String for PacketSize reply to gdb query.
 * Note: if running out of RAM the reply to qSupported packet can be removed. */
static const char gdb_str_packetsz[] PROGMEM = "PacketSize=" STR_VAL(AVR8_MAX_BUFF_HEX);

/* ArduDebug: all fixed replies live in flash, each only once, instead of
   being copied to RAM at startup. */
static const char gdb_str_ok[]    PROGMEM = "OK";
static const char gdb_str_e05[]   PROGMEM = "E05";
static const char gdb_str_e50[]   PROGMEM = "E50";
static const char gdb_str_s05[]   PROGMEM = "S05";
static const char gdb_str_qc01[]  PROGMEM = "QC01";
static const char gdb_str_m1[]    PROGMEM = "m1";
static const char gdb_str_l[]     PROGMEM = "l";
static const char gdb_str_empty[] PROGMEM = "";

/* PC is 17-bit on ATmega2560; we need 1 more byte for registers but since we will
 * work with the PC as uint32 we need one extra byte; the code then assumes this byte
 * is always zero. */
#if defined(__AVR_ATmega2560__)
	#define GDB_NUMREGBYTES	(39)			/* Total bytes in registers */
#else
	#define GDB_NUMREGBYTES	(37)
#endif

/* GDB_STACKSIZE - size of the internal stack used by this stub.
 * ArduDebug: measured in the simulator (tests/sim) with all GDB commands
 * the stub answers: 44 bytes used on the ATmega328P, 50 on the ATmega2560. */
#define GDB_STACKSIZE 	(80)

static char stack[GDB_STACKSIZE];			/* Internal stack used by the stub */
static unsigned char regs[GDB_NUMREGBYTES];	/* Copy of all registers */

#if defined(__AVR_ATmega2560__)
	#define R_PC	*(uint32_t*)(regs+35)	/* Copy of PC register */
#else
	#define R_PC	*(uint16_t*)(regs+35)	/* Copy of PC register */
#endif
#define	R_PC_H  *(uint8_t*)(regs+36)	/* High byte of the Copy of the PC */
#if defined(__AVR_ATmega2560__)	/* PC is 17-bit on ATmega2560*/
	#define	R_PC_HH  *(uint8_t*)(regs+37)	/* High-High byte of the Copy of the PC */
#endif
#define R_SP	*(uint16_t*)(regs+33)	/* Copy of SP register */
#define R_SREG	*(uint8_t*)(regs+32)	/* Copy of SREG register */




/* ---------- User interface (API) for this driver ------------- */

/* Initialize the debug driver. */
void debug_init(void)
{
	if (debug_data_pad) { }	/* keep the .data pad, see above */

	/* Init gdb context */
	gdb_ctx = &ctx;
	gdb_ctx->sp = 0;
	gdb_ctx->breaks_cnt = 0;
	gdb_ctx->buff_sz = 0;
	gdb_ctx->singlestep_enabled = 0;
	gdb_ctx->target_running = 0;

	/* Init breaks */
	memset(gdb_ctx->breaks, 0, sizeof(gdb_ctx->breaks));


	gdb_ctx->breakpoint_enabled = 0;


#if !AVR8_USB_CDC
	/* Initialize serial port */
	uart_init();
#endif



	/* ArduDebug: listen 20 ms for the bridge's "debugger is coming" (0x02,
	   sent every 10 ms after the reset). If it comes, the UART interrupt
	   stops the program right here, before setup(). Without a debugger the
	   sketch just starts 20 ms later. Interrupts are on: init() did sei().
	   The loop ends as soon as the flag is set: once GDB continues with a
	   breakpoint set, every instruction is checked and runs very slowly. */
#if AVR8_USB_CDC
	/* ArduDebug: after a restart by the debugger, main() calls this once
	   the bridge has opened the USB port again: stop here and wait for GDB.
	   Otherwise the sketch just starts; the debugger can connect later. */
	debug_usb_clear();
	if (debug_restarted)
		breakpoint();
#else
	for (uint8_t i = 0; i < 200 && !debug_bss_pad; i++)
		_delay_us(100);
#endif
}



/* ------------ User interface (API) for this driver ---------------- */


/* ---------- UART communication routines  ------------- */

#if AVR8_USB_CDC
/* ArduDebug: USB transport for the ATmega32U4. The stub polls the CDC
   endpoints of the Arduino USB core directly, with interrupts off, just as
   it polls the UART on the Uno. Endpoint numbers: see USBDesc.h. */
#define DBG_EP_OUT	2	/* CDC_RX */
#define DBG_EP_IN	3	/* CDC_TX */


uint8_t debug_cdc_open(void);	/* CDC.cpp: 1 if the host has the port open */

/* ArduDebug: when the debugger connects while the sketch runs, the board
   restarts through the watchdog and the sketch starts over, stopped before
   setup(), like a Uno after its reset. The request survives the reset in
   RAM at 0x0802, right behind the bootloader key at 0x0800: the Caterina
   bootloader clears its key at every start, but leaves this place alone. */
#define DEBUG_KEY_ADDR		((volatile uint16_t *)0x0802)
#define DEBUG_RESTART_KEY	0xDEB6
/* 1: restarted for the debugger, waiting for its 0x02; 2: answered it,
   further 0x02 of the same announcement are answered too; 0: anything else.
   A 0x02 in state 0 comes from a new debugger session: start over. */
uint8_t debug_restarted __attribute__((section(".noinit")));

/* Runs right after the reset, before the variables are initialized */
__attribute__((naked, used, section(".init3")))
static void debug_check_restart(void)
{
	debug_restarted = (*DEBUG_KEY_ADDR == DEBUG_RESTART_KEY);
	*DEBUG_KEY_ADDR = 0;
	MCUSR = 0;
	wdt_disable();
}

static void debug_restart(void)
{
	*DEBUG_KEY_ADDR = DEBUG_RESTART_KEY;
	wdt_enable(WDTO_15MS);
	for (;;) ;
}

/* 1 once GDB has talked to the stub since the last reset */
static uint8_t debug_session;

/* ArduDebug: the debugger closed the USB port (DTR off). Like a Uno, which
   the closing resets through DTR, the sketch starts over, now without the
   debugger. Called from the core's USB interrupt (CDC.cpp) or from the stub
   while the program is stopped. Only arms the watchdog, so the USB request
   can still be answered. */
void debug_usb_closed(void)
{
	if (debug_session) {
		*DEBUG_KEY_ADDR = 0;
		wdt_enable(WDTO_60MS);
	}
}

/* ArduDebug: while the program is stopped, interrupts are off and the core
   cannot answer the host's requests on the control endpoint. The stub only
   handles DTR (port opened or closed): closing the port must restart the
   sketch. Other requests get a STALL; they only come when a stopped board
   is opened again, which normally does not happen. */
static void debug_usb_control(void)
{
	uint8_t type, req, val;

	UENUM = 0;
	if (!(UEINTX & _BV(RXSTPI)))
		return;
	type = UEDATX;
	req  = UEDATX;
	val  = UEDATX;
	UEINTX = ~_BV(RXSTPI);
	if (type == 0x21 && req == 0x22) {		/* SET_CONTROL_LINE_STATE */
		UEINTX = ~_BV(TXINI);			/* status stage */
		if (!(val & 0x01))
			debug_usb_closed();		/* port closed */
	} else {
		UECONX |= _BV(STALLRQ);			/* not supported now */
	}
}

/* ArduDebug: set by debug_usb_rx() when the debugger sent something while
   the program runs; cleared once the stub has read the whole packet. */
static volatile uint8_t debug_usb_pending;

/* Packet read: let the USB interrupt report the next one (see debug_usb_rx()) */
static void debug_usb_clear(void)
{
	debug_usb_pending = 0;
	UEIENX |= _BV(RXOUTE);
}

/* Send what is waiting in the IN bank */
static void usb_flush(void)
{
	UENUM = DBG_EP_IN;
	if (UEBCLX)
		UEINTX = 0x3A;		/* release the bank, like ReleaseTX() */
}

/* Read a single character from USB */
static uint8_t getDebugChar(void)
{
	uint8_t c;

	usb_flush();			/* the host waits for our answer first */
	UENUM = DBG_EP_OUT;
	for (;;) {
		/* RXOUTI first: only then is the byte count of the bank valid.
		   Checking the count first could release a packet that arrived
		   just in between. */
		if (UEINTX & _BV(RXOUTI)) {
			if (UEBCLX)
				break;
			UEINTX = 0x6B;	/* empty packet: release, like ReleaseRX() */
		}
		debug_usb_control();
		UENUM = DBG_EP_OUT;
	}
	c = UEDATX;
	if (!UEBCLX) {
		UEINTX = 0x6B;		/* bank read completely: release it */
		debug_usb_clear();
	}
	return c;
}

/* Write a single character to USB */
static void putDebugChar(uint8_t c)
{
	UENUM = DBG_EP_IN;
	while (!(UEINTX & _BV(RWAL))) {	/* wait for a free bank */
		debug_usb_control();	/* the host may have closed the port */
		UENUM = DBG_EP_IN;
	}
	UEDATX = c;
	if (!(UEINTX & _BV(RWAL)))
		UEINTX = 0x3A;		/* bank full: send it */
}
#else
/* Initialize UART */
static void uart_init(void)
{
	/* Init UART */
	UCSRA = _BV(U2X);		/* double UART speed */
	UCSRB = (1 << RXEN ) | (1 << TXEN );		/* enable RX and Tx */
#ifdef URSEL
	UCSRC = (1 << URSEL) | (1 << UCSZ0) | (1 << UCSZ1);	/* ATmega8: URSEL selects UCSRC */
#else
	UCSRC =  (1 << UCSZ0 ) | (1 << UCSZ1 ); /* Use 8- bit character sizes */
#endif
	UBRRH = ( GDB_BAUD_PRESCALE >> 8);
	UBRRL = GDB_BAUD_PRESCALE;
	UCSRB |= (1 << RXCIE ); /* Enable the USART Recieve Complete interrupt ( USART_RXC ) */

}

/* Read a single character from the serial port */
static uint8_t getDebugChar(void)
{
	/* wait for data to arrive */
	while ( !(UCSRA & (1<<RXC)) )
		;

	return (uint8_t)UDR;
}

/* Write a single character to serial port */
static void putDebugChar(uint8_t c)
{
	/* Wait for empty transmit buffer */
	while ( !( UCSRA & (1<<UDRE)) )
		;

	/* Put data into buffer, sends the data */
	UDR = c;
}
#endif
/* ---------------- end UART communication routines ---------------------------------- */



/* ---------- Debugging driver routines  ------------- */
/**
 * Stimulate interrupt;
 * Used for single stepping or when any breakpoint in RAM is set to stop the
 * program after every instruction.
 * The AVR core will always execute one instruction in the main code before
 * jumping into ISR even if interrupt is pending. We set the INT0 to trigger when
 * pin is low and set the pin low. */
__attribute__((always_inline))
static inline void gdb_enable_swinterrupt()
{
	AVR8_SWINT_EICR &= AVR8_SWINT_SC;
	/* The pin needs to be configured as output to allow us to set the
	 * level on the pin and thus generate the interrupt*/
	AVR8_SWINT_DDR |= _BV(AVR8_SWINT_PIN);		/* set pin to output mode */
	EIFR |= _BV(AVR8_SWINT_INTMASK);	/* clear INTx flag */
	EIMSK |= _BV(AVR8_SWINT_INTMASK);	/* enable INTx interrupt */
	AVR8_SWINT_PORT &= ~_BV(AVR8_SWINT_PIN);		/* make sure the pin is low */
}

/** Disable the interrupt used for single stepping and RAM breakpoints. */
__attribute__((always_inline))
static inline void gdb_disable_swinterrupt()
{
	EIMSK &= ~_BV(AVR8_SWINT_INTMASK);
}

#if AVR8_USB_CDC
/* ArduDebug: called from the USB interrupt of the core (USBCore.cpp) when
   the debugger sent data. Like the UART interrupt on the Uno, it gets the
   stub going: the software interrupt fires right after the return. */
void debug_usb_rx(void)
{
	UENUM = DBG_EP_OUT;
	UEIENX &= ~_BV(RXOUTE);		/* again once the packet was read */
	debug_usb_pending = 1;
	/* With breakpoints, stepping or an armed pause the software interrupt
	   is active anyway and its handler checks the flag. Enabling it here
	   would be wrong then: this may run inside the handler's window for the
	   other interrupts, and it would fire nested in the handler itself. */
	if ( !gdb_ctx->breakpoint_enabled && !gdb_ctx->singlestep_enabled && debug_pause_req != 2 )
		gdb_enable_swinterrupt();
}
#endif

/** Macro which is true if there is a pending interrupt from UART Rx
 * */
#if AVR8_USB_CDC
#define	UART_RXINT_PENDING()  (debug_usb_pending)
#else
#define	UART_RXINT_PENDING()  (UCSRA & (1<<RXC))
#endif

/* handle_exception
 * Called when the program stops; communicates with the GDB.
 * The name follows the GDB stub documentation:
 * This is the central workhorse.
 * This is where the communications protocol is implemented;
 * handle_exception acts as the gdb representative on the target machine.
 * It begins by sending summary information on the state of your program,
 * then continues to execute, retrieving and transmitting any information gdb needs,
 * until you execute a gdb command that makes your program resume;
 * at that point, handle_exception returns control to your own code on the target machine.
 * */
__attribute__((optimize("-Os")))
static void handle_exception(void)
{
	uint8_t checksum, pkt_checksum;
	uint8_t b;


	gdb_ctx->singlestep_enabled = 0;		/* stepping by single instruction is enabled below for each step */


	while (1) {

		b = getDebugChar();
		
		switch(b) {
		case '$':
#if AVR8_USB_CDC
			debug_restarted = 0;	/* GDB talks: the announcement is over */
			debug_session = 1;
#endif
			/* Read everything to buffer */
			gdb_ctx->buff_sz = 0;
			for (pkt_checksum = 0, b = getDebugChar();
				 b != '#'; b = getDebugChar())
			{
				/* ArduDebug: do not overflow the buffer, drop the rest.
				   The checksum still covers all characters. */
				if (gdb_ctx->buff_sz < AVR8_MAX_BUFF)
					gdb_ctx->buff[gdb_ctx->buff_sz++] = b;
				pkt_checksum += b;
			}
			gdb_ctx->buff[gdb_ctx->buff_sz] = 0;

			checksum  = hex2nib(getDebugChar()) << 4;
			checksum |= hex2nib(getDebugChar());

			/* send nack in case of wrong checksum  */
			if (pkt_checksum != checksum) {
				putDebugChar('-');
				continue;
			}

			/* ack */
			putDebugChar('+');

			/* parse already read buffer */
			if (gdb_parse_packet(gdb_ctx->buff))
				continue;

			 

			if(gdb_ctx->singlestep_enabled || gdb_ctx->breakpoint_enabled)
			{
				/* this will generate interrupt after one instruction in main code */
				gdb_enable_swinterrupt();

			}
			else
			{
				/* Do not go to ext int ISR after every instruction. For RAM breakpoints this is
				  the case if the program is let run and it can be only stopped by break command (pause).
				  For flash breakpoints it is the same plus the program can also break itself when it encounters
				  breakpoint in flash.	*/
				gdb_disable_swinterrupt();
			}

			/* leave the trap, continue execution */
			return;

		case '-':  /* NACK, repeat previous reply */
			gdb_send_buff(gdb_ctx->buff, gdb_ctx->buff_sz);
			break;
		
		case 0x02:
			/* ArduDebug: "debugger is coming". The bridge sends this after the
			   reset until we answer. The first one stops the program right
			   after debug_init(), before setup(); we confirm with 0x02 and
			   wait here for GDB. The flag ends the wait in debug_init(). */
			debug_bss_pad = 1;
#if AVR8_USB_CDC
			/* the debugger connects: start over, also if the program
			   was left stopped by an earlier session */
			if ( gdb_ctx->target_running || !debug_restarted )
				debug_restart();
			debug_restarted = 2;
#else
			if ( gdb_ctx->target_running )
				return;
#endif
			putDebugChar(0x02);
			break;

#if AVR8_USB_CDC
		case 0x04:
			/* ArduDebug: restart button. The bridge sends this instead of
			   resetting the board through DTR, which has no effect here. */
			debug_restart();	/* does not return */
			/* fall through */
#endif

		case 0x01:
			/* ArduDebug: a character typed in the gdb-server terminal follows.
			   The bridge sends these only while the program runs, so just pass
			   it on and let the program continue, like after '+' below. */
			debug_rx_char(getDebugChar());
			if ( gdb_ctx->target_running )
				return;
			break;

		case '+':  /* ACK, great */
			/* there is special case we need to handle - if we send something to the GDB while the 
			target is running, and GDB returns ACK, we need to let the target run instead of 
			waiting here for commands from GDB - because there will be none and the target hangs.
			This happens with the debug_message() which can be sent while target is running. 
			Probably it would be safe to always return from here as the UART ISR would call us again when
			another char arrives, but the GDB stub documentation above states that we should wait for
			communication until there is command to run the target... */
			if ( gdb_ctx->target_running ) 
				return;				
			break;
		
		case 0x03:					
			/* user interrupt by Ctrl-C, send current state and
			   continue reading */
			/* ArduDebug: the first request only marks the pause; the program
			   stops before its next loop() pass. A second one stops at once. */
			if ( gdb_ctx->target_running && !debug_pause_req ) {
				debug_pause_req = 1;
				return;
			}
			debug_pause_req = 0;
			gdb_ctx->target_running = 0;	/* stopped by debugger break */
			gdb_send_state(GDB_SIGINT);
			break;

		default:
			gdb_send_reply(gdb_str_empty); /* not supported */
			break;
		}	// switch
	}	// while 1

	/* should never get here, see while(1) above. */		

}

/* This is the main "dispatcher" of commands from GDB
 * If returns false, the debugged program continues execution */
__attribute__((optimize("-Os")))
static bool_t gdb_parse_packet(const uint8_t *buff)
{

	
	switch (*buff) {
	case '?':               /* last signal */
		gdb_send_reply(gdb_str_s05); /* signal # 5 is SIGTRAP */
		break;
	case 'H':               /* Set thread, always OK */
		gdb_send_reply(gdb_str_ok);
		break;
	case 'T':               /* Is thread alive, always OK */
		gdb_send_reply(gdb_str_ok);
		break;
	case 'g':               /* read registers */
		gdb_read_registers();
		break;
	case 'G':               /* write registers */
		gdb_write_registers(buff + 1);
		break;

	case 'm':               /* read memory */
		gdb_read_memory(buff + 1);
		break;
	case 'M':               /* write memory */
		gdb_write_memory(buff + 1);
		break;

		/* Support loading program from gdb */

	case 'D':               /* detach the debugger */
	case 'k':               /* kill request */
		gdb_send_reply(gdb_str_ok);
		gdb_ctx->target_running = 1;
		return FALSE;

	case 'c':               /* continue */


		gdb_ctx->target_running = 1;
		return FALSE;

	case 'C':               /* continue with signal */
	case 'S':               /* step with signal */
		gdb_send_reply(gdb_str_empty); /* not supported */
		break;

	case 's':               /* step */



		gdb_ctx->singlestep_enabled = 1;		
		return FALSE;

	case 'z':               /* remove break/watch point */
	case 'Z':               /* insert break/watch point */
		gdb_insert_remove_breakpoint(gdb_ctx->buff);
		break;

	case 'q':               /* query requests */
		/* ArduDebug: the second character is enough to tell the queries
		   apart that we answer. "monitor reset" (qRcmd) is handled by the
		   bridge and never reaches the stub. */
		if (buff[1] == 'S' && buff[2] == 'u') {		/* qSupported */
			gdb_send_reply(gdb_str_packetsz);
		}
		else if (buff[1] == 'C' && buff[2] == 0) {	/* qC: current thread is always 1 */
			gdb_send_reply(gdb_str_qc01);
		}
		else if (buff[1] == 'f' && buff[2] == 'T') {	/* qfThreadInfo: always 1 thread */
			gdb_send_reply(gdb_str_m1);
		}
		else if (buff[1] == 's' && buff[2] == 'T') {	/* qsThreadInfo: end of list */
			gdb_send_reply(gdb_str_l);
		}
		else {
			gdb_send_reply(gdb_str_empty);  /* not supported */
		}

		break;

	default:
		gdb_send_reply(gdb_str_empty);  /* not supported */
		break;
	}
	
	return TRUE;
}



__attribute__((optimize("-Os")))
static void gdb_insert_remove_breakpoint(const uint8_t *buff)
{
	uint32_t rom_addr_b, sz;
	uint8_t len;
	/* skip 'z0,' */
	len = parse_hex(buff + 3, &rom_addr_b);
	/* skip 'z0,xxx,' */
	parse_hex(buff + 3 + len + 1, &sz);

	/* get break type */
	switch (buff[1]) {
	case '0': /* software breakpoint */
		if (buff[0] == 'Z') {
			if ( gdb_insert_breakpoint(rom_addr_b >> 1) )
				gdb_send_reply(gdb_str_ok);
			else {
				gdb_send_reply(gdb_str_e50);	/* Unable to set breakpoint */
				/* The error code does not seem to make any difference... */
			}
		}
		else {
			gdb_remove_breakpoint(rom_addr_b >> 1);
			gdb_send_reply(gdb_str_ok);
		}
		break;

	default:
		/* we do not support other breakpoints, only software */
		gdb_send_reply(gdb_str_empty);
		break;
	}
}

/* rom_addr is in words */
__attribute__((optimize("-Os")))
static bool_t gdb_insert_breakpoint(Address_t rom_addr)
{

	uint8_t i;
	Address_t* p = gdb_ctx->breaks;
	/* First look if the breakpoint already exists */
	for (i = gdb_ctx->breaks_cnt; i > 0; i--)
	{
		if (*p++ == rom_addr)
			return TRUE;
	}

	if ( gdb_ctx->breaks_cnt >= AVR8_MAX_BREAKS)
		return FALSE;	/* no more breakpoints available */

	gdb_ctx->breaks[gdb_ctx->breaks_cnt++] = rom_addr;
	gdb_ctx->breakpoint_enabled = 1;	/* at least one breakpoint exists */
	return TRUE;

}

static void gdb_remove_breakpoint(Address_t rom_addr)
{
	/* ArduDebug: find it, move the rest one place down */
	uint8_t i, n = gdb_ctx->breaks_cnt;
	Address_t *b = gdb_ctx->breaks;

	for (i = 0; i < n; i++) {
		if (b[i] == rom_addr) {
			for (n--; i < n; i++)
				b[i] = b[i + 1];
			b[n] = 0;
			gdb_ctx->breaks_cnt = n;
			break;
		}
	}

	if ( n == 0 )
		gdb_ctx->breakpoint_enabled = 0;	/* if there are no breakpoints */

}

/* ----------------- Functions for flash breakpoints support ------------------- */

/* END Functions for flash breakpoints version only */



/* GDB needs the 32 8-bit, gpw registers (r00 - r31), the
   8-bit SREG, the 16-bit SP (stack pointer) and the 32-bit PC
   (program counter). Thus need to send a reply with
   r00, r01, ..., r31, SREG, SPL, SPH, PCL, PCH,
   low bytes before high since AVR is little endian.
   This routine requires (32 gpwr, SREG, SP, PC) * 2 hex bytes
   space of buffer, i.e. min (32 + 1 + 2 + 4) * 2 = 78 */
__attribute__((optimize("-Os")))
static void gdb_read_registers(void)
{
	uint32_t pc = (uint32_t)gdb_ctx->pc << 1;	/* gdb uses byte addresses */
	uint8_t *p = gdb_ctx->buff;

	p = mem2hex(p, regs, 33);			/* R0..R31 and SREG */
	p = mem2hex(p, (uint8_t*)&gdb_ctx->sp, 2);	/* SP */
	p = mem2hex(p, (uint8_t*)&pc, 4);		/* PC, 32 bit */

	gdb_ctx->buff_sz = p - gdb_ctx->buff;
	gdb_send_buff(gdb_ctx->buff, gdb_ctx->buff_sz);
}

__attribute__((optimize("-Os")))
static void gdb_write_registers(const uint8_t *buff)
{
	uint32_t pc;

	buff = hex2mem(regs, buff, 33);			/* R0..R31 and SREG */
	buff = hex2mem((uint8_t*)&gdb_ctx->sp, buff, 2);	/* SP */
	hex2mem((uint8_t*)&pc, buff, 4);		/* PC, 32 bit */
	gdb_ctx->pc = pc >> 1;	/* drop the lowest bit; PC addresses words */

	gdb_send_reply(gdb_str_ok);
}

__attribute__((optimize("-Os")))
static void gdb_read_memory(const uint8_t *buff)
{
	uint32_t addr, sz;
	uint8_t i, b, sram;
	uint8_t *p = gdb_ctx->buff;

	buff += parse_hex(buff, &addr);
	/* skip 'xxx,' */
	parse_hex(buff + 1, &sz);

	if ((addr & MEM_SPACE_MASK) == SRAM_OFFSET)
		sram = 1;
	else if ((addr & MEM_SPACE_MASK) == FLASH_OFFSET)
		sram = 0;
	else {
		/* posix EIO error */
		gdb_send_reply(gdb_str_e05);
		return;
	}
#if defined(__AVR_ATmega2560__)
	addr &= ~MEM_SPACE_MASK;
	for (i = 0; i < sz; ++i) {
		b = sram ? ((uint8_t*)(uintptr_t)addr)[i] : safe_pgm_read_byte(addr + i);
		p = mem2hex(p, &b, 1);
	}
#else
	/* ArduDebug: 16 bit addresses are enough below 64 KB of flash */
	uint16_t a = addr;
	for (i = (uint8_t)sz; i > 0; i--, a++) {
		b = sram ? *(uint8_t*)a : pgm_read_byte(a);
		p = mem2hex(p, &b, 1);
	}
#endif
	gdb_ctx->buff_sz = p - gdb_ctx->buff;
	gdb_send_buff(gdb_ctx->buff, gdb_ctx->buff_sz);
}

__attribute__((optimize("-Os")))
static void gdb_write_memory(const uint8_t *buff)
{
	uint32_t addr, sz;

	buff += parse_hex(buff, &addr);
	/* skip 'xxx,' */
	buff += parse_hex(buff + 1, &sz);
	/* skip , and : delimiters */
	buff += 2;

	if ((addr & MEM_SPACE_MASK) == SRAM_OFFSET) {
		addr &= ~MEM_SPACE_MASK;
		hex2mem((uint8_t*)(uintptr_t)addr, buff, sz);
	}
	else if ((addr & MEM_SPACE_MASK) == FLASH_OFFSET){
		/* posix EIO error */
		gdb_send_reply(gdb_str_e05);
		return;
	}
	else {
		/* posix EIO error */
		gdb_send_reply(gdb_str_e05);
		return;
	}
	gdb_send_reply(gdb_str_ok);
}


/* ------------------------------------------------------------- */



/* ---------- Interrupt handlers ------------- */
/* Note: GDB stub is normally expected to implement
 * void exceptionHandler (int exception_number, void *exception_address)
 * to install exception handler for given exception type (e.g. Illegal opcode),
 * but we do not need this.*/

/*
 * Interrupt handler for received character from serial port.
 * Having this allows breaking the program during execution from GDB. */
#if !AVR8_USB_CDC
ISR(UART_ISR_VECTOR, ISR_BLOCK ISR_NAKED)
{
	save_regs1 ();
	/* save_regs1 loads SREG into r29 */

	/* Sets interrupt flag = interrupts enabled; but not in real, just in the stored value */
	/* asm volatile ("ori r29, 0x80");	*/ /* user must see interrupts enabled */
	/* Disabled - I don't know why th euser should see it enabled... */
	save_regs2 ();

	
#if defined(__AVR_ATmega2560__)
	R_PC_HH &= 0x01;		/* there is only 1 bit used in the highest byte of PC (17-bit PC) */
	/* No need to mask R_PC_H */
#else
	R_PC_H &= RET_ADDR_MASK;
#endif
	gdb_ctx->pc = R_PC;
	gdb_ctx->sp = R_SP;	
	
	/* Communicate with gdb */
	handle_exception();	

	restore_regs ();

	asm volatile (
			"out	__SREG__, r31\n"	/* restore SREG */
			"lds	r31, regs+31\n"		/* real value of r31 */
			"reti \n");
	/* simplified exit code - just restore SREG, interrupts will be enabled because we execute RETI */

}
#endif	/* !AVR8_USB_CDC */


/*
 * Interrupt handler for the interrupt which allows single-stepping using the
 * feature of AVR that after each interrupt, one instruction of main program
 * is executed before any pending interrupt service routine is called.
 * Names such as INT0_vect are the same for Atmega328 and Atmega 1280/2560 */

ISR(AVR8_SWINT_VECT, ISR_BLOCK ISR_NAKED )
{
	static uint8_t ind_bks;

#if AVR8_USB_CDC	/* no UART of our own (USB: see getDebugChar()) */
	#define FP_UART_CHECK	"lds	r24, %[ucsra]\n"	/* debugger data waiting: full path */ \
				"tst	r24\n" \
				"brne	2f\n"
	#define FP_UART_MASK	""
	#define FP_UART_UNMASK	""
	#define FP_UCSRA	&debug_usb_pending
	#define FP_RXC		0
	#define FP_UCSRB	0
	#define FP_NORXCIE	0
#else
	#define FP_UART_CHECK	"lds	r24, %[ucsra]\n"	/* UART byte waiting: full path */ \
				"sbrc	r24, %[rxc]\n" \
				"rjmp	2f\n"
	#define FP_UART_MASK	"lds	r24, %[ucsrb]\n" \
				"mov	r25, r24\n" \
				"andi	r24, %[norxcie]\n" \
				"sts	%[ucsrb], r24\n"
	#define FP_UART_UNMASK	"sts	%[ucsrb], r25\n"
	#define FP_UCSRA	_SFR_MEM_ADDR(UCSRA)
	#define FP_RXC		RXC
	#define FP_UCSRB	_SFR_MEM_ADDR(UCSRB)
	#define FP_NORXCIE	((uint8_t)~_BV(RXCIE))
#endif
#if defined(__AVR_ATmega8__)
	/* GICR is above the I/O registers that cbi/sbi can reach */
	#define FP_SWINT_OFF	"in	r24, %[eimsk]\n" \
				"andi	r24, %[noint0]\n" \
				"out	%[eimsk], r24\n"
	#define FP_SWINT_ON	"in	r24, %[eimsk]\n" \
				"ori	r24, %[int0bit]\n" \
				"out	%[eimsk], r24\n"
#else
	#define FP_SWINT_OFF	"cbi	%[eimsk], %[int0]\n"
	#define FP_SWINT_ON	"sbi	%[eimsk], %[int0]\n"
#endif
#if defined(__AVR_ATmega2560__)
	/* 17-bit PC: the return address has 3 bytes (high byte first), the
	   breakpoints are 32-bit words (low byte first). r29 and r23 extra. */
	#define FP_PUSH_X	"push	r29\n" \
				"push	r23\n"
	#define FP_POP_X	"pop	r23\n" \
				"pop	r29\n"
	#define FP_RET_ADDR	"ldd	r23, Z+11\n"		/* high byte */ \
				"ldd	r25, Z+12\n" \
				"ldd	r24, Z+13\n"		/* low byte */ \
				"andi	r23, %[mask]\n"
	#define FP_BRK_CMP	"ld	r30, X+\n" \
				"ld	r31, X+\n" \
				"ld	r29, X+\n" \
				"adiw	r26, 1\n"			/* skip the 4th byte */ \
				"cp	r30, r24\n" \
				"cpc	r31, r25\n" \
				"cpc	r29, r23\n"
#else
	#define FP_PUSH_X	""
	#define FP_POP_X	""
	#define FP_RET_ADDR	"ldd	r25, Z+9\n"		/* high byte */ \
				"ldd	r24, Z+10\n"		/* low byte */ \
				"andi	r25, %[mask]\n"
	#define FP_BRK_CMP	"ld	r30, X+\n" \
				"ld	r31, X+\n" \
				"cp	r30, r24\n" \
				"cpc	r31, r25\n"
#endif
	/* ArduDebug: fast path. With a breakpoint set, this interrupt runs after
	   every single instruction of the program. Saving and restoring all
	   registers each time costs about 500 cycles. Here only a few registers
	   are saved and the return address is compared with the breakpoints.
	   Nothing to do (no step, no pause, no UART byte, no breakpoint hit):
	   return at once. Otherwise restore the registers and take the full
	   path below, which then sees exactly the same state. */
	asm volatile (
		"push	r24\n"
		"in	r24, __SREG__\n"
		"push	r24\n"
		"push	r25\n"
		"push	r26\n"
		"push	r27\n"
		"push	r28\n"
		"push	r30\n"
		"push	r31\n"
		FP_PUSH_X
		"lds	r24, %[step]\n"		/* single step: full path */
		"tst	r24\n"
		"brne	2f\n"
		"lds	r24, %[pause]\n"		/* armed pause: full path */
		"cpi	r24, 2\n"
		"breq	2f\n"
		FP_UART_CHECK
		"in	r30, __SP_L__\n"		/* return address: above the pushes */
		"in	r31, __SP_H__\n"
		FP_RET_ADDR
		"lds	r28, %[cnt]\n"
		"ldi	r26, lo8(%[brk])\n"
		"ldi	r27, hi8(%[brk])\n"
	"1:	tst	r28\n"				/* compare with each breakpoint */
		"breq	3f\n"
		FP_BRK_CMP
		"breq	2f\n"				/* breakpoint hit: full path */
		"dec	r28\n"
		"rjmp	1b\n"
	"3:"	FP_SWINT_OFF				/* nothing to do: let the other */
		FP_UART_MASK				/* interrupts run. INT0 has the */
		"sei\n"				/* highest priority and would */
		"nop\n"				/* starve them (millis, tone, */
		"cli\n"				/* Servo, Wire...). Mask INT0 and */
		FP_UART_UNMASK				/* the stub's own UART interrupt, */
		FP_SWINT_ON				/* enable interrupts for one */
							/* instruction: pending ones run */
							/* here, as if they came before */
							/* the next program instruction. */
		FP_POP_X				/* return */
		"pop	r31\n"
		"pop	r30\n"
		"pop	r28\n"
		"pop	r27\n"
		"pop	r26\n"
		"pop	r25\n"
		"pop	r24\n"
		"out	__SREG__, r24\n"
		"pop	r24\n"
		"reti\n"
	"2:"	FP_POP_X				/* restore and fall into the full path */
		"pop	r31\n"
		"pop	r30\n"
		"pop	r28\n"
		"pop	r27\n"
		"pop	r26\n"
		"pop	r25\n"
		"pop	r24\n"
		"out	__SREG__, r24\n"
		"pop	r24\n"
		:
		: [step]  "i" (&ctx.singlestep_enabled),
		  [pause] "i" (&debug_pause_req),
		  [ucsra] "i" (FP_UCSRA),
		  [rxc]   "n" (FP_RXC),
		  [mask]  "n" (RET_ADDR_MASK),
		  [cnt]   "i" (&ctx.breaks_cnt),
		  [brk]   "i" (ctx.breaks),
		  [eimsk] "n" (_SFR_IO_ADDR(EIMSK)),
		  [int0]  "n" (AVR8_SWINT_INTMASK),
		  [int0bit] "n" (_BV(AVR8_SWINT_INTMASK)),
		  [noint0] "n" ((uint8_t)~_BV(AVR8_SWINT_INTMASK)),
		  [ucsrb] "n" (FP_UCSRB),
		  [norxcie] "n" (FP_NORXCIE)
	);

	save_regs1 ();
	/*asm volatile ("ori r29, 0x80");*/	/* user must see interrupts enabled */
	/* removed enabling the interrupt for user - don't know why they should see it enabled... */

	save_regs2 ();
#if defined(__AVR_ATmega2560__)
	R_PC_HH &= RET_ADDR_MASK;		/* there is only 1 bit used in the highest byte of PC (17-bit PC) */
#else
	R_PC_H &= RET_ADDR_MASK;
#endif
	/* Note that for ATmega2560 we allocate 4 bytes for PC in the regs array so that it is safe to
	 cast to uint32 by the R_PC macro. */
	gdb_ctx->pc = R_PC;
	gdb_ctx->sp = R_SP;

	/* ArduDebug: armed pause from debug_pause_point(). While the program is
	   still inside that function, just return: the interrupt stays pending and
	   comes back after the next instruction. Stop once it is back in main(). */
	if ( debug_pause_req == 2 ) {
		if ( R_SP <= debug_pause_sp )
			goto out;
		debug_pause_req = 0;
		gdb_ctx->target_running = 0;
		gdb_send_state(GDB_SIGINT);
		handle_exception();
		goto out;
	}



#if AVR8_USB_CDC
	/* ArduDebug: data from the debugger while no breakpoint is set: handle it
	   like the UART interrupt does on the Uno (pause, typed characters). */
	if ( debug_usb_pending && !gdb_ctx->breakpoint_enabled && !gdb_ctx->singlestep_enabled ) {
		handle_exception();
		if ( !gdb_ctx->breakpoint_enabled && !gdb_ctx->singlestep_enabled && debug_pause_req != 2 )
			gdb_disable_swinterrupt();
		goto out;
	}
#endif

	/* if single-stepping, go to trap */
	if ( gdb_ctx->singlestep_enabled)
		goto trap;


	if ( gdb_ctx->breakpoint_enabled )
	{

		/* A byte from the debugger: typed character or pause request (Ctrl-C) */
		if ( UART_RXINT_PENDING() ) {
			ind_bks = getDebugChar();
			if ( ind_bks == 0x01 ) {
				/* ArduDebug: typed character, see handle_exception() */
				debug_rx_char(getDebugChar());
			} else if ( ind_bks == 0x03 && !debug_pause_req ) {
				/* ArduDebug: first request only marks the pause, see above */
				debug_pause_req = 1;
			} else if ( ind_bks == 0x03 ) {
				debug_pause_req = 0;
				gdb_ctx->target_running = 0;	/* stopped on a breakpoint or after step */
				/* need to send state as we already read the command */
				gdb_send_state(GDB_SIGINT);
				/* ArduDebug: stay stopped and talk to GDB right here. Returning would
				   let the target run on until GDB's next byte arrives, so GDB would
				   see a different PC than reported above. */
				handle_exception();
				goto out;
			}			
		}

		/* find breakpoint */
		for (ind_bks = 0; ind_bks < gdb_ctx->breaks_cnt; ind_bks++)
		{
			if (gdb_ctx->breaks[ind_bks] == gdb_ctx->pc)
				goto trap;
		}
	}

	/* Nothing */
	goto out;

trap:

	
	gdb_ctx->target_running = 0;	/* stopped on a breakpoint or after step */
	gdb_send_state(GDB_SIGTRAP);	
	handle_exception();

out:

	/* If there is pending interrupt from UART (char received), handle it - it may be
	 * request to interrupt the target. Because INT0 has higher
	 * priority, it will not allow the UART ISR to execute and this the user is not able
	 * to pause the debugged program if breakpoint is set, has to wait for the breakpoint to
	 * be hit...and if it is never hit, unreachable, there's no way to get back to debugger.
	 * 2018-03-24 Realized we DO need to handle UART here. It was not handled since 20.6.2017,
	 * see comment below. But we need to do this only for RAM breakpoints, not for single step,
	 * In single step the control goes to UART handler after each instruction.
	 *  */


	restore_regs ();
	asm volatile (
			"out	__SREG__, r31\n"	/* restore SREG */
			"lds	r31, regs+31\n"		/* real value of r31 */
			"reti \n");

}


/* This function must be naked, otherwise the stack can be corrupted
 * and save_regs1() assumes stack is not touched. */
__attribute__((naked))
void breakpoint(void)
{
	save_regs1 ();		/* also saves SREG */
	save_regs2 ();

#if defined(__AVR_ATmega2560__)
	R_PC_HH &= RET_ADDR_MASK;		/* there is only 1 bit used in the highest byte of PC (17-bit PC) */
#else
	R_PC_H &= RET_ADDR_MASK;
#endif


	gdb_ctx->pc = R_PC;
	gdb_ctx->sp = R_SP;

	gdb_ctx->target_running = 0;
	gdb_send_state(GDB_SIGTRAP);
	handle_exception();

	/* cannot just jump to shared code because it ends with RETI which would enable interrupts
	 which is not OK as we may be called with disabled interrupts */

	restore_regs ();	/* loads SREG to r31 */
	asm volatile (
		"out	__SREG__, r31\n"	/* restore SREG */
		"lds	r31, regs+31\n"		/* real value of r31 */
		"ret \n");
}

/* ArduDebug: called by main() before a loop() pass when a pause was requested.
   It does not stop here itself: it arms the pause and enables the software
   interrupt, so the stop happens in the INT0 handler right after the return
   to main(). There the reported PC is real code of the program, and stepping
   on from it works like after any other stop. */
__attribute__((noinline))
void debug_pause_point(void)
{
	debug_pause_sp = SP;
	debug_pause_req = 2;
	gdb_enable_swinterrupt();
}


/*
 * Send text message to gdb console.
 * Note: GDB will queue the messages until "\n" (0x0a) is received; then
 * it displays the messages in console.
 */
void debug_message(const char* msg)
{
	uint8_t cSREG;
	char c;
	uint8_t i = 0;
	cSREG = SREG; /* store SREG value */
	cli();	/* disable interrupts */


	gdb_ctx->buff[i++] = 'O';	/* message to display in gdb console */
	while ( *msg && i < (AVR8_MAX_BUFF-4) )
	{
		c = *msg++;
		gdb_ctx->buff[i++] = nib2hex((c >> 4) & 0xf);
		gdb_ctx->buff[i++] = nib2hex((c >> 0) & 0xf);
	}

	/* Add "\n" */
	gdb_ctx->buff[i++] = '0';
	gdb_ctx->buff[i++] = 'a';

	gdb_send_buff(gdb_ctx->buff , i);
	
	SREG = cSREG; /* restore SREG value (I-bit) */
	/* Example packet we send:
	 $O48656c6c6f2c20776f726c64210a#55 = Hello, world!\n
	 */
}

/* ArduDebug: like debug_message(), but sends len bytes without appending
   a newline. Returns the number of bytes that fit into the packet. */
uint8_t debug_write(const char* buf, uint8_t len)
{
	uint8_t cSREG = SREG;
#if AVR8_USB_CDC
	/* ArduDebug: nobody has the USB port open: drop the text. Sending would
	   wait forever for the host to fetch it. */
	if (!debug_cdc_open())
		return len;
	cli();
#else
	/* ArduDebug: only the stub's own interrupts (UART receive, software
	   interrupt) are held back while the packet goes out. All others keep
	   running: millis(), tone(), Servo... */
	cli();
	uint8_t ucsrb = UCSRB, eimsk = EIMSK;
	UCSRB = ucsrb & ~_BV(RXCIE);
	EIMSK = eimsk & ~_BV(AVR8_SWINT_INTMASK);
	SREG = cSREG;
#endif
	gdb_ctx->buff[0] = 'O';
	if (len > (AVR8_MAX_BUFF-4) / 2)
		len = (AVR8_MAX_BUFF-4) / 2;
	mem2hex(gdb_ctx->buff + 1, (const uint8_t*)buf, len);
	gdb_send_buff(gdb_ctx->buff, len * 2 + 1);
#if !AVR8_USB_CDC
	cli();
	UCSRB = ucsrb;
	EIMSK = eimsk;
#endif
	SREG = cSREG;
	return len;
}

/* ------------------------------------------------------------- */

/* ---------- GDB RCP packet processing  ------------- */

/* Simpler version */
__attribute__((optimize("-Os")))
static void gdb_send_buff(const uint8_t *buff, uint8_t sz)
{
	uint8_t sum = 0;

	putDebugChar('$');
	while ( sz-- > 0)
	{
		putDebugChar(*buff);
		sum += *buff;
		buff++;
	}

	putDebugChar('#');
	putDebugChar(nib2hex((sum >> 4) & 0xf));
	putDebugChar(nib2hex(sum & 0xf));
#if AVR8_USB_CDC
	usb_flush();
#endif
}

__attribute__((optimize("-Os")))
/* ArduDebug: reply is in flash (PROGMEM) */
static void gdb_send_reply(const char *reply)
{
	gdb_ctx->buff_sz = strlen_P(reply);
	if ( gdb_ctx->buff_sz > (AVR8_MAX_BUFF - 4))
		gdb_ctx->buff_sz = AVR8_MAX_BUFF - 4;

	memcpy_P(gdb_ctx->buff, reply, gdb_ctx->buff_sz);
	gdb_send_buff(gdb_ctx->buff, gdb_ctx->buff_sz);

}

/* Note: we send "expedited response T" which contains also the SREG, SP and PC
 * to speed things up.
 * Minimal response is the "last signal" response, for example $S05#b8, but then
 * GDB needs to read registers by another message.. */
__attribute__((optimize("-Os")))
static void gdb_send_state(uint8_t signo)
{
	uint32_t pc = (uint32_t)gdb_ctx->pc << 1;


	/* thread is always 1 */
	/* Real packet sent is e.g. T0520:82;21:fb08;22:021b0000;thread:1;
	 05 - last signal response
	 Numbers 20, 21,... are number of register in hex in the same order as in read registers
	 20 (32 dec) = SREG
	 21 (33 dec) = SP
	 22 (34 dec) = PC
	 */
	memcpy_P(gdb_ctx->buff,
			 PSTR("TXX20:XX;21:XXXX;22:XXXXXXXX;thread:1;"),
			 38);
	gdb_ctx->buff_sz = 38;

	mem2hex(gdb_ctx->buff + 1, &signo, 1);
	mem2hex(gdb_ctx->buff + 6, regs + 32, 1);			/* SREG */
	mem2hex(gdb_ctx->buff + 12, (uint8_t*)&gdb_ctx->sp, 2);	/* SP */
	mem2hex(gdb_ctx->buff + 20, (uint8_t*)&pc, 4);		/* PC */

	/* not in hex, send from ram */
	gdb_send_buff(gdb_ctx->buff, gdb_ctx->buff_sz);
}



/* Note: The save and restore functions must be always inline,
 otherwise the debug version will call these functions and
 destroy the stack; The functions assume the stack is not touched.
 */
__attribute__((always_inline))
static inline void save_regs1 (void)
{
	/*  20-6-2017
	 New version: gdb.c says disable interrupts as soon as possible */
	asm volatile (
				"push	r0	\n"
				"in		r0, __SREG__ \n"
				"cli	\n"			/* disable interrupts */
				"sts regs+32, r0\n"	/* save SREG to its place */
				"pop r0 \n"		/* restore r0 from stack */
				"sts	regs+31, r31\n"		/* save R31 */
				"sts	regs+30, r30\n"		/* save R30 */
				"ldi	r31, hi8(regs)\n"	/* Z points to regs */
				"ldi	r30, lo8(regs)\n"
				"std	Z+29, r29\n");		/* save R29 */
				/*"in	r29, __SREG__\n");*/	/* get SREG */

	/* Original version, unsafe if called with interrupts enabled because
	  ISR can damage registers r31,r30...
	  This is not problem in ISRs because interrupts are disabled by default
	  but in breakpoint() function this could cause troubles. */
}

__attribute__((always_inline))
static inline void save_regs2 (void)
{
	asm volatile (
	/* SREG save removed 20-6-2017 */
	/*"std	Z+32, r29\n" */		/* put SREG value to his place */
	"std	Z+28, r28\n"		/* save R28 */
	"ldi	r29, 0\n"		/* Y points to 0 */
	"ldi	r28, 0\n"

	/* R0 address is 0; this loop saves from 0 to Z, which is
	 a "pointer" to "regs" variable .	 */
	"std	Z+27, r27\n"		/* save R27 */
"1:	ld	r27, Y+\n"		/* load register 0..26 */
	"st	Z+, r27\n"		/* save it */
	"cpi	r28, 27\n"
	"brne	1b\n"


    /* Get return address from stack; if we are in ISR, this will be the address
     * where the main loop was interrupted. If we are in breakpoint() this will be
     * address of the breakpoint() function.
     * Debugger must think we are in the main loop, so we need to pop the return address
     * from the stack which it will see.
     * But we need to save this address and restore it later so that the program can
     * continue normally.
     */
#if defined(__AVR_ATmega2560__)
	"pop 	r25\n"			/* pop return address, high-high byte for AVRs with 3-byte PC */
#endif
	"pop	r27\n"			/* pop return address, high byte */
	"pop	r26\n"			/* pop return address, low byte */
	/* Disabled dropping the breakpoint() return address.
	 It seems better to return to breakpoint() normally. It would make sense if breakpoint()
	 would be called from ISR which we do not want the debugger to see */

	/* now save the return address - where we will really return */
"1:	 std	Z+35-27, r26\n"		/* save return address (PC) */
	"std	Z+36-27, r27\n"		/* -27 because from previous code Z points to regs+27*/
#if defined(__AVR_ATmega2560__)
	"std	Z+37-27, r25\n"
#endif
	"in	r26, __SP_L__\n"
	"in	r27, __SP_H__\n"
	"std	Z+33-27, r26\n"		/* save SP */
	"std	Z+34-27, r27\n"
	"clr	__zero_reg__\n");

	/* Setup internal stack */
	asm volatile (
	"out __SP_H__, %B0\n"
	"out __SP_L__, %A0"
	: : "r" (stack+GDB_STACKSIZE-1));
}


__attribute__((always_inline))
static inline void restore_regs (void)
{
	asm volatile (
	"ldi	r31, hi8(regs)\n"	/* Z points to regs */
	"ldi	r30, lo8(regs)\n"
	"ldi	r29, 0\n"		/* Y points to 0 */
	"ldi	r28, 0\n"
"1:	ld	r27, Z+\n"
	"st	Y+, r27\n"		/* restore register 0..27 */
	"cpi	r28, 28\n"
	"brne	1b\n"

	"ldd	r29, Z+33-28\n"		/* SP low */
	"out	__SP_L__, r29\n"
	"ldd	r29, Z+34-28\n"		/* SP high */
	"out	__SP_H__, r29\n"
	"ldd	r29, Z+35-28\n"		/* PC low */
	"push	r29\n"
	"ldd	r29, Z+36-28\n"		/* PC high */
	"push	r29\n"
#if defined(__AVR_ATmega2560__)
	"ldd	r29, Z+37-28\n"		/* PC high-high */
	"push	r29\n"
#endif
	"ldd	r28, Z+28-28\n"		/* restore R28 */
	"ldd	r29, Z+29-28\n"		/* restore R29 */
	"ldd	r30, Z+30-28\n"		/* restore R30 */
	"lds	r31, regs+32\n");	/* r31 = sreg */
}

/* ------------------------------------------------------------- */

/* ---------- Helpers ------------- */

/* Convert a hexadecimal digit to a 4 bit nibble. */
__attribute__((optimize("-Os")))
static uint8_t hex2nib(uint8_t hex)
{
	if (hex >= 'A' && hex <= 'F')
		return 10 + (hex - 'A');
	else if (hex >= 'a' && hex <= 'f')
		return 10 + (hex - 'a');
	else if (hex >= '0' && hex <= '9')
		return hex - '0';

	return 0xff;
}

__attribute__((optimize("-Os")))
static uint8_t parse_hex(const uint8_t *buff, uint32_t *hex)
{
	uint8_t nib, len;
	for (*hex = 0, len = 0; (nib = hex2nib(buff[len])) != 0xff; ++len)
		*hex = (*hex << 4) + nib;
	return len;
}


#if defined(__AVR_ATmega2560__)
static uint8_t safe_pgm_read_byte(uint32_t rom_addr_b)
{
	if (rom_addr_b >= (1l<<16))
		return pgm_read_byte_far(rom_addr_b);
	else
		return pgm_read_byte(rom_addr_b);
}
#endif











/* EOF */




