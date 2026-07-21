#ifndef __EVERBLU_CYBLE_H__
#define __EVERBLU_CYBLE_H__

#include <Arduino.h>
#include <time.h>
#include <cc1101.h>

/**
 * Outcome of an attempt to read the meter.
 */
enum MeterReadResult
{
    METER_READ_OK,      // Data retrieved and decoded
    METER_ASLEEP,       // Outside the meter's wakeup window, nothing transmitted
    METER_NO_RESPONSE,  // Meter did not answer on any frequency tried
    METER_BUSY,         // A read or sweep is already running
    METER_UNREADABLE,   // Meter answered, but its response could not be read
};

/**
 * What one exchange on one frequency achieved.
 *
 * The middle value is the one that earns this enum its place. A meter ack is
 * proof the frequency is right: the meter heard the master request and replied
 * on it. So a failure that happens *after* an ack is never a reason to try
 * another frequency, and the sweep must be able to tell the two apart.
 */
enum ExchangeOutcome
{
    EXCHANGE_NO_ANSWER, // Silence; this frequency may well be the wrong one
    EXCHANGE_ACK_ONLY,  // Meter acknowledged, but its response did not decode
    EXCHANGE_COMPLETE,  // A full reading
};

/**
 * What we have learned about one specific meter, persisted in EEPROM so the
 * frequency sweep only has to happen once.
 */
struct MeterProfile
{
    uint32_t magic;      // Identifies a written record; guards against erased flash
    uint16_t schema;     // Bumped when this layout changes, invalidating old records
    float frequency;     // MHz at which this meter answered
    uint8_t wakeupStart; // Hour the meter starts answering, 0xFF if unknown
    uint8_t wakeupStop;  // Hour the meter stops answering, 0xFF if unknown
};

/**
 * When the meter is interrogated automatically, and when that last happened.
 * Persisted so a reboot neither forgets the setting nor causes a second read
 * on a day that has already been read.
 */
struct ReaderSchedule
{
    uint32_t magic;
    uint16_t schema;
    uint8_t hour;       // Local hour of the daily reading
    uint8_t minute;     // Local minute of the daily reading
    time_t lastReadAt;  // When the last successful read completed
};

class EverbluCyble
{
public:
    EverbluCyble(uint8_t gdoPin, uint8_t year, uint32_t serial);
    /**
     * @brief Configure the radio and load any stored meter profile.
     *
     * @return true if a valid profile was loaded (the device is provisioned)
     */
    bool init();
    /**
     * @brief Read the meter, sweeping for its frequency only if necessary.
     *
     * Tries the cached frequency first, then re-centres narrowly around it
     * (crystal error drifts with temperature), then falls back to a full sweep.
     * A successful read refreshes the stored profile.
     *
     * @param now Current time, used to skip transmitting while the meter is deaf
     */
    MeterReadResult readMeter(time_t now);
    /**
     * @brief Forget the stored frequency and sweep the whole band for it.
     *
     * This is the "first use" path, and the recovery path if the meter is
     * replaced. Costs minutes of transmission, so it is never automatic.
     */
    MeterReadResult sweepForMeter(time_t now);

    /**
     * @brief One exchange at one frequency, and nothing more.
     *
     * Never sweeps and never falls back. Exists so a diagnostic costs the meter
     * a single wake-up preamble rather than the 61 a failed sweep spends —
     * repeated sweeping appears to be enough to stop a meter answering at all.
     * Saves the profile if a full reading results.
     */
    MeterReadResult testFrequency(time_t now, float freqMhz);
    /**
     * @brief Read if the scheduled time has passed and today has not been read.
     *
     * Call this periodically. It is date-based rather than a countdown, so a
     * reboot or a lost connection cannot silently skip or repeat a day.
     *
     * Gives up for the day after a fixed number of failed attempts, so a meter
     * that is not answering costs a bounded amount of transmission rather than
     * a sweep every tick until the window closes.
     *
     * @param now Current time
     * @param performed Set to true if a read was actually attempted
     */
    MeterReadResult readIfDue(time_t now, bool *performed);
    /** Failed attempts made on the current local day. */
    uint8_t attemptsToday() const { return _attemptsToday; }
    /** Attempts allowed per day before the reader waits for tomorrow. */
    uint8_t maxAttemptsPerDay() const;
    /** When the last successful read completed, 0 if there has never been one. */
    time_t lastReadAt() const { return _schedule.lastReadAt; }
    /**
     * @brief Whether the meter is expected to answer at the given time.
     *
     * Uses the window the meter reported about itself once known, otherwise the
     * documented default of Monday to Saturday, 06:00 to 18:00 local time.
     */
    bool isMeterAwake(time_t now) const;
    /**
     * @brief Discard the stored profile, forcing a full sweep on the next read.
     */
    void forgetMeter();
    /**
     * @brief Park the radio: force IDLE and flush both FIFOs.
     *
     * Call before anything that reboots the ESP. The CC1101 has no reset line
     * wired, so it is not reset by the ESP restarting — it keeps transmitting
     * whatever it was transmitting until init() reaches SRES, which for an OTA
     * update is the flash write plus the reboot plus the eboot copy. That would
     * be tens of seconds of unmodulated carrier at 433 MHz from an unattended
     * device.
     */
    void radioIdle();

    /** Local time at which the meter is interrogated automatically. */
    uint8_t scheduledHour() const { return _schedule.hour; }
    uint8_t scheduledMinute() const { return _schedule.minute; }
    /** Set and persist the automatic interrogation time. Ignores out-of-range. */
    bool setScheduledTime(uint8_t hour, uint8_t minute);
    /** Whether a valid meter profile is stored. */
    bool isProvisioned() const { return _provisioned; }

    /**
     * Verdict of the wiring check run at init(), and the chip identity behind
     * it. A third axis alongside availability and status: a reader that fails
     * this is still available, still reports whatever its last interrogation
     * concluded, and is still allowed to try again.
     */
    WiringCheckResult wiringResult() const { return _wiring; }
    ChipIdentity chipIdentity() const { return _cc1101->identity(); }
    /** Re-run the wiring check, e.g. after re-seating a wire. */
    WiringCheckResult checkWiring();

    /**
     * @brief Called between frequency attempts during a sweep.
     *
     * A sweep blocks for minutes, which is long enough for the MQTT keepalive
     * to expire. This hook lets the caller service its network stack without
     * this class having to know anything about MQTT.
     */
    void setBetweenAttemptsCallback(void (*callback)(void)) { _betweenAttempts = callback; }

    // Decoded from the most recent meter response. Zeroed before every
    // interrogation, so these describe the last exchange, not the last success.
    uint32_t currentIndex;   // Cumulative consumption in litres
    uint8_t numReadings;     // Times the meter reports having been read
    uint8_t batteryLifetime; // Remaining battery life in months
    // The window as the meter just reported it. MeterProfile carries fields of
    // the same name holding the window as last persisted; these are the fresh
    // reading, and saveProfile() is what copies one into the other.
    uint8_t wakeupStart;     // Hour the meter starts answering
    uint8_t wakeupStop;      // Hour the meter stops answering

    // Thirteen monthly indexes, oldest first: [0] is M-13, [12] is M-1. Each is
    // a cumulative index like currentIndex, not a monthly consumption, so the
    // series only ever rises. They live at the very end of the response, which
    // is why an undersized capture destroyed them long before it touched
    // anything else.
    static const uint8_t MONTHLY_HISTORY_COUNT = 13;
    uint32_t monthlyIndex[MONTHLY_HISTORY_COUNT];
    // How many of the above the response actually carried. Short of the full
    // count means the frame was truncated, not that the meter has no history.
    uint8_t monthlyCount;

    // When the meter believes it answered, by its own clock — which runs free
    // and has been seen minutes adrift, so it dates the reading rather than
    // timing it.
    uint8_t readDay, readMonth, readYear; // Year is two digits, 26 for 2026
    uint8_t readWeekday;
    uint8_t readHour, readMinute, readSecond;

    // The serial as the meter spells it, distinct from the year and serial that
    // form its radio address. Stored on the wire reversed, and NUL-terminated
    // here after being put back in reading order.
    static const uint8_t METER_SERIAL_MAX = 11;
    char meterSerial[METER_SERIAL_MAX + 1];

private:
    uint8_t _year;
    uint32_t _serial;
    MeterProfile _profile;
    ReaderSchedule _schedule;
    bool _provisioned;
    WiringCheckResult _wiring;
    // Attempts are budgeted per day, and the budget lives in RAM rather than in
    // the schedule record: persisting it would mean an EEPROM write per failed
    // read, and a reboot is a legitimate reason to try again.
    uint8_t _attemptsToday;
    time_t _attemptsDay;
    // Guards the radio against re-entry. The between-attempts callback services
    // MQTT, which can dispatch a command that calls straight back in here.
    bool _busy;
    CC1101 *_cc1101;
    void (*_betweenAttempts)(void);

    /**
     * @brief Log a warning if the wiring check is failing, at the top of every
     *        interrogation.
     *
     * Nothing is prevented — see ADR-0005. This exists so the "no_response"
     * that almost certainly follows is attributed to the reader's own wiring
     * rather than to the meter, at the moment someone is looking.
     */
    void warnIfWiringFailed() const;

    MeterReadResult readMeterInternal(time_t now);
    MeterReadResult sweepForMeterInternal(time_t now);

    // Persistence
    bool loadProfile();
    void saveProfile(float frequency);
    void loadSchedule();
    void saveSchedule();
    void recordSuccessfulRead(time_t now);
    /**
     * @brief Turn the outcome of a search into a read result.
     *
     * A full reading persists the working frequency and the read; an ack-only
     * outcome reports the meter as unreadable (it answered, so the frequency is
     * right); silence reports no response. @p foundMhz is the frequency that
     * worked, saved only on a complete reading.
     */
    MeterReadResult completeRead(ExchangeOutcome outcome, float foundMhz, time_t now);
    /** Whether a successful read has already happened on the given local day. */
    bool alreadyReadToday(time_t now) const;
    /**
     * @brief Roll the attempt budget over if the local day has changed.
     *
     * Called on the way into a scheduling decision rather than from a timer, so
     * the budget resets correctly however long the device was asleep, offline
     * or unpowered across the day boundary.
     */
    void rollAttemptBudget(time_t now);

    // Frequency search
    /**
     * @brief Try frequencies outwards from a centre until the meter answers.
     *
     * Offsets are computed from an integer step index rather than accumulated,
     * so a long sweep cannot drift through float rounding error.
     *
     * @param centerMhz Frequency to try first
     * @param maxSteps Number of steps to try either side
     * @param foundMhz Set to the frequency that worked
     * @return true if the meter answered
     */
    ExchangeOutcome sweepAround(float centerMhz, uint16_t maxSteps, float *foundMhz);
    ExchangeOutcome tryFrequency(float freqMhz);

    // Exchange
    ExchangeOutcome getDataFromMeter();
    bool decodeBufferReceived(const uint8_t *decoded_buffer, uint8_t size);

    /** @brief One 4-byte little-endian cumulative index at a frame offset. */
    static uint32_t readIndexAt(const uint8_t *buffer, uint8_t offset);

    /** @brief Put the meter's own serial back into reading order. */
    void decodeMeterSerial(const uint8_t *buffer, uint8_t size);
    void initializeForSyncPatternReception();
    void initializeForDataReception();
    void restoreDefaultSettings();
    void createRadianMasterRequest(uint8_t *outputBuffer);
    void resetData();
    bool longWakeupPreamble(uint8_t chunksToSend);
    bool askWaterMeter();
    /**
     * @brief TX body of askWaterMeter(), split out so the caller can always
     *        restore the modem configuration whichever step fails.
     */
    bool transmitWakeupAndRequest();
    /**
     * @brief Receive one RADIAN frame: lock on the sync pattern, then capture
     *        the oversampled payload.
     *
     * @param timeoutMs
     * @param expectedFrameBytes Decoded frame size, e.g. WATER_METER_RESPONSE_LEN.
     *                           The 1-in-4 oversampling factor and the start/stop
     *                           bit overhead are applied internally.
     * @param rxBuffer Destination for the raw oversampled bytes
     * @param rxBufferSize Capacity of rxBuffer, validated before receiving
     * @return Number of bytes written to rxBuffer, 0 on failure
     */
    uint32_t receiveData(uint32_t timeoutMs, uint32_t expectedFrameBytes, uint8_t *rxBuffer, uint32_t rxBufferSize);
    /**
     * @brief Receive the meter ack, and nothing else.
     *
     * Nothing may be decoded, logged or printed here: the meter starts sending
     * its response immediately afterwards, so any work between the two is time
     * stolen from the listener. The raw capture is handed to the caller to
     * examine once the exchange has closed.
     *
     * @param[out] capture Optional: receives ownership of the raw oversampled
     *             buffer, which the caller must free. NULL when nothing arrived.
     * @param[out] captureLen Optional: how many bytes @p capture holds.
     */
    bool wait_meter_ack(uint8_t **capture = NULL, uint32_t *captureLen = NULL);
    bool wait_meter_response();

    /**
     * @brief Does a captured ack name our meter as its sender?
     *
     * Reads only the identity fields in the frame's first 14 bytes, which sit
     * ahead of anything the decoder is known to lose off the tail and need no
     * checksum to be trusted.
     */
    bool ackIdentifiesOurMeter(const uint8_t *raw, uint32_t rawLen);
};

#endif // __EVERBLU_CYBLE_H__
