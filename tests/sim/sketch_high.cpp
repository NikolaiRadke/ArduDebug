// Like sketch.cpp, but target() lies above 128 KB of flash (ATmega2560):
// the breakpoint check must compare all 3 bytes of the return address.
#include <Arduino.h>
volatile uint8_t done, never;
uint16_t hits;
void __attribute__((noinline)) bigpad() { asm volatile(".rept 66000\n nop\n .endr"); }
void __attribute__((noinline, section(".text.startup.target"))) target() { hits++; }
void setup() { if (never) bigpad(); }
void loop() { if (!done && millis() >= 1000) { done = 1; target(); } }
