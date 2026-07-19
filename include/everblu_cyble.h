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
    METER_READ_OK,     // Data retrieved and decoded
    METER_ASLEEP,      // Outside the meter's wakeup window, nothing transmitted
    METER_NO_RESPONSE, // Meter did not answer on any frequency tried
    METER_BUSY,        // A read or sweep is already running
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
    bool sweepAround(float centerMhz, uint16_t maxSteps, float *foundMhz);
    bool tryFrequency(float freqMhz);

    // Exchange
    bool getDataFromMeter();
    bool decodeBufferReceived(const uint8_t *decoded_buffer, uint8_t size);
    bool isLookLikeRadianFrame(const uint8_t *buffer, uint32_t len);
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
    bool wait_meter_ack();
    bool wait_meter_response();
};

#endif // __EVERBLU_CYBLE_H__
