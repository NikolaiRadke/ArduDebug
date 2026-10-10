/*
  DebugSerial.cpp - Serial replacement for ArduDebug debug builds.
  Part of ArduDebug (https://github.com/NikolaiRadke/ArduDebug)
  Copyright (c) 2026 Nikolai Radke. LGPL-2.1-or-later, like the Arduino core.
*/

#ifdef ARDUDEBUG

#include "Arduino.h"
#include "avr8-stub.h"

DebugSerial Serial;

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
