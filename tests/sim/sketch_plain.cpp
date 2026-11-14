// Plain mode: no debugger announces itself, so Serial works like in a
// normal build: plain text at the rate of begin(), input goes to read().
#include <Arduino.h>
void setup() {
  Serial.begin(9600);
  Serial.println(F("Hello"));
}
void loop() {
  while (Serial.available()) {
    Serial.write('<');
    Serial.write(Serial.read());
    Serial.write('>');
  }
}
