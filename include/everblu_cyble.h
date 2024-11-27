#ifndef __EVERBLU_CYBLE_H__
#define __EVERBLU_CYBLE_H__

#include <Arduino.h>
#include <cc1101.h>

class EverbluCyble
{
public:
    EverbluCyble(uint8_t gdoPin, uint8_t year, uint32_t serial);
    void init();
    void lookForMeter();
    void getDataFromMeter();
    uint32_t current_index;   // Current meter index in liter
    uint8_t num_of_readings;  // Number of time meter has been read
    uint8_t battery_lifetime; // Remaining battery life in months
    uint8_t wakeup_start;     // Meter wakeup time
    uint8_t wakeup_stop;      // Meter sleep time
private:
    uint8_t _year;
    uint32_t _serial;
    float _frequency;
    CC1101 *_cc1101;
    void decodeBufferReceived(uint8_t *decoded_buffer, uint8_t size);
    bool isLookLikeRadianFrame(const uint8_t *buffer, uint32_t len);
    void initializeForSyncPatternReception();
    void initializeForDataReception();
    void restoreDefaultSettings();
    void createRadianMasterRequest(uint8_t *outputBuffer);
    void resetData();
    void longWakeupPreamble();
    bool askWaterMeter();
    bool wait_meter_ack();
    bool wait_meter_response();
    uint32_t receiveData(uint32_t timeoutMs, uint32_t radianFrameSizeBytes, uint8_t *rxBuffer);
};

#endif // __EVERBLU_CYBLE_H__