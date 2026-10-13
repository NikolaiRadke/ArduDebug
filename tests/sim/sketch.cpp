// Test sketch for the simulator test: target() is called once, as soon as
// millis() reaches 1000. The test sets a breakpoint on it.
#include <Arduino.h>
volatile uint8_t done;
uint16_t hits;
void __attribute__((noinline)) target() { hits++; }
void setup() { }
void loop() { if (!done && millis() >= 1000) { done = 1; target(); } }
