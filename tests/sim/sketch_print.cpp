// Like sketch.cpp, but busy with Serial output: interrupts (millis()) must
// keep running while the text goes to the debugger.
#include <Arduino.h>
volatile uint8_t done;
uint16_t hits;
void __attribute__((noinline)) target() { hits++; }
void check() { if (!done && millis() >= 1000) { done = 1; target(); } }
void setup() { Serial.begin(115200); }
void loop() {
  Serial.println("0123456789012345678901234567890123456789");
  check();
  Serial.println(F("Text from flash, sent in pieces of 16 characters"));
  check();
}
