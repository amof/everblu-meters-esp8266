#include <arduino.h>

#include <SPI.h>
#include "cc1101.h"
#include "cc1101_registers.h"


uint8_t PA[] = { 0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00, };

#define SPI_SPEED 500000UL
#define STATE_CHG_TMO 25    // State change timeout

CC1101::CC1101(uint8_t gd0_pin)
{
  _gdo_pin = gd0_pin;
}

void CC1101::reset(void) // reset defined in cc1100 datasheet §19.1
{
  writeCmd(SRES);	//GDO0 pin should output a clock signal with a frequency of CLK_XOSC/192.
  //periode 1/7.417us= 134.8254k  * 192 --> 25.886477M
  //10 periode 73.83 = 135.4463k *192 --> 26Mhz
  delay(1); //1ms for getting chip to reset properly

  writeCmd(SFTX);   //flush the TX_fifo content -> a must for interrupt handling
  writeCmd(SFRX);	  //flush the RX_fifo content -> a must for interrupt handling	
  
  delay(1); //1ms
}

void CC1101::init(void)
{
  pinMode(_gdo_pin, INPUT_PULLUP);

  spiConfigure(0, SPI_SPEED);

  version();
}

void CC1101::setFrequency(float freq)
{
  reset();
  configureRF_0(freq);
  //calibrateAndCompensate();
}

void CC1101::printRegistersSettings(void)
{
  uint8_t config_reg_verify[CFG_REGISTER], Patable_verify[8];
  uint8_t i;

  memset(config_reg_verify, 0, CFG_REGISTER);
  memset(Patable_verify, 0, 8);

  readBurstReg(0, config_reg_verify, CFG_REGISTER);  // reads all config registers from cc1100
  readBurstReg(PATABLE_ADDR, Patable_verify, 8);      // reads output power settings from cc1100

  // Print the header for config registers
  Serial.println("Config Register in hex:");
  Serial.println(" 0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F");

  for (i = 0; i < CFG_REGISTER; i++)  // Print each register value
  {
    if (i % 16 == 0 && i != 0) {
      Serial.println();  // Print a new line every 16 values
    }
    Serial.print(config_reg_verify[i], HEX);
    Serial.print(" ");
  }
  Serial.println();  // End the line after printing all register values

  // Print the PaTable header
  Serial.println("PaTable:");

  for (i = 0; i < 8; i++)  // Print each PaTable value
  {
    Serial.print(Patable_verify[i], HEX);
    Serial.print(" ");
  }
  Serial.println();  // End the line after printing all PaTable values
  delay(1);
}

void CC1101::version(void)
{
  // Read the register values
  uint8_t partNumber = halRfReadReg(PARTNUM_ADDR);
  uint8_t version = halRfReadReg(VERSION_ADDR);

  // Print the values
  Serial.print("[CC1101] Partnumber: 0x");
  Serial.println(partNumber, HEX);
  Serial.print("[CC1101] Version: 0x");
  Serial.println(version, HEX); 
  delay(1);
}

bool CC1101::waitForGdo0Change(uint8_t voltageLevel, int timeoutMs) {
    int elapsedTime = 0;
    while ((digitalRead(_gdo_pin) == voltageLevel) && (elapsedTime < timeoutMs)) {
        delay(1);
        elapsedTime++;
    }

    if (elapsedTime < timeoutMs) {
        Serial.printf("[CC1101] Sync pattern [%u] detected\n", voltageLevel);
        return true;
    } else {
        return false;
    }
}

bool CC1101::waitForState(ChipStatusState state)
{
    uint16_t timeout = 0;
    bool isInWantedState = true;
    do
    {
        halRfReadReg(MARCSTATE_ADDR);
        delay(2);
        timeout +=2;
    } while ((status_state != state) && (timeout < STATE_CHG_TMO));

    if(timeout >= STATE_CHG_TMO)
    {
        Serial.printf("[CC1101] Actual state %u != %u\n", (uint8_t)status_state, (uint8_t)state);
        isInWantedState = false;
    }

    return isInWantedState;
}

void CC1101::getRxStats()
{
    lqi = halRfReadReg(LQI_ADDR);
    freqEst = halRfReadReg(FREQEST_ADDR);
    rssiDbm = rssiTo2dbm(halRfReadReg(RSSI_ADDR));

    Serial.println("[CC1101] Rx statistics:");
    Serial.printf("RSSI=%u\n", rssiDbm);
    Serial.printf("LQI=%u\n", lqi);
    Serial.printf("Freq Est=%u\n", freqEst);
}

uint32_t CC1101::readFifoData(int timeoutMs, uint32_t totalSizeBytes, uint8_t *rxBuffer) {
    uint32_t totalBytesReceived = 0;
    uint8_t bytesInRx = 1;
    int elapsedTime = 0;

    while ((totalBytesReceived < totalSizeBytes) && (elapsedTime < timeoutMs)) {
        delay(5);
        elapsedTime += 5;
        bytesInRx = halRfReadReg(RXBYTES_ADDR) & RXBYTES_MASK;
        if (bytesInRx) {
            readBurstReg(RX_FIFO_ADDR, &rxBuffer[totalBytesReceived], bytesInRx);
            totalBytesReceived += bytesInRx;
        }
    }

    return totalBytesReceived;
}

/**********
 * CC1101 helpers
 */

void CC1101::halRfWriteReg(uint8_t reg_addr, uint8_t value)
{
  uint8_t tbuf[2] = { 0 };
  tbuf[0] = reg_addr | WRITE_SINGLE_BYTE;
  tbuf[1] = value;
  uint8_t len = 2;
  spiTransfert(0, tbuf, len);
  status_FIFO_FreeByte = tbuf[0] & 0x0F;
  status_state = static_cast<ChipStatusState>((tbuf[0] >> 4) & 0x0F);
}

uint8_t CC1101::halRfReadReg(uint8_t spi_instr)
{
  uint8_t value;
  uint8_t rbuf[2] = { 0 };
  uint8_t len = 2;

  rbuf[0] = spi_instr | READ_SINGLE_BYTE;
  rbuf[1] = 0;
  spiTransfert(0, rbuf, len);
  // Two bytes are received:
  // 1st: Chip status byte (status + fifo byte)
  // 2nd: Registry content
  status_FIFO_ReadByte = rbuf[0] & 0x0F;
  status_state = static_cast<ChipStatusState>((rbuf[0] >> 4) & 0x0F);
  value = rbuf[1];
  return value;
}

void CC1101::writeCmd(uint8_t spi_instr)
{
  uint8_t tbuf[1] = { 0 };
  tbuf[0] = spi_instr | WRITE_SINGLE_BYTE;
  spiTransfert(0, tbuf, 1);
  status_state = static_cast<ChipStatusState>((tbuf[0] >> 4) & 0x0F);
}

void CC1101::readBurstReg(uint8_t spi_instr, uint8_t *pArr, uint8_t len)
{
  uint8_t rbuf[len + 1];
  uint8_t i = 0;
  memset(rbuf, 0, len + 1);
  rbuf[0] = spi_instr | READ_BURST;
  spiTransfert(0, rbuf, len + 1);
  for (i = 0; i < len; i++)
  {
    pArr[i] = rbuf[i + 1];
  }
  status_FIFO_ReadByte = rbuf[0] & 0x0F;
  status_state = static_cast<ChipStatusState>((rbuf[0] >> 4) & 0x0F);
}

void CC1101::writeBurstReg(uint8_t spi_instr, uint8_t *pArr, uint8_t len)
{
  uint8_t tbuf[len + 1];
  uint8_t i = 0;
  tbuf[0] = spi_instr | WRITE_BURST;
  for (i = 0; i < len; i++)
  {
    tbuf[i + 1] = pArr[i];
  }
  spiTransfert(0, tbuf, len + 1);
  status_FIFO_FreeByte = tbuf[len] & 0x0F;
  status_state = static_cast<ChipStatusState>((tbuf[len] >> 4) & 0x0F);
}

int8_t CC1101::rssiTo2dbm(uint8_t rssi_dec)
{
  int8_t rssi_dbm;
  if (rssi_dec >= 128)
  {
    rssi_dbm = ((rssi_dec - 256) / 2) - 74;			//rssi_offset via datasheet
  }
  else
  {
    rssi_dbm = ((rssi_dec) / 2) - 74;
  }
  return rssi_dbm;
}

/**********
 * INTERNAL
 */

void CC1101::calibrateAndCompensate(uint32_t timeoutMs)
{
  // Trigger calibration using the SCAL strobe
  writeCmd(SCAL);

  // Wait for calibration to complete (poll the FSCAL_DONE bit)
  uint8_t marcstate = 0xFF;
  uint32_t elapsedTime = 0;
  while ((marcstate != MARCSTATE_IDLE) && (marcstate != RX_RX) && (elapsedTime < timeoutMs))
  {
    marcstate = halRfReadReg(MARCSTATE_ADDR); // Polling MARCSTATE
    delay(1);
    elapsedTime++;
  }

  if (elapsedTime >= timeoutMs)
  {
    Serial.println("[CC1101] TMO in calibrateAndCompensate");
    return;
  }
  
  // Configure Frequency Offset Compensation (FOC)
  // FOCCFG - Frequency Offset Compensation Configuration
  // Enable frequency offset compensation with settings for maximum performance
  // Bit 6-4: Frequency compensation loop gain = 1 (normal loop gain)
  // Bit 3: No automatic freeze
  // Bit 2: Automatic frequency compensation enabled
  // Bit 1-0: No data rate offset compensation
  halRfWriteReg(FOCCFG, 0x1D);

  // Print calibration complete and FOC enabled message
  Serial.println("Calibration complete, Frequency Offset Compensation enabled.");

  delay(5);
}

void CC1101::configureRF_0(float freq)
{
  halRfWriteReg(IOCFG2, 0x0D);    // GDO2 Output Pin Configuration : Serial Data Output
  halRfWriteReg(IOCFG0, 0x06);    // GDO0 Output Pin Configuration : Asserts when sync word has been sent / received, and de-asserts at the end of the packet.
  halRfWriteReg(FIFOTHR, 0x47);   // 0x4? adc with bandwith< 325khz
  halRfWriteReg(SYNC1, 0x55);     // 01010101
  halRfWriteReg(SYNC0, 0x00);     // 00000000 
 
  halRfWriteReg(PKTCTRL1, 0x00);  // Preamble quality estimator threshold=0   ; APPEND_STATUS=0; no addr check
  halRfWriteReg(PKTCTRL0, 0x00);  // fix length , no CRC
  halRfWriteReg(FSCTRL1, 0x08);   // Frequency Synthesizer Control
 
  writeFrequency(freq);           // Write frequency
 
  halRfWriteReg(MDMCFG4, 0xF6);   // Modem Configuration   RX filter BW = 58Khz
  halRfWriteReg(MDMCFG3, 0x83);   // Modem Configuration   26M*((256+83h)*2^6)/2^28 = 2.4kbps 
  halRfWriteReg(MDMCFG2, 0x02);   // Modem Configuration   2-FSK;  no Manchester ; 16/16 sync word bits detected
  halRfWriteReg(MDMCFG1, 0x00);   // Modem Configuration num preamble 2=>0 , Channel spacing_exp
  halRfWriteReg(MDMCFG0, 0x00);   // MDMCFG0 Channel spacing = 25Khz
  halRfWriteReg(DEVIATN, 0x15);   // 5.157471khz 
  halRfWriteReg(MCSM1, 0x00);     // CCA always ; default mode IDLE
  halRfWriteReg(MCSM0, 0x18);     // Main Radio Control State Machine Configuration
  halRfWriteReg(FOCCFG, 0x1D);    // Frequency Offset Compensation Configuration
  halRfWriteReg(BSCFG, 0x1C);     // Bit Synchronization Configuration
  halRfWriteReg(AGCCTRL2, 0xC7);  // AGC Control
  halRfWriteReg(AGCCTRL1, 0x00);  // AGC Control
  halRfWriteReg(AGCCTRL0, 0xB2);  // AGC Control
  halRfWriteReg(WORCTRL, 0xFB);   // Wake On Radio Control
  halRfWriteReg(FREND1, 0xB6);    // Front End RX Configuration
  halRfWriteReg(FSCAL3, 0xE9);    // Frequency Synthesizer Calibration
  halRfWriteReg(FSCAL2, 0x2A);    // Frequency Synthesizer Calibration
  halRfWriteReg(FSCAL1, 0x00);    // Frequency Synthesizer Calibration
  halRfWriteReg(FSCAL0, 0x1F);    // Frequency Synthesizer Calibration
  halRfWriteReg(TEST2, 0x81);     // Various Test Settings link to adc retention
  halRfWriteReg(TEST1, 0x35);     // Various Test Settings link to adc retention
  halRfWriteReg(TEST0, 0x09);     // Various Test Settings link to adc retention

  writeBurstReg(PATABLE_ADDR, PA, 8);
}

void CC1101::writeFrequency(float mhz) {
  byte freq2 = 0;
  byte freq1 = 0;
  byte freq0 = 0;

  //Serial.printf("%.4f Mhz : ", mhz);

  for (bool i = 0; i == 0;) {
    if (mhz >= 26) {
      mhz -= 26;
      freq2 += 1;
    }
    else if (mhz >= 0.1015625) {
      mhz -= 0.1015625;
      freq1 += 1;
    }
    else if (mhz >= 0.00039675) {
      mhz -= 0.00039675;
      freq0 += 1;
    }
    else { i = 1; }
  }
  if (freq0 > 255) { freq1 += 1; freq0 -= 256; }

  halRfWriteReg(FREQ2, freq2);
  halRfWriteReg(FREQ1, freq1);
  halRfWriteReg(FREQ0, freq0);
}

/********
 * SPI
 ********
 */
void CC1101::spiConfigure(int channel, int speed)
{
  _spi_speed = speed;

  pinMode(PIN_SPI_SS, OUTPUT);
  digitalWrite(PIN_SPI_SS, HIGH);

  SPI.pins(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SPI_SS);
  SPI.begin();
}

void CC1101::spiTransfert(int channel, unsigned char *data, int len)
{
  SPI.beginTransaction(SPISettings(_spi_speed, MSBFIRST, SPI_MODE0));

  digitalWrite(PIN_SPI_SS, 0);
  SPI.transfer(data, len);
  digitalWrite(PIN_SPI_SS, 1);

  SPI.endTransaction();
}