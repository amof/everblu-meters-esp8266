#include "eeprom_store.h"
#include "EEPROM.h"

// The one place the Arduino EEPROM library is touched. Records move in and out
// as raw bytes; the tagging and typing live in the header's templates.

void EepromStore::begin(size_t size)
{
    EEPROM.begin(size);
}

void EepromStore::readBytes(int address, void *out, size_t len)
{
    uint8_t *bytes = static_cast<uint8_t *>(out);
    for (size_t i = 0; i < len; i++)
        bytes[i] = EEPROM.read(address + (int)i);
}

void EepromStore::writeBytes(int address, const void *in, size_t len)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(in);
    for (size_t i = 0; i < len; i++)
        EEPROM.write(address + (int)i, bytes[i]);
}

void EepromStore::commit()
{
    EEPROM.commit();
}
