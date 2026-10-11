/*
  DebugSerial.cpp - Serial replacement for ArduDebug debug builds.
  Part of ArduDebug (https://github.com/NikolaiRadke/ArduDebug)
  Copyright (c) 2026 Nikolai Radke. LGPL-2.1-or-later, like the Arduino core.
*/

#ifdef ARDUDEBUG

#include "Arduino.h"
#include "avr8-stub.h"

DebugSerial Serial;

// Receive buffer for characters typed in the gdb-server terminal.
// The stub calls debug_rx_char() from its interrupt; the sketch reads with
// read(). One byte stays free to tell "full" from "empty".
#define RX_SIZE 64
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint8_t rx_head, rx_tail;

// Replaces the weak default in avr8-stub.c as soon as the sketch uses Serial.
// Called with interrupts disabled. A full buffer drops the character.
extern "C" void debug_rx_char(uint8_t c)
{
  uint8_t next = (rx_head + 1) % RX_SIZE;
  if (next != rx_tail) {
    rx_buf[rx_head] = c;
    rx_head = next;
  }
}

int DebugSerial::available(void)
{
  return (uint8_t)(RX_SIZE + rx_head - rx_tail) % RX_SIZE;
}

int DebugSerial::peek(void)
{
  if (rx_head == rx_tail) return -1;
  return rx_buf[rx_tail];
}

int DebugSerial::read(void)
{
  if (rx_head == rx_tail) return -1;
  uint8_t c = rx_buf[rx_tail];
  rx_tail = (rx_tail + 1) % RX_SIZE;
  return c;
}

// Sends the text in as many debugger packets as needed.
size_t DebugSerial::write(const uint8_t *buffer, size_t size)
{
  size_t done = 0;
  while (done < size) {
    uint8_t chunk = (size - done > 255) ? 255 : (uint8_t)(size - done);
    done += debug_write((const char *)buffer + done, chunk);
  }
  return size;
}

#endif
