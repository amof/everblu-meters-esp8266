#ifndef __EEPROM_STORE_H__
#define __EEPROM_STORE_H__

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/**
 * Typed access to the ESP's EEPROM — a RAM-cached image of one flash sector —
 * with the write-behind model kept explicit: load(), save() and clear() all
 * touch the cache, and the cache only reaches flash on the commit() that save()
 * and clear() issue for you. A record written without one survives until reboot
 * but not past it.
 *
 * Records are tagged with a magic number and a schema version. load() returns
 * false for a record this firmware never wrote (an erased sector reads as 0xFF,
 * not zero) or one written under an older layout, so callers never mistake
 * stale bytes for a valid record. Whatever a record means beyond being present
 * and current — a frequency in band, an hour that is really an hour — is the
 * caller's to check.
 *
 * The Arduino EEPROM library is reached only from eeprom_store.cpp, so nothing
 * else in the firmware depends on it. The template bodies here read and write
 * whole records as bytes through that one boundary.
 */
class EepromStore
{
public:
    /** Load the flash sector into the cache. Call once before any access. */
    void begin(size_t size);

    /**
     * @brief Read a tagged record.
     *
     * @return true if the stored bytes carry @p magic and @p schema, i.e. this
     *         firmware wrote them under this layout. On false, @p out holds
     *         whatever was there and must not be trusted.
     */
    template <typename T>
    bool load(int address, T &out, uint32_t magic, uint16_t schema)
    {
        readBytes(address, &out, sizeof(T));
        return out.magic == magic && out.schema == schema;
    }

    /** Stamp a record with its tag and persist it. Writes flash. */
    template <typename T>
    void save(int address, T &record, uint32_t magic, uint16_t schema)
    {
        record.magic = magic;
        record.schema = schema;
        writeBytes(address, &record, sizeof(T));
        commit();
    }

    /**
     * @brief Erase a record so the next load() rejects it. Writes flash.
     *
     * Zeroes @p record too, leaving the in-memory copy as invalid as the stored
     * one — its magic no longer matches anything.
     */
    template <typename T>
    void clear(int address, T &record)
    {
        memset(&record, 0, sizeof(T));
        writeBytes(address, &record, sizeof(T));
        commit();
    }

private:
    void readBytes(int address, void *out, size_t len);
    void writeBytes(int address, const void *in, size_t len);
    void commit();
};

#endif // __EEPROM_STORE_H__
