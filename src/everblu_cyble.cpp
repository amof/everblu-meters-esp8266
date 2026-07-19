#include "EEPROM.h"
#include "everblu_cyble.h"
#include "cc1101_registers.h"
#include "utils.h"
#include "everblu_log.h"

// The meter transmits on a fixed nominal carrier (433.82 MHz, 25kHz channel,
// 5kHz deviation, 2400 baud). We sweep not because meters differ but because
// the CC1101 module's crystal error means the frequency actually radiated is
// not the one programmed; two boards given the same setting can behave
// differently. 2kHz steps either side of nominal is what the reference
// implementation recommends.
const static float FREQ_CENTER = 433.82f;
const static float FREQ_STEP = 0.002f;
const static float FREQ_MIN = 433.76f;
const static float FREQ_MAX = 433.89f;

// Full sweep: +/-30 steps covers +/-60kHz, i.e. ~140ppm of crystal error.
const static uint16_t FREQ_STEPS_FULL = 30;
// Re-centring sweep around a known-good frequency: +/-3 steps covers +/-6kHz,
// enough to track the crystal drifting with temperature between readings.
const static uint16_t FREQ_STEPS_RECENTER = 3;

// Default wakeup window when the meter has not told us its own yet:
// Monday to Saturday, 06:00 to 18:00 local time.
#define DEFAULT_WAKEUP_START 6
#define DEFAULT_WAKEUP_STOP 18
#define WAKEUP_HOUR_UNKNOWN 0xFF

// Bump EEPROM_SCHEMA whenever MeterProfile changes, so stale records are
// rejected instead of being reinterpreted as a different layout.
#define EEPROM_MAGIC 0x45564231UL // "EVB1"
#define EEPROM_SCHEMA 1

// Index in frame for Everblu Cyble
#define INDEX_CURRENT_INDEX 18
#define INDEX_BATTERY_LIFE_TIME 31
#define INDEX_WAKEUP_START 44
#define INDEX_WAKEUP_STOP 45
#define INDEX_NUM_OF_READINGS 48

#define TX_TMO 300         // TX timeout
#define RX_TMO 150         // RX timeout
#define RX_RESP_TMO 700    // RX response timeout
#define WUPBUFFER_SIZE 8   // Wake up buffer size
#define FIFO_THRESHOLD 10  // Fifo Threshold
#define TX_BUFFER_SIZE 39  // TX buffer size
#define EEPROM_SIZE 64         // EEPROM size
#define EEPROM_PROFILE_ADDR 0  // Address where the meter profile is stored
#define EEPROM_SCHEDULE_ADDR 24 // Address where the reading schedule is stored

#define SCHEDULE_MAGIC 0x45564332UL // "EVC2"
#define SCHEDULE_SCHEMA 1
#define DEFAULT_READING_HOUR 12 // Midday: comfortably inside the wakeup window

// Failed automatic reads allowed per day before giving up until tomorrow. Each
// one can cost a full frequency sweep, so this bounds a non-answering meter's
// airtime instead of letting it retry for the rest of the wakeup window. A
// manual read is never subject to it: someone standing at the device asking for
// a reading has information the schedule does not.
#define MAX_READ_ATTEMPTS_PER_DAY 5

// The EEPROM records are raw struct images, so their sizes are part of the
// on-flash format. Fail the build rather than silently misread stored data if a
// toolchain change alters padding or the width of time_t.
static_assert(sizeof(MeterProfile) <= EEPROM_SCHEDULE_ADDR, "MeterProfile overlaps ReaderSchedule");
static_assert(EEPROM_SCHEDULE_ADDR + sizeof(ReaderSchedule) <= EEPROM_SIZE, "ReaderSchedule exceeds EEPROM");

const static uint8_t WAKE_UP_COUNT = 77; // 77 * (8*8) =  4928 bits
#define WATER_METER_ACK_LEN 0x12
#define WATER_METER_RESPONSE_LEN 0x7C

// On the wire each byte carries 8 data bits plus a start and 2 stop bits...
#define RADIAN_FRAME_SIZE(expectedSizeBytes) (((expectedSizeBytes) * (8 + 3) / 8) + 1)
// ...and the payload is captured at 4x the bit rate, so the raw RX buffer must
// be four times the frame size. Dropping this factor overflows the heap.
#define RADIAN_OVERSAMPLING 4
#define RADIAN_RX_BUFFER_SIZE(expectedSizeBytes) (RADIAN_FRAME_SIZE(expectedSizeBytes) * RADIAN_OVERSAMPLING)

EverbluCyble::EverbluCyble(uint8_t gdoPin, uint8_t year, uint32_t serial)
{
    _year = year;
    _serial = serial;
    _provisioned = false;
    _busy = false;
    // Optimistic until init() runs the check. Nothing consults this before
    // then, and defaulting to a failure would make every pre-init log line
    // claim a fault that has not been looked for yet.
    _wiring = WIRING_OK;
    _attemptsToday = 0;
    _attemptsDay = 0;
    _betweenAttempts = NULL;
    memset(&_profile, 0, sizeof(_profile));
    memset(&_schedule, 0, sizeof(_schedule));
    _cc1101 = new CC1101(gdoPin);
}

bool EverbluCyble::init()
{
    _cc1101->init();

    // Before anything else is attempted, and on every boot rather than only on
    // an unprovisioned one: a wire that falls off later must not go unnoticed
    // just because the reader was working the day it was assembled.
    _wiring = _cc1101->checkWiring();

    EEPROM.begin(EEPROM_SIZE);

    loadSchedule();
    LOG("[Everblu] Automatic reading at %02u:%02u local\n", _schedule.hour, _schedule.minute);

    _provisioned = loadProfile();
    if (_provisioned)
        LOG("[Everblu] Known meter at %.4f MHz\n", _profile.frequency);
    else
        LOG("[Everblu] No meter profile stored, a full sweep is needed\n");

    return _provisioned;
}

bool EverbluCyble::loadProfile()
{
    EEPROM.get(EEPROM_PROFILE_ADDR, _profile);

    // An erased flash sector reads as 0xFF, which as a float is NaN. Feeding
    // that to the synthesizer silently programs 0 Hz, so the record must prove
    // it was written by us before any of it is trusted.
    if (_profile.magic != EEPROM_MAGIC || _profile.schema != EEPROM_SCHEMA)
        return false;

    // Second line of defence: a corrupted record must not steer the radio
    // outside the band. NaN fails this comparison too.
    if (!(_profile.frequency >= FREQ_MIN && _profile.frequency <= FREQ_MAX))
    {
        LOG("[Everblu] Stored frequency out of range, ignoring profile\n");
        return false;
    }

    return true;
}

void EverbluCyble::saveProfile(float frequency)
{
    _profile.magic = EEPROM_MAGIC;
    _profile.schema = EEPROM_SCHEMA;
    _profile.frequency = frequency;
    // The meter reports its own wakeup window; prefer it over our default.
    _profile.wakeupStart = wakeupStart;
    _profile.wakeupStop = wakeupStop;

    EEPROM.put(EEPROM_PROFILE_ADDR, _profile);
    EEPROM.commit();
    _provisioned = true;

    LOG("[Everblu] Profile saved: %.4f MHz, awake %uh-%uh\n",
                  _profile.frequency, _profile.wakeupStart, _profile.wakeupStop);
}

void EverbluCyble::loadSchedule()
{
    EEPROM.get(EEPROM_SCHEDULE_ADDR, _schedule);

    // Erased flash or an older layout: fall back to the documented default
    // rather than reading garbage hours out of 0xFF bytes.
    if (_schedule.magic != SCHEDULE_MAGIC ||
        _schedule.schema != SCHEDULE_SCHEMA ||
        _schedule.hour > 23 || _schedule.minute > 59)
    {
        _schedule.magic = SCHEDULE_MAGIC;
        _schedule.schema = SCHEDULE_SCHEMA;
        _schedule.hour = DEFAULT_READING_HOUR;
        _schedule.minute = 0;
        _schedule.lastReadAt = 0;
    }
}

void EverbluCyble::saveSchedule()
{
    _schedule.magic = SCHEDULE_MAGIC;
    _schedule.schema = SCHEDULE_SCHEMA;
    EEPROM.put(EEPROM_SCHEDULE_ADDR, _schedule);
    EEPROM.commit();
}

bool EverbluCyble::setScheduledTime(uint8_t hour, uint8_t minute)
{
    if (hour > 23 || minute > 59)
    {
        LOG("[Everblu] Ignoring invalid reading time %u:%u\n", hour, minute);
        return false;
    }

    _schedule.hour = hour;
    _schedule.minute = minute;
    saveSchedule();
    LOG("[Everblu] Automatic reading moved to %02u:%02u local\n", hour, minute);
    return true;
}

/** Whether two instants fall on the same local calendar day. */
static bool sameLocalDay(time_t a, time_t b)
{
    struct tm aTm = *localtime(&a);
    struct tm bTm = *localtime(&b);

    return (aTm.tm_year == bTm.tm_year) && (aTm.tm_yday == bTm.tm_yday);
}

bool EverbluCyble::alreadyReadToday(time_t now) const
{
    if (_schedule.lastReadAt == 0)
        return false;

    return sameLocalDay(_schedule.lastReadAt, now);
}

uint8_t EverbluCyble::maxAttemptsPerDay() const
{
    return MAX_READ_ATTEMPTS_PER_DAY;
}

void EverbluCyble::rollAttemptBudget(time_t now)
{
    if (_attemptsDay != 0 && sameLocalDay(_attemptsDay, now))
        return;

    _attemptsDay = now;
    _attemptsToday = 0;
}

MeterReadResult EverbluCyble::readIfDue(time_t now, bool *performed)
{
    *performed = false;

    if (alreadyReadToday(now))
        return METER_READ_OK;

    struct tm nowTm = *localtime(&now);
    uint16_t nowMinutes = (nowTm.tm_hour * 60) + nowTm.tm_min;
    uint16_t dueMinutes = (_schedule.hour * 60) + _schedule.minute;

    // Compared as "the time has passed and today is unread" rather than as an
    // exact match, so a device that was offline or busy at the scheduled minute
    // still reads later the same day instead of skipping it entirely.
    if (nowMinutes < dueMinutes)
        return METER_READ_OK;

    // A tick outside the wakeup window is not a failed attempt, it is a tick on
    // which nothing was due. Letting it through would mark every minute of a
    // Sunday, or of an evening, as an attempt: readMeter() would refuse anyway,
    // but the caller would log and republish the refusal once a minute for the
    // rest of the day. The read stays pending and fires when the window opens.
    if (!isMeterAwake(now))
        return METER_READ_OK;

    rollAttemptBudget(now);
    if (_attemptsToday >= MAX_READ_ATTEMPTS_PER_DAY)
        return METER_READ_OK;

    *performed = true;
    MeterReadResult result = readMeter(now);

    // METER_BUSY means the radio was already in use — a manual read or sweep
    // running over the tick. Nothing was transmitted on this meter's behalf, so
    // charging it to the budget would let an unrelated command exhaust the day.
    if (result != METER_BUSY)
    {
        _attemptsToday++;
        if (_attemptsToday >= MAX_READ_ATTEMPTS_PER_DAY)
            LOG("[Everblu] %u failed attempts today, giving up until tomorrow\n", _attemptsToday);
    }

    return result;
}

void EverbluCyble::forgetMeter()
{
    memset(&_profile, 0, sizeof(_profile));
    EEPROM.put(EEPROM_PROFILE_ADDR, _profile);
    EEPROM.commit();
    _provisioned = false;
    LOG("[Everblu] Meter profile cleared\n");
}

void EverbluCyble::radioIdle()
{
    // Deliberately ignores _busy. This is called from the OTA start callback,
    // which fires while a sweep may be mid-flight; refusing because the radio
    // is busy would leave it transmitting across the reboot, which is the exact
    // situation this exists to prevent.
    _cc1101->idleAndFlush();
    LOG("[Everblu] Radio parked\n");
}

bool EverbluCyble::isMeterAwake(time_t now) const
{
    struct tm *local = localtime(&now);
    if (local == NULL)
        return false;

    // The meter is deaf on Sundays.
    if (local->tm_wday == 0)
        return false;

    uint8_t start = DEFAULT_WAKEUP_START;
    uint8_t stop = DEFAULT_WAKEUP_STOP;
    // Only believe the meter's own window if it looks like a pair of hours.
    // These come from fixed byte offsets in the response and the interpretation
    // is inherited, not verified, so anything implausible falls back to the
    // documented default rather than blocking reads forever.
    if (_provisioned &&
        _profile.wakeupStart < 24 && _profile.wakeupStop < 24 &&
        _profile.wakeupStart < _profile.wakeupStop)
    {
        start = _profile.wakeupStart;
        stop = _profile.wakeupStop;
    }

    return (local->tm_hour >= start) && (local->tm_hour < stop);
}

WiringCheckResult EverbluCyble::checkWiring()
{
    _wiring = _cc1101->checkWiring();
    return _wiring;
}

void EverbluCyble::warnIfWiringFailed() const
{
    if (_wiring == WIRING_OK)
        return;

    LOG("[Everblu] Wiring check is failing (%s), a no_response below is probably the reader, not the meter\n",
        _wiring == WIRING_SPI_FAILED ? "spi" : "gdo0");
}

MeterReadResult EverbluCyble::readMeter(time_t now)
{
    if (_busy)
    {
        LOG("[Everblu] Radio already busy, ignoring request\n");
        return METER_BUSY;
    }

    warnIfWiringFailed();

    _busy = true;
    MeterReadResult result = readMeterInternal(now);
    _busy = false;

    return result;
}

MeterReadResult EverbluCyble::sweepForMeter(time_t now)
{
    if (_busy)
    {
        LOG("[Everblu] Radio already busy, ignoring request\n");
        return METER_BUSY;
    }

    warnIfWiringFailed();

    _busy = true;
    MeterReadResult result = sweepForMeterInternal(now);
    _busy = false;

    return result;
}

MeterReadResult EverbluCyble::readMeterInternal(time_t now)
{
    float found = 0.0f;

    // Transmitting while the meter is deaf is pure airtime: a full sweep is
    // ~10 minutes of continuous carrier that cannot possibly succeed.
    if (!isMeterAwake(now))
    {
        LOG("[Everblu] Outside the meter's wakeup window, skipping\n");
        return METER_ASLEEP;
    }

    if (_provisioned)
    {
        // The stored frequency is a hint, not a guarantee: the CC1101 crystal
        // error that the sweep compensates for moves with temperature.
        LOG("[Everblu] Reading at known frequency %.4f MHz\n", _profile.frequency);
        if (tryFrequency(_profile.frequency))
        {
            recordSuccessfulRead(now);
            return METER_READ_OK;
        }

        LOG("[Everblu] No answer, re-centring around the known frequency\n");
        if (sweepAround(_profile.frequency, FREQ_STEPS_RECENTER, &found))
        {
            saveProfile(found); // Track the drift
            recordSuccessfulRead(now);
            return METER_READ_OK;
        }
    }

    LOG("[Everblu] Full sweep\n");
    if (sweepAround(FREQ_CENTER, FREQ_STEPS_FULL, &found))
    {
        saveProfile(found);
        recordSuccessfulRead(now);
        return METER_READ_OK;
    }

    return METER_NO_RESPONSE;
}

MeterReadResult EverbluCyble::sweepForMeterInternal(time_t now)
{
    float found = 0.0f;

    if (!isMeterAwake(now))
    {
        LOG("[Everblu] Outside the meter's wakeup window, not sweeping\n");
        return METER_ASLEEP;
    }

    // Deliberately ignores any stored frequency: this is what the user reaches
    // for when the stored one is believed wrong.
    LOG("[Everblu] Full sweep requested\n");
    if (sweepAround(FREQ_CENTER, FREQ_STEPS_FULL, &found))
    {
        saveProfile(found);
        recordSuccessfulRead(now);
        return METER_READ_OK;
    }

    return METER_NO_RESPONSE;
}

void EverbluCyble::recordSuccessfulRead(time_t now)
{
    _schedule.lastReadAt = now;
    saveSchedule();
}

bool EverbluCyble::sweepAround(float centerMhz, uint16_t maxSteps, float *foundMhz)
{
    // Walk outwards from the centre so the common case (little crystal error)
    // terminates after a handful of attempts instead of sweeping the whole band.
    for (uint16_t step = 0; step <= maxSteps; step++)
    {
        for (int8_t direction = 1; direction >= -1; direction -= 2)
        {
            if (step == 0 && direction < 0)
                continue; // The centre is only worth trying once

            // Computed from the step index rather than accumulated, so a long
            // sweep cannot drift through repeated float rounding.
            float freq = centerMhz + (direction * (float)step * FREQ_STEP);
            if (freq < FREQ_MIN || freq > FREQ_MAX)
                continue;

            if (tryFrequency(freq))
            {
                *foundMhz = freq;
                return true;
            }

            // A full sweep runs for minutes, far longer than an MQTT keepalive.
            // Let the caller service its network stack between attempts.
            if (_betweenAttempts != NULL)
                _betweenAttempts();
        }
    }

    return false;
}

bool EverbluCyble::tryFrequency(float freqMhz)
{
    LOG("[Everblu] --> Testing frequency: %.4f MHz\n", freqMhz);
    _cc1101->setFrequency(freqMhz);
    return getDataFromMeter();
}

bool EverbluCyble::decodeBufferReceived(const uint8_t *decoded_buffer, uint8_t size)
{
    // Indices are offsets, so the frame must be strictly longer than the last
    // one read. INDEX_NUM_OF_READINGS is 48, hence 49 bytes minimum.
    if (size <= INDEX_NUM_OF_READINGS)
        return false;

    currentIndex = (uint32_t)decoded_buffer[INDEX_CURRENT_INDEX] |
                    ((uint32_t)decoded_buffer[INDEX_CURRENT_INDEX + 1] << 8) |
                    ((uint32_t)decoded_buffer[INDEX_CURRENT_INDEX + 2] << 16) |
                    ((uint32_t)decoded_buffer[INDEX_CURRENT_INDEX + 3] << 24);
    numReadings = decoded_buffer[INDEX_NUM_OF_READINGS];
    batteryLifetime = decoded_buffer[INDEX_BATTERY_LIFE_TIME];
    wakeupStart = decoded_buffer[INDEX_WAKEUP_START];
    wakeupStop = decoded_buffer[INDEX_WAKEUP_STOP];

    return true;
}

bool EverbluCyble::getDataFromMeter()
{
    LOG("[Everblu] Retrieving data from meter\n");

    // Reset internal variables
    resetData();

    // Put CC1101 into IDLE and flush TX/RX FIFO
    _cc1101->idleAndFlush();

    // Request data from meter
    LOG("[Everblu] Wake-up meter and request data\n");
    if (askWaterMeter() == false)
        return false;

    // Wait for meter ack
    LOG("[Everblu] Wait for meter ack\n");
    if (wait_meter_ack() == false)
    {
        LOG("[Everblu] No response from meter\n");
        return false;
    }

    // Wait for meter response
    LOG("[Everblu] Wait for meter response\n");
    return wait_meter_response();
}

bool EverbluCyble::askWaterMeter()
{
    bool success = transmitWakeupAndRequest();

    // Always return to IDLE, flush, and restore the default modem configuration,
    // whichever step failed. idleAndFlush() also clears a TXFIFO_UNDERFLOW, the
    // expected terminal state of an infinite-length transmission.
    _cc1101->idleAndFlush();
    _cc1101->halRfWriteReg(MDMCFG2, 0x02);  // Restore modem configuration
    _cc1101->halRfWriteReg(PKTCTRL0, 0x00); // Fix packet length

    return success;
}

bool EverbluCyble::transmitWakeupAndRequest()
{
    uint8_t txBuffer[TX_BUFFER_SIZE];
    uint8_t firstChunk[WUPBUFFER_SIZE] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};

    // Build the request up front: once TX starts the FIFO must be kept fed, and
    // there is no time budget left for a CRC computation.
    createRadianMasterRequest(txBuffer);

    // Configure RF
    _cc1101->halRfWriteReg(MDMCFG2, 0x00);  // clear MDMCFG2 to do not send preamble and sync
    _cc1101->halRfWriteReg(PKTCTRL0, 0x02); // infinite packet len

    // The FIFO must hold data BEFORE strobing STX. With MDMCFG2 = 0x00 no
    // preamble or sync word is generated, so an empty FIFO underflows
    // immediately, and datasheet 15.4 states that writing to the TX FIFO after
    // it has underflowed will not restart TX mode.
    _cc1101->writeBurstReg(TX_FIFO_ADDR, firstChunk, WUPBUFFER_SIZE);

    // Go into TX. MCSM0.FS_AUTOCAL = 01 so this calibrates first;
    // Table 34 gives IDLE->TX with calibration as 799us.
    if (_cc1101->strobeAndWait(STX, _cc1101->CHIP_SS_TX) == false)
        return false;

    // Await meter to wake up. One chunk is already in flight.
    if (longWakeupPreamble(WAKE_UP_COUNT - 1) == false)
        return false;

    // Send the Radian master request back to back with the preamble. Wait for
    // room first: the backpressure above deliberately keeps the FIFO nearly
    // full, and a TX FIFO overflow corrupts the FIFO content (datasheet 10.1).
    if (_cc1101->waitTxFifoFree(TX_BUFFER_SIZE, TX_TMO) == false)
        return false;
    _cc1101->writeBurstReg(TX_FIFO_ADDR, txBuffer, TX_BUFFER_SIZE);

    // Let the request drain before leaving TX, otherwise the tail of the frame
    // is flushed away instead of transmitted.
    return _cc1101->waitTxFifoDrained(TX_TMO);
}

bool EverbluCyble::wait_meter_ack()
{
    // The buffer holds the RAW oversampled bytes, not the decoded frame:
    // 4 samples per bit on the wire (see RADIAN_RX_BUFFER_SIZE).
    uint32_t rxBufferSize = RADIAN_RX_BUFFER_SIZE(WATER_METER_ACK_LEN);
    uint8_t *rxBuffer = (uint8_t *)malloc(rxBufferSize);
    if (rxBuffer == NULL)
    {
        LOG("[Everblu] Unable to allocate %u bytes for ack\n", rxBufferSize);
        return false;
    }

    bool isAck = (receiveData(RX_TMO, WATER_METER_ACK_LEN, rxBuffer, rxBufferSize) != 0);

    free(rxBuffer);

    return isAck;
}

bool EverbluCyble::wait_meter_response()
{
    uint8_t *meterData = NULL;
    uint8_t meterDataSize = 0;
    uint32_t bytesReceived = 0;
    bool success = false;

    uint32_t rxBufferSize = RADIAN_RX_BUFFER_SIZE(WATER_METER_RESPONSE_LEN);
    uint8_t *rxBuffer = (uint8_t *)malloc(rxBufferSize);
    if (rxBuffer == NULL)
    {
        LOG("[Everblu] Unable to allocate %u bytes for response\n", rxBufferSize);
        return false;
    }

    bytesReceived = receiveData(RX_RESP_TMO, WATER_METER_RESPONSE_LEN, rxBuffer, rxBufferSize);
    if (bytesReceived == 0)
    {
        free(rxBuffer);
        return false;
    }

    // Decoding strips start/stop bits and collapses the oversampling, so the
    // result is always shorter than the input.
    LOG("[Everblu] Decoding 4bitp\n");
    meterData = (uint8_t *)malloc(bytesReceived);
    if (meterData == NULL)
    {
        LOG("[Everblu] Unable to allocate %u bytes for decoding\n", bytesReceived);
        free(rxBuffer);
        return false;
    }
    memset(meterData, 0, bytesReceived);
    meterDataSize = decode_4bitpbit_serial(rxBuffer, bytesReceived, meterData);

    LOG("[Everblu] Decoding data received\n");
    success = decodeBufferReceived(meterData, meterDataSize);

    if (success)
    {
        LOG("[Everblu] Data received:\n");
        LOG("Current meter index (litres): %u\n", currentIndex);
        LOG("Number of meter readings: %u\n", numReadings);
        LOG("Battery life remaining (months): %u\n", batteryLifetime);
        LOG("Meter wakeup time: %u\n", wakeupStart);
        LOG("Meter sleep time: %u\n", wakeupStop);
    }
    else
    {
        LOG("[Everblu] Frame too short to decode: %u bytes\n", meterDataSize);
    }

    free(rxBuffer);
    free(meterData);

    return success;
}

uint32_t EverbluCyble::receiveData(uint32_t timeoutMs, uint32_t expectedFrameBytes, uint8_t *rxBuffer, uint32_t rxBufferSize)
{
    uint32_t totalBytesReceived = 0;
    uint32_t oversampledSize = RADIAN_RX_BUFFER_SIZE(expectedFrameBytes);

    // Callers pass the decoded frame size; the oversampling factor is applied
    // here so the unit can never be converted twice, or not at all.
    if (oversampledSize > rxBufferSize)
    {
        LOG("[Everblu] Buffer too small: %u bytes needed, %u given\n", oversampledSize, rxBufferSize);
        return 0;
    }

    // Stage 1: lock on the start of the sync pattern at 2.4kbps
    initializeForSyncPatternReception();
    if (_cc1101->strobeAndWait(SRX, _cc1101->CHIP_SS_RX) &&
        _cc1101->waitForGdo0Assert(timeoutMs) &&
        _cc1101->readFifoData(timeoutMs, 1, rxBuffer) != 0)
    {
        // Stage 2: switch to 9.59kbps to capture the payload 4x oversampled
        initializeForDataReception();
        if (_cc1101->strobeAndWait(SRX, _cc1101->CHIP_SS_RX) &&
            _cc1101->waitForGdo0Assert(timeoutMs))
        {
            totalBytesReceived = _cc1101->readFifoData(timeoutMs, oversampledSize, rxBuffer);
            _cc1101->getRxStats();
        }
    }

    // Restore on every path: a half-configured modem (9.59kbps, 0xFFF0 sync,
    // infinite length) would otherwise poison the next frequency in the sweep.
    restoreDefaultSettings();

    return totalBytesReceived;
}

void EverbluCyble::resetData()
{
    currentIndex = 0;
    numReadings = 0;
    batteryLifetime = 0;
    wakeupStart = 0;
    wakeupStop = 0;
}

bool EverbluCyble::longWakeupPreamble(uint8_t chunksToSend)
{
    uint8_t wakeUpBuffer[WUPBUFFER_SIZE] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55};

    // Preamble is a series of 0101….0101 at 2400 bits/sec.
    // Long preamble for meter wake-up: 4928 bits (2464 x 01)
    //
    // The FIFO drains at 8*8/2400 = 26.6ms per chunk. Refilling on a fixed delay
    // of the same order lets it run dry, which is fatal: datasheet 15.4 makes
    // TXFIFO_UNDERFLOW unrecoverable without SFTX. Gate on free space instead so
    // the FIFO stays nearly full and the carrier is never interrupted.
    while (chunksToSend > 0)
    {
        if (_cc1101->waitTxFifoFree(WUPBUFFER_SIZE, TX_TMO) == false)
            return false;

        _cc1101->writeBurstReg(TX_FIFO_ADDR, wakeUpBuffer, WUPBUFFER_SIZE);
        chunksToSend--;
    }

    return true;
}

void EverbluCyble::createRadianMasterRequest(uint8_t *outputBuffer)
{
    uint16_t crc = 0;
    uint8_t to_encode[] = {0x13, 0x10, 0x00, 0x45, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x45, 0x20, 0x0A, 0x50, 0x14, 0x00, 0x0A, 0x40, 0xFF, 0xFF}; // les 2 derniers octet sont en reserve pour le CKS ainsi que le serial number
    uint8_t synch_pattern[] = {0x50, 0x00, 0x00, 0x00, 0x03, 0xFF, 0xFF, 0xFF, 0xFF};

    to_encode[4] = _year;
    to_encode[5] = (uint8_t)((_serial & 0x00FF0000) >> 16);
    to_encode[6] = (uint8_t)((_serial & 0x0000FF00) >> 8);
    to_encode[7] = (uint8_t)(_serial & 0x000000FF);
    // LOG("[Everblu] calculating CRC\n");
    crc = crc_kermit(to_encode, sizeof(to_encode) - 2);
    to_encode[sizeof(to_encode) - 2] = (uint8_t)((crc & 0xFF00) >> 8);
    to_encode[sizeof(to_encode) - 1] = (uint8_t)(crc & 0x00FF);
    memcpy(outputBuffer, synch_pattern, sizeof(synch_pattern));
    // LOG("[Everblu] Encoding\n");
    encode2serial_1_3(to_encode, sizeof(to_encode), &outputBuffer[sizeof(synch_pattern)]);
}

bool EverbluCyble::isLookLikeRadianFrame(const uint8_t *buffer, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i)
    {
        if (buffer[i] == 0xFF)
        {
            return true;
        }
    }
    return false;
}

void EverbluCyble::initializeForSyncPatternReception()
{
    _cc1101->idleAndFlush();               // SFRX is only legal from IDLE (Table 42)
    _cc1101->halRfWriteReg(MCSM1, 0x0F);   // CCA always; default mode RX
    _cc1101->halRfWriteReg(MDMCFG2, 0x02); // 2-FSK; no Manchester; 16/16 sync word bits detected
    _cc1101->halRfWriteReg(SYNC1, 0x55);   // Set sync pattern start (01010101)
    _cc1101->halRfWriteReg(SYNC0, 0x50);   // Set sync pattern start (01010000)
    _cc1101->halRfWriteReg(MDMCFG4, 0xF6); // RX filter BW = 58kHz
    _cc1101->halRfWriteReg(MDMCFG3, 0x83); // 2.4kbps data rate
    _cc1101->halRfWriteReg(PKTLEN, 1);     // Set packet length to sync pattern size
}

void EverbluCyble::initializeForDataReception()
{
    _cc1101->idleAndFlush();                // SFRX is only legal from IDLE (Table 42)
    _cc1101->halRfWriteReg(SYNC1, 0xFF);    // Set sync pattern end (11111111)
    _cc1101->halRfWriteReg(SYNC0, 0xF0);    // Set sync pattern end (11110000)
    _cc1101->halRfWriteReg(MDMCFG4, 0xF8);  // RX filter BW = 58kHz
    _cc1101->halRfWriteReg(MDMCFG3, 0x83);  // 9.59kbps data rate
    _cc1101->halRfWriteReg(PKTCTRL0, 0x02); // Infinite packet length
}

void EverbluCyble::restoreDefaultSettings()
{
    _cc1101->idleAndFlush();                // Idle, then flush both FIFOs
    _cc1101->halRfWriteReg(MDMCFG4, 0xF6);  // Restore RX filter BW
    _cc1101->halRfWriteReg(MDMCFG3, 0x83);  // Restore data rate
    _cc1101->halRfWriteReg(PKTCTRL0, 0x00); // Fixed packet length
    _cc1101->halRfWriteReg(PKTLEN, 38);     // Packet length
    _cc1101->halRfWriteReg(SYNC1, 0x55);    // Restore sync pattern start
    _cc1101->halRfWriteReg(SYNC0, 0x00);    // Restore sync pattern end
}
