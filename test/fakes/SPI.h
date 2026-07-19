#ifndef __FAKE_SPI_H__
#define __FAKE_SPI_H__

// Stand-in for the ESP8266 SPI library. Transactions are handed straight to the
// chip model in fake_cc1101.h, so the driver's own halRf* helpers — header
// byte, status byte, burst bit — are exercised rather than bypassed.

#include <stdint.h>
#include "fake_cc1101.h"

#define MSBFIRST 1
#define SPI_MODE0 0

class SPISettings
{
public:
    SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class FakeSPI
{
public:
    void pins(uint8_t, uint8_t, uint8_t, uint8_t) {}
    void begin() {}
    void beginTransaction(SPISettings) {}
    void endTransaction() {}

    /** Burst form, exchanged in place. */
    void transfer(void *buf, size_t len) { fakeSpiTransfer((uint8_t *)buf, (int)len); }

    /** Single-byte form, used by reset() to issue SRES. */
    uint8_t transfer(uint8_t value)
    {
        uint8_t frame[1] = {value};
        fakeSpiTransfer(frame, 1);
        return frame[0];
    }
};

extern FakeSPI SPI;

#endif // __FAKE_SPI_H__
