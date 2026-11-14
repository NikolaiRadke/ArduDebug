/*
  DebugSerial.h - Serial replacement for ArduDebug debug builds.
  Part of ArduDebug (https://github.com/NikolaiRadke/ArduDebug)
  Copyright (c) 2026 Nikolai Radke. LGPL-2.1-or-later, like the Arduino core.

  In debug builds, avr8-stub needs the serial port for itself. DebugSerial
  sends all output through the debugger instead; the ArduDebug bridge shows
  it in the gdb-server terminal of the Arduino IDE. What is typed in that
  terminal arrives in a 64 byte receive buffer, like with HardwareSerial.
  Without a debugger, Serial works like in a normal build: plain text (Uno,
  Nano, Mega: at the rate of begin()), readable in the Serial Monitor.
*/

#ifndef DebugSerial_h
#define DebugSerial_h

#include "Stream.h"

#if defined(__AVR_ATmega32U4__)
extern "C" uint8_t debug_cdc_open(void);	// CDC.cpp
#endif

class DebugSerial : public Stream
{
  public:
    // Without a debugger (plain mode) begin() sets the baud rate like
    // HardwareSerial does. With a debugger the stub keeps its own rate.
    void begin(unsigned long baud) { begin(baud, 0x06); }	// SERIAL_8N1
    void begin(unsigned long baud, uint8_t config);
    void end() {}
    virtual int available(void);
    virtual int peek(void);
    virtual int read(void);
    virtual int availableForWrite(void) { return 64; }
    virtual void flush(void) {}
    virtual size_t write(uint8_t c) { return write(&c, 1); }
    virtual size_t write(const uint8_t *buffer, size_t size);
    using Print::write;
#if defined(__AVR_ATmega32U4__)
    // Leonardo: true once a program (the debugger) has opened the USB port
    operator bool() { return debug_cdc_open(); }
#else
    operator bool() { return true; }
#endif
};

#endif
