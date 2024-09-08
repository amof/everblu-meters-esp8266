#include "EEPROM.h"
#include "everblu_cyble.h"
#include "cc1101_registers.h"
#include "utils.h"

// Frequencies to look for meter
const static float FREQ_MIN = 433.76f;
const static float FREQ_MAX =  433.890f;
const static float FREQ_INC = 0.0005f;

// Index in frame for Everblu Cyble
#define INDEX_CURRENT_INDEX     18
#define INDEX_BATTERY_LIFE_TIME 31
#define INDEX_WAKEUP_START      44
#define INDEX_WAKEUP_STOP       45
#define INDEX_NUM_OF_READINGS   48

#define TX_TMO 300          // TX timeout
#define RX_TMO 150          // RX timeout
#define RX_RESP_TMO 700     // RX response timeout
#define WUPBUFFER_SIZE 8    // Wake up buffer size
#define FIFO_THRESHOLD 10   // Fifo Threshold
#define TX_BUFFER_SIZE 39   // TX buffer size
#define EEPROM_SIZE 16      // EEPROM size
#define EEPROM_FREQ_ADDR 0  // Address where the float is stored

#define WATER_METER_ACK_LEN         0x12
#define WATER_METER_RESPONSE_LEN    0x7C
#define RADIAN_FRAME_SIZE(expectedSizeBytes) (((expectedSizeBytes) * (8 + 3) / 8) + 1)

EverbluCyble::EverbluCyble(uint8_t gdoPin, uint8_t year, uint32_t serial)
{
    _year = year;
    _serial = serial;
    _frequency = 0;
    _cc1101 = new CC1101(gdoPin);
}

void EverbluCyble::init()
{
    _cc1101->init();
    EEPROM.begin(EEPROM_SIZE);

    // Retrieve the frequency of counter from memory and test it
    // If nothing found or can't find it, scan again
    float frequency = 0;
    EEPROM.get(EEPROM_FREQ_ADDR, frequency);
    if(frequency < FREQ_MIN || frequency > FREQ_MAX)
        lookForMeter();
    else
        _cc1101->setFrequency(frequency);
}

void EverbluCyble::lookForMeter()
{
    Serial.println("[Everblu] Looking for meter");
    for (float freq = FREQ_MIN; freq <= FREQ_MAX; freq += FREQ_INC) {
        Serial.printf("[Everblu] --> Testing frequency : %f\n", freq);
        
        _cc1101->setFrequency(freq);
        getDataFromMeter();
    
        if (num_of_readings!= 0 || current_index != 0) {
            Serial.printf("==> Found at frequency: %f\n", freq);
            EEPROM.put(EEPROM_FREQ_ADDR, freq);
            EEPROM.commit();
            break;
        }
    }
}

void EverbluCyble::decodeBufferReceived(uint8_t *decoded_buffer, uint8_t size)
{
    if (size < 48)
        return;

    current_index = decoded_buffer[INDEX_CURRENT_INDEX] 
                | (decoded_buffer[INDEX_CURRENT_INDEX+1] << 8) 
                | (decoded_buffer[INDEX_CURRENT_INDEX+2] << 16) 
                | (decoded_buffer[INDEX_CURRENT_INDEX+3] << 24);
    num_of_readings = decoded_buffer[INDEX_NUM_OF_READINGS];
    battery_lifetime = decoded_buffer[INDEX_BATTERY_LIFE_TIME];
    wakeup_start = decoded_buffer[INDEX_WAKEUP_START];
    wakeup_stop = decoded_buffer[INDEX_WAKEUP_STOP];
}

void EverbluCyble::getDataFromMeter()
{
    Serial.println("[Everblu] Retrieving data from meter");

    // Reset internal variables
    resetData();

    // Request data from meter
    Serial.println("[Everblu] Wake-up meter and request data");
    if(askWaterMeter() == false)
        return;

    // Wait for meter ack
    Serial.println("[Everblu] Wait for meter ack");
    if(wait_meter_ack() == false)
    {
        Serial.println("[Everblu] No response from meter");
        return;
    }

    // Wait for meter response
    Serial.println("[Everblu] Wait for meter response");
    if(wait_meter_response() == false)
        return;
}

bool EverbluCyble::askWaterMeter()
{
    uint8_t txBuffer[TX_BUFFER_SIZE];

    // Configure RF
    _cc1101->halRfWriteReg(MDMCFG2, 0x00);              // clear MDMCFG2 to do not send preamble and sync
    _cc1101->halRfWriteReg(PKTCTRL0, 0x02);             // infinite packet len
    
    // Go INTO Tx
    Serial.println("[Everblu] Going into TX mode");
    _cc1101->writeCmd(STX);	                            // sends the data store into transmit buffer over the air
    delay(5);                                          // Give a bit of time for calibration
    // If not in correct state, return
    if(_cc1101->waitForState(_cc1101->CHIP_SS_TX) == false)
        return false;

    // Await meter to wake up
    Serial.println("[Everblu] Sending preamble");
    longWakeupPreamble();

    // Wait 130ms
    delay(130);

    // Send Radian master request
    Serial.println("[Everblu] Write radian master request");
    createRadianMasterRequest(txBuffer);
    _cc1101->writeBurstReg(TX_FIFO_ADDR, txBuffer, TX_BUFFER_SIZE);

    // Flush the Tx FIFO content and restore default registers
    Serial.println("[Everblu] Flushing FIFO & restoring modem configuration");
    _cc1101->writeCmd(SFTX);
    _cc1101->halRfWriteReg(MDMCFG2, 0x02);      // Restore modem configuration
    _cc1101->halRfWriteReg(PKTCTRL0, 0x00);     // Fix packet length

    return true;
}

bool EverbluCyble::wait_meter_ack()
{
    uint8_t *rxBuffer;
    bool isAck = true;

    // Allocate memory
    uint32_t radianFrameSize = RADIAN_FRAME_SIZE(WATER_METER_ACK_LEN);
    rxBuffer = (uint8_t*)malloc(radianFrameSize * sizeof(uint8_t));
    if(rxBuffer == NULL)
    {
        Serial.printf("[Everblu] Unable to allocate memory with %u bytes", radianFrameSize);
        return false;
    }

    if(receiveData(RX_TMO, 0, rxBuffer) == 0)
        isAck = false;

    // Free memory
    free(rxBuffer);

    return isAck;
}

bool EverbluCyble::wait_meter_response()
{
    uint8_t *rxBuffer;
    uint32_t rxBufferSize = 0;
    uint8_t *meterData;
    uint8_t meterDataSize = 0;

    // Allocate memory
    uint32_t radianFrameSize = RADIAN_FRAME_SIZE(WATER_METER_RESPONSE_LEN);
    rxBuffer = (uint8_t*)malloc(radianFrameSize * sizeof(uint8_t));
    if(rxBuffer == NULL)
    {
        Serial.printf("[Everblu] Unable to allocate memory with %u bytes", radianFrameSize);
        return false;
    }

    rxBufferSize = receiveData(RX_RESP_TMO, radianFrameSize, rxBuffer);
    if(rxBufferSize==0)
        return false;
    
    Serial.println("[Everblu] Decoding 4bitp");
    meterData = (uint8_t*)malloc(rxBufferSize * sizeof(uint8_t));
    if(rxBuffer == NULL)
    {
        Serial.printf("[Everblu] Unable to allocate memory with %u bytes", rxBufferSize);
        return false;
    }
    meterDataSize = decode_4bitpbit_serial(rxBuffer, rxBufferSize, meterData);

    Serial.println("[Everblu] Decoding data received");
    decodeBufferReceived(meterData, meterDataSize);

    Serial.println("[Everblu] Data received:");
    Serial.printf("Current meter index (liters): %u\n", current_index);
    Serial.printf("Number of meter readings: %u\n", num_of_readings);
    Serial.printf("Battery life remaining (months): %u\n", battery_lifetime);
    Serial.printf("Meter wakeup time: %u\n", wakeup_start);
    Serial.printf("Meter sleep time: %u\n", wakeup_stop);

    // Free memory
    free(rxBuffer);
    free(meterData);

    return true;
}

uint32_t EverbluCyble::receiveData(uint32_t timeoutMs, uint32_t radianFrameSizeBytes, uint8_t *rxBuffer)
{
    Serial.println("[Everblu] Configure for sync pattern");
    initializeForSyncPatternReception();

    Serial.println("[Everblu] Going into RX mode");
    _cc1101->writeCmd(SIDLE);  // sets to idle first. must be in
    _cc1101->writeCmd(SRX);    // writes receive strobe (receive mode)
    // If not in correct state, return
    if(_cc1101->waitForState(_cc1101->CHIP_SS_RX) == false)
        return 0;

    Serial.println("[Everblu] Wait for GDO0 change LOW");
    if (!_cc1101->waitForGdo0Change(LOW,timeoutMs)) 
        return 0;

    if(_cc1101->readFifoData(timeoutMs, 1, rxBuffer)==0)
        return 0;
    Serial.println("[Everblu] First sync received");

    _cc1101->getRxStats();

    Serial.println("[Everblu] Configure for data reception");
    initializeForDataReception();

    Serial.println("[Everblu] Going into RX mode");
    _cc1101->writeCmd(SIDLE);  // sets to idle first. must be in
    _cc1101->writeCmd(SRX);    // writes receive strobe (receive mode)
    // If not in correct state, return
    if(_cc1101->waitForState(_cc1101->CHIP_SS_RX) == false)
        return 0;

    Serial.println("[Everblu] Wait for GDO0 change HIGH");
    if (!_cc1101->waitForGdo0Change(HIGH,timeoutMs)) 
        return 0;

    uint32_t totalBytesReceived = _cc1101->readFifoData(timeoutMs, radianFrameSizeBytes * 4, rxBuffer);
    if (totalBytesReceived == 0) 
        return 0;

    Serial.printf("[Everblu] Data received (%u bytes)", totalBytesReceived);

    restoreDefaultSettings();

    return totalBytesReceived;
}

void EverbluCyble::resetData()
{
    current_index = 0;   
    num_of_readings = 0; 
    battery_lifetime = 0;
    wakeup_start = 0;
    wakeup_stop = 0;
}

void EverbluCyble::longWakeupPreamble()
{
    uint8_t wakeUpCount = 77; // 77 * (8*8) =  4928 bits
    uint16_t timeout = 0;
    uint8_t wakeUpBuffer[WUPBUFFER_SIZE] = { 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };

    // Preamble is a series of 0101….0101 at 2400 bits/sec.
    // Long preamble for meter wake-up: 4928 bits (2464 x 01)
    while((wakeUpCount > 0) && ((timeout < TX_TMO)))
    {
        _cc1101->writeBurstReg(TX_FIFO_ADDR, wakeUpBuffer, 8); 
        wakeUpCount--;
        delay(20); // Wait before sending more data
        timeout += 2;
    }
}

void EverbluCyble::createRadianMasterRequest(uint8_t *outputBuffer)
{ 
    uint16_t crc = 0;
    uint8_t to_encode[] ={0x13,0x10,0x00,0x45,0xFF,0xFF,0xFF,0xFF,0x00,0x45,0x20,0x0A,0x50,0x14,0x00,0x0A,0x40,0xFF,0xFF}; //les 2 derniers octet sont en reserve pour le CKS ainsi que le serial number
    uint8_t synch_pattern[] ={0x50,0x00,0x00,0x00,0x03,0xFF,0xFF,0xFF,0xFF};
    
    to_encode[4] = _year;
    to_encode[5] = (uint8_t)((_serial&0x00FF0000)>>16);
    to_encode[6] = (uint8_t)((_serial&0x0000FF00)>>8);
    to_encode[7] = (uint8_t) (_serial&0x000000FF);
    Serial.println("[Everblu] calculating CRC");
    crc = crc_kermit(to_encode,sizeof(to_encode)-2);

    to_encode[sizeof(to_encode)-2]=(uint8_t)((crc&0xFF00)>>8);
    to_encode[sizeof(to_encode)-1]=(uint8_t)(crc&0x00FF);

    memcpy(outputBuffer,synch_pattern,sizeof(synch_pattern));
    Serial.println("[Everblu] Encoding");
    encode2serial_1_3(to_encode,sizeof(to_encode),&outputBuffer[sizeof(synch_pattern)]);
}

bool EverbluCyble::isLookLikeRadianFrame(const uint8_t *buffer, uint32_t len) 
{
  for (uint32_t i = 0; i < len; ++i) {
      if (buffer[i] == 0xFF) {
          return true;
      }
  }
  return false;
}

void EverbluCyble::initializeForSyncPatternReception() {
    _cc1101->writeCmd(SFRX);                // Flush RX FIFO
    _cc1101->halRfWriteReg(MCSM1, 0x0F);    // CCA always; default mode RX
    _cc1101->halRfWriteReg(MDMCFG2, 0x02);  // 2-FSK; no Manchester; 16/16 sync word bits detected
    _cc1101->halRfWriteReg(SYNC1, 0x55);    // Set sync pattern start (01010101)
    _cc1101->halRfWriteReg(SYNC0, 0x50);    // Set sync pattern start (01010000)
    _cc1101->halRfWriteReg(MDMCFG4, 0xF6);  // RX filter BW = 58kHz
    _cc1101->halRfWriteReg(MDMCFG3, 0x83);  // 2.4kbps data rate
    _cc1101->halRfWriteReg(PKTLEN, 1);      // Set packet length to sync pattern size
}

void EverbluCyble::initializeForDataReception() {
    _cc1101->halRfWriteReg(SYNC1, 0xFF);    // Set sync pattern end (11111111)
    _cc1101->halRfWriteReg(SYNC0, 0xF0);    // Set sync pattern end (11110000)
    _cc1101->halRfWriteReg(MDMCFG4, 0xF8);  // RX filter BW = 58kHz
    _cc1101->halRfWriteReg(MDMCFG3, 0x83);  // 9.59kbps data rate
    _cc1101->halRfWriteReg(PKTCTRL0, 0x02); // Infinite packet length
    _cc1101->writeCmd(SFRX);                // Flush RX FIFO
}

void EverbluCyble::restoreDefaultSettings() {
    _cc1101->writeCmd(SFRX);
    _cc1101->writeCmd(SIDLE);
    _cc1101->halRfWriteReg(MDMCFG4, 0xF6);  // Restore RX filter BW
    _cc1101->halRfWriteReg(MDMCFG3, 0x83);  // Restore data rate
    _cc1101->halRfWriteReg(PKTCTRL0, 0x00); // Fixed packet length
    _cc1101->halRfWriteReg(PKTLEN, 38);     // Packet length
    _cc1101->halRfWriteReg(SYNC1, 0x55);    // Restore sync pattern start
    _cc1101->halRfWriteReg(SYNC0, 0x00);    // Restore sync pattern end
}
