#ifndef __FAKE_CC1101_H__
#define __FAKE_CC1101_H__

// A stateful stand-in for the chip, sitting behind the fake Arduino and SPI
// primitives so the real, unmodified src/cc1101.cpp can be driven on a desktop.
// See docs/adr/0003-test-the-invariants-not-the-trace.md for why the thing
// under test is the thing that gets flashed, with no seam introduced for the
// tests' benefit.
//
// This models only what a wiring check touches: the register file, the two
// identity registers, and how GDO0 follows IOCFG0. It has no FIFO and no state
// machine — a wiring check never leaves IDLE.

#include <stdint.h>
#include <string.h>

// ESP8266 HSPI pins, as the core defines them for the nodemcu board.
#define PIN_SPI_SS 15
#define PIN_SPI_MOSI 13
#define PIN_SPI_MISO 12
#define PIN_SPI_SCK 14

// Status registers live at 0x30-0x3D and are reached with the burst bit set,
// which is why PARTNUM's 0xF0 masks down to 0x30.
#define FAKE_STATUS_BASE 0x30

/**
 * How GDO0 is physically connected. The wiring check exists to tell these
 * apart, so they are the inputs the tests vary.
 */
enum FakeGdo0Wiring
{
    GDO0_CONNECTED,    // Follows whatever the chip drives
    GDO0_DISCONNECTED, // Floats; the ESP's INPUT_PULLUP holds it high
    GDO0_SHORTED_LOW,  // Tied to ground, so it never rises
};

struct FakeCC1101
{
    uint8_t regs[0x40];
    uint8_t partNumber;
    uint8_t version;
    FakeGdo0Wiring gdo0;
    // Set false to model SPI not reaching the chip at all: MISO stuck high, so
    // CHIP_RDYn never asserts and every register reads back as 0xFF.
    bool spiConnected;
    // Every IOCFG0 value written, in order. The restore invariant is asserted
    // over this rather than over the final register value, so a check that
    // never touched IOCFG0 cannot be mistaken for one that restored it.
    uint8_t iocfg0Writes[16];
    uint8_t iocfg0WriteCount;

    void reset()
    {
        memset(regs, 0, sizeof(regs));
        // Power-on default for IOCFG0 (datasheet Table 43).
        regs[0x02] = 0x3F;
        partNumber = 0x00;
        version = 0x14;
        gdo0 = GDO0_CONNECTED;
        spiConnected = true;
        iocfg0WriteCount = 0;
        memset(iocfg0Writes, 0, sizeof(iocfg0Writes));
    }
};

/**
 * One chip shared by every translation unit. A function-local static rather
 * than a header-scope object, which would give cc1101.cpp and the test file a
 * copy each and silently test nothing.
 */
inline FakeCC1101 &fakeChip()
{
    static FakeCC1101 chip;
    return chip;
}

/** Milliseconds since boot, advanced only by delay(). */
inline uint32_t &fakeMillis()
{
    static uint32_t ms = 0;
    return ms;
}

/**
 * @brief The level the GDO0 pin actually presents to the ESP.
 *
 * The pull-up is the reason this is interesting: a disconnected wire reads
 * high, which is indistinguishable from a chip legitimately driving it high.
 * Only asking for a low can tell them apart.
 */
inline bool fakeGdo0Level()
{
    FakeCC1101 &chip = fakeChip();

    if (chip.gdo0 == GDO0_DISCONNECTED)
        return true; // INPUT_PULLUP
    if (chip.gdo0 == GDO0_SHORTED_LOW)
        return false;

    uint8_t iocfg0 = chip.regs[0x02];
    bool inverted = (iocfg0 & 0x40) != 0;
    // GDO_CFG 0x2F drives the pin to 0. Every other setting the driver uses is
    // an event source that is not firing while the chip sits in IDLE, so the
    // pin rests low either way — the GDOx_INV bit is what produces a high.
    bool driven = false;
    return inverted ? !driven : driven;
}

/**
 * @brief Serve one SPI transaction in place, as the chip would.
 *
 * data[0] carries the header on the way in and the chip status byte on the way
 * out, which is what the driver's halRf* helpers expect.
 */
inline void fakeSpiTransfer(uint8_t *data, int len)
{
    FakeCC1101 &chip = fakeChip();

    if (!chip.spiConnected)
    {
        memset(data, 0xFF, len);
        return;
    }

    uint8_t header = data[0];
    uint8_t addr = header & 0x3F;
    bool isRead = (header & 0x80) != 0;
    bool isBurst = (header & 0x40) != 0;

    // Chip status byte: CHIP_RDYn low, state IDLE, FIFO bytes 0.
    data[0] = 0x00;

    if (len == 1)
        return; // Command strobe; nothing else is exchanged.

    if (isRead)
    {
        if (addr >= FAKE_STATUS_BASE && isBurst)
        {
            // Status registers do not auto-increment.
            if (addr == FAKE_STATUS_BASE)
                data[1] = chip.partNumber;
            else if (addr == FAKE_STATUS_BASE + 1)
                data[1] = chip.version;
            else
                data[1] = chip.regs[addr];
            return;
        }

        for (int i = 1; i < len; i++)
            data[i] = chip.regs[(addr + (isBurst ? i - 1 : 0)) & 0x3F];
        return;
    }

    for (int i = 1; i < len; i++)
    {
        uint8_t target = (addr + (isBurst ? i - 1 : 0)) & 0x3F;
        chip.regs[target] = data[i];

        if (target == 0x02 && chip.iocfg0WriteCount < 16)
            chip.iocfg0Writes[chip.iocfg0WriteCount++] = data[i];
    }
}

#endif // __FAKE_CC1101_H__
