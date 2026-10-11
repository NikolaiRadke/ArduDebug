/*
  DebugSerial.h - Serial replacement for ArduDebug debug builds.
  Part of ArduDebug (https://github.com/NikolaiRadke/ArduDebug)
  Copyright (c) 2026 Nikolai Radke. LGPL-2.1-or-later, like the Arduino core.

  In debug builds, avr8-stub needs the serial port for itself. DebugSerial
  sends all output through the debugger instead; the ArduDebug bridge shows
  it in the gdb-server terminal of the Arduino IDE. What is typed in that
  terminal arrives in a 64 byte receive buffer, like with HardwareSerial.
*/

#ifndef DebugSerial_h
#define DebugSerial_h

#include "Stream.h"

class DebugSerial : public Stream
{
  public:
    void begin(unsigned long) {}
    void begin(unsigned long, uint8_t) {}
    void end() {}
    virtual int available(void);
    virtual int peek(void);
    virtual int read(void);
    virtual int availableForWrite(void) { return 64; }
    virtual void flush(void) {}
    virtual size_t write(uint8_t c) { return write(&c, 1); }
    virtual size_t write(const uint8_t *buffer, size_t size);
    using Print::write;
    operator bool() { return true; }
};

#endif
