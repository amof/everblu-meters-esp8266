#ifndef __FAKE_ARDUINO_H__
#define __FAKE_ARDUINO_H__

// Minimal stand-in for the Arduino core, so the real src/*.cpp can be compiled
// and tested on a desktop. Only what the code under test actually reaches for.
// See docs/adr/0003-test-the-invariants-not-the-trace.md.

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "fake_cc1101.h"

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

typedef uint8_t byte;

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

inline void pinMode(uint8_t, uint8_t) {}

/**
 * Only two pins are ever read, and each answers from the chip model: MISO as
 * CHIP_RDYn, and everything else as GDO0 through the ESP's pull-up.
 */
inline int digitalRead(uint8_t pin)
{
    if (pin == PIN_SPI_MISO)
        return fakeChip().spiConnected ? LOW : HIGH;

    return fakeGdo0Level() ? HIGH : LOW;
}

inline void digitalWrite(uint8_t, uint8_t) {}

/**
 * Time advances only when the code under test asks to wait. A real clock would
 * make the timeout paths flaky for no gain: nothing modelled here is genuinely
 * time-dependent.
 */
inline void delay(uint32_t ms) { fakeMillis() += ms; }
inline uint32_t millis() { return fakeMillis(); }
inline void delayMicroseconds(uint32_t) {}

/**
 * Feeds the ESP's watchdog on the target. Here it advances the clock, because
 * some poll loops yield rather than delay — waitMisoLow() is one — and a
 * yield that did not let time pass would spin forever instead of timing out.
 */
inline void yield() { fakeMillis() += 1; }

// The ESP8266 core's newlib provides localtime_r, MSVC and MinGW do not. This
// is the C library standing in for the target's, same as everything above.
#ifdef _WIN32
#include <time.h>
inline struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    return localtime_s(result, timer) == 0 ? result : NULL;
}
#endif

#endif // __FAKE_ARDUINO_H__
