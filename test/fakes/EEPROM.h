#ifndef __FAKE_EEPROM_H__
#define __FAKE_EEPROM_H__

// Stand-in for the ESP8266 core's EEPROM library, backed by a plain byte array.
// See docs/adr/0003-test-the-invariants-not-the-trace.md.
//
// The target's EEPROM is a RAM cache of one flash sector: get() and put() hit
// the cache, and only commit() writes flash. That distinction is modelled here
// rather than elided, because the persistence code's correctness depends on it
// — a put() without a commit() survives until reboot on the target and would
// otherwise look identical to a persisted record in a test.

#include <stdint.h>
#include <string.h>

class FakeEEPROMClass
{
public:
    // An erased flash sector reads as 0xFF, not as zero. Several records are
    // validated by magic precisely to reject that pattern, so starting from
    // zeroed memory would skip the case those checks exist for.
    static const uint8_t ERASED = 0xFF;
    static const size_t CAPACITY = 4096;

    FakeEEPROMClass() { reset(); }

    /** Return to the state of a device that has never been written. */
    void reset()
    {
        memset(_cache, ERASED, sizeof(_cache));
        memset(_flash, ERASED, sizeof(_flash));
        commitCount = 0;
    }

    void begin(size_t size)
    {
        _size = size < CAPACITY ? size : CAPACITY;
        // begin() loads the sector into the cache; a test that pre-seeds flash
        // and then constructs the object under test sees what a reboot sees.
        memcpy(_cache, _flash, sizeof(_cache));
    }

    void end() { commit(); }

    bool commit()
    {
        memcpy(_flash, _cache, sizeof(_flash));
        commitCount++;
        return true;
    }

    template <typename T>
    T &get(int address, T &value)
    {
        if (address >= 0 && (size_t)address + sizeof(T) <= _size)
            memcpy(&value, _cache + address, sizeof(T));
        return value;
    }

    template <typename T>
    const T &put(int address, const T &value)
    {
        if (address >= 0 && (size_t)address + sizeof(T) <= _size)
            memcpy(_cache + address, &value, sizeof(T));
        return value;
    }

    uint8_t read(int address) { return _cache[address]; }
    void write(int address, uint8_t value) { _cache[address] = value; }

    /** Write a record straight to flash, as if a previous run had saved it. */
    template <typename T>
    void seed(int address, const T &value)
    {
        memcpy(_flash + address, &value, sizeof(T));
    }

    /** How many times flash was actually written. */
    uint32_t commitCount;

private:
    size_t _size;
    uint8_t _cache[CAPACITY];
    uint8_t _flash[CAPACITY];
};

/**
 * One EEPROM shared by every translation unit, following fakeChip(). A
 * function-local static rather than a header-scope object, which would give
 * everblu_cyble.cpp and the test file a copy each and silently test nothing.
 */
inline FakeEEPROMClass &fakeEeprom()
{
    static FakeEEPROMClass eeprom;
    return eeprom;
}

// The code under test names the core's global object. Routing that name to the
// accessor keeps the definition here, so a test suite that never touches
// persistence does not have to declare one to link.
#define EEPROM fakeEeprom()

#endif // __FAKE_EEPROM_H__
