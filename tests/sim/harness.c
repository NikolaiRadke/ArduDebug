/*
 * ArduDebug simulator test: runs a debug build in simavr and talks to
 * avr8-stub over UART0 like GDB does.
 *
 * usage: harness <elf> <mcu> <f_cpu> <target> <millis> [<neg>]
 *   f_cpu   clock in Hz; the stub uses 115200 baud at 16 MHz, else 57600
 *   target  byte address of target() (breakpoint, hex)
 *   millis  RAM address of timer0_millis (hex)
 *   neg     optional: a breakpoint that must never be hit (hex)
 *
 * The test sketch calls target() once, as soon as millis() >= 1000.
 * Exit code 0: all checks passed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <simavr/sim_avr.h>
#include <simavr/sim_elf.h>
#include <simavr/avr_uart.h>
#include <simavr/sim_interrupts.h>

static avr_t *avr;
static char rx[4096];
static int rxn;
static avr_irq_t *uart_in;
static const char *txq;
static int txpos, txlen;
static avr_cycle_count_t next_tx, byte_cycles;
static uint32_t millis_addr;
static int failed;

/* The stub's software interrupt: a low-level INTx on an output pin held low */
static avr_int_vector_t *swv;
static unsigned swv_num, pin_addr, pin_bit, eimsk_addr = 0x3D, eimsk_bit;

static void uart_out(struct avr_irq_t *irq, uint32_t v, void *p)
{
	if (rxn == (int)sizeof(rx) - 1) {	/* full of program output: keep the end */
		memmove(rx, rx + rxn - 256, 256);
		rxn = 256;
	}
	rx[rxn++] = (char)v;
	rx[rxn] = 0;
}

static double now(void) { return (double)avr->cycle / avr->frequency; }

static uint32_t millis(void)
{
	uint8_t *d = avr->data + millis_addr;
	return d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
}

/* simavr only reacts to pin changes. On the chip a low-level interrupt is
   requested as long as the pin is low and the interrupt is enabled, and it
   simply vanishes while masked. simavr instead keeps a queue, and a stale
   entry there (masked, or no longer pending) uses up the single chance for
   an interrupt after SEI. So: keep only real pending entries in the queue. */
DEFINE_FIFO(avr_int_vector_p, avr_int_pending);

static void level_int(void)
{
	int want = (avr->data[eimsk_addr] & (1u << eimsk_bit)) &&
		   !(avr->data[pin_addr] & (1u << pin_bit));
	if (!want)
		swv->pending = 0;
	avr_int_table_p t = &avr->interrupts;
	int n = avr_int_pending_get_read_size(&t->pending);
	for (int i = 0; i < n; i++) {
		avr_int_vector_p v = avr_int_pending_read(&t->pending);
		if (v->pending)
			avr_int_pending_write(&t->pending, v);
	}
	if (want && !swv->pending)
		avr_raise_interrupt(avr, swv);
	if (avr->interrupt_state > 0 && !avr_has_pending_interrupts(avr))
		avr->interrupt_state = 0;
}

static void run_until(double t)
{
	while (now() < t) {
		if (txq && txpos < txlen && avr->cycle >= next_tx) {
			avr_raise_irq(uart_in, (uint8_t)txq[txpos++]);
			next_tx = avr->cycle + byte_cycles;
		}
		level_int();
		int st = avr_run(avr);
		if (st == cpu_Done || st == cpu_Crashed) {
			printf("FAIL: CPU stopped (%d) at %.3f s\n", st, now());
			exit(1);
		}
	}
}

static int wait_for(const char *s, double timeout)
{
	double end = now() + timeout;
	while (now() < end) {
		run_until(now() + 0.001);
		char *f = strstr(rx, s);
		if (f) {
			/* a packet: wait for its end, '#' and two checksum digits */
			while (f[0] == '$' && now() < end) {
				char *h = strchr(f, '#');
				if (h && h[1] && h[2])
					break;
				run_until(now() + 0.001);
			}
			return 1;
		}
	}
	return 0;
}

static void send(const char *s) { txq = s; txpos = 0; txlen = strlen(s); }

static char pkt[128];
static const char *packet(const char *body)
{
	unsigned cs = 0;
	for (const char *p = body; *p; p++)
		cs += (unsigned char)*p;
	snprintf(pkt, sizeof pkt, "$%s#%02x", body, cs & 0xff);
	return pkt;
}

/* send a packet and wait for the reply, after acknowledging the last one */
static int command(const char *body, const char *expect, double timeout)
{
	rxn = 0; rx[0] = 0;
	send("+");
	run_until(now() + 0.001);
	send(packet(body));
	return wait_for(expect, timeout);
}

/* PC from a stop reply "$T..22:xxxxxxxx;" (little endian, byte address) */
static uint32_t stop_pc(void)
{
	char *p = strstr(rx, ";22:");
	unsigned b[4] = {0};
	if (p)
		sscanf(p + 4, "%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3]);
	return b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failed = 1;
}

int main(int argc, char **argv)
{
	if (argc < 6) {
		printf("usage: %s elf mcu f_cpu target millis [neg]\n", argv[0]);
		return 2;
	}
	elf_firmware_t fw;
	memset(&fw, 0, sizeof fw);
	if (elf_read_firmware(argv[1], &fw)) {
		printf("cannot read %s\n", argv[1]);
		return 2;
	}
	strcpy(fw.mmcu, argv[2]);
	fw.frequency = strtoul(argv[3], 0, 10);
	avr = avr_make_mcu_by_name(fw.mmcu);
	if (!avr) {
		printf("unknown MCU %s\n", argv[2]);
		return 2;
	}
	avr_init(avr);
	avr->log = LOG_NONE;
	avr_load_firmware(avr, &fw);
	uint32_t target = strtoul(argv[4], 0, 16);
	millis_addr = strtoul(argv[5], 0, 16);
	uint32_t neg = argc > 6 ? strtoul(argv[6], 0, 16) : 0;
	/* 10 bits per byte at the stub's baud rate */
	byte_cycles = fw.frequency * 10 / (fw.frequency >= 16000000 ? 115200 : 57600);

	if (!strcmp(argv[2], "atmega2560") || !strcmp(argv[2], "atmega1280")) {
		swv_num = 7; pin_addr = 0x2C; pin_bit = 6; eimsk_bit = 6;	/* INT6, PE6 */
	} else if (!strcmp(argv[2], "atmega8")) {
		swv_num = 1; pin_addr = 0x30; pin_bit = 2;			/* INT0, PD2 */
		eimsk_addr = 0x5B; eimsk_bit = 6;				/* in GICR */
	} else {
		swv_num = 1; pin_addr = 0x29; pin_bit = 2; eimsk_bit = 0;	/* INT0, PD2 */
	}
	for (int i = 0; i < avr->interrupts.vector_count; i++)
		if (avr->interrupts.vector[i]->vector == swv_num)
			swv = avr->interrupts.vector[i];
	if (!swv) {
		printf("vector %u not found\n", swv_num);
		return 2;
	}
	avr_irq_t *out = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT);
	avr_irq_register_notify(out, uart_out, NULL);
	uart_in = avr_io_getirq(avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_INPUT);

	run_until(0.2);
	check(millis() >= 190, "sketch runs, millis() counts");

	rxn = 0; rx[0] = 0;
	send("\x03");
	check(wait_for("$T", 0.5), "pause stops the program");

	char b[64];
	snprintf(b, sizeof b, "Z0,%x,2", target);
	check(command(b, "OK", 0.1), "breakpoint set");
	if (neg) {
		snprintf(b, sizeof b, "Z0,%x,2", neg);
		check(command(b, "OK", 0.1), "second breakpoint set");
	}
	command("c", "", 0.005);
	uint32_t m0 = millis();
	run_until(now() + 0.3);
	check(millis() - m0 >= 280, "millis() keeps counting with breakpoints set");

	int hit = wait_for("$T", 1.5);
	check(hit, "breakpoint hit");
	if (!hit)
		return 1;
	check(stop_pc() == target, "stopped at target()");
	check(millis() >= 1000 && millis() < 1010, "stopped right when millis() reached 1000");
	check(now() > 0.99 && now() < 1.05, "millis() in step with the clock");

	uint32_t pc = stop_pc();
	int steps_ok = 1;
	for (int i = 0; i < 3; i++) {
		if (!command("s", "#", 0.1)) { steps_ok = 0; break; }
		uint32_t n = stop_pc();
		if (n <= pc || n > pc + 4) steps_ok = 0;
		pc = n;
	}
	check(steps_ok, "three single steps");

	snprintf(b, sizeof b, "z0,%x,2", target);
	check(command(b, "OK", 0.1), "breakpoint removed");
	command("c", "", 0.005);
	m0 = millis();
	rxn = 0; rx[0] = 0;
	run_until(now() + 0.5);
	check(millis() - m0 >= 480, "program runs on, millis() counts");
	check(!strstr(rx, "$T"), "no further stop");

	printf(failed ? "FAILED\n" : "PASSED\n");
	return failed;
}
