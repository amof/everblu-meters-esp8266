#ifndef __FAKE_ARDUINO_H__
#define __FAKE_ARDUINO_H__

// Minimal stand-in for the Arduino core, so the real src/*.cpp can be compiled
// and tested on a desktop. Only what the code under test actually reaches for.
// See docs/adr/0003-test-the-invariants-not-the-trace.md.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/**
 * The logging surface used by utils.cpp. Writes to stdout so a failing test
 * can still show the buffer it was comparing.
 */
class FakeSerial
{
public:
    template <typename... Args>
    void printf(const char *fmt, Args... args) { ::printf(fmt, args...); }
    void println() { ::printf("\n"); }
    void println(const char *s) { ::printf("%s\n", s); }
    void print(const char *s) { ::printf("%s", s); }
};

extern FakeSerial Serial;

#endif // __FAKE_ARDUINO_H__
