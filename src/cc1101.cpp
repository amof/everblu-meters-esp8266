/*  the radian_trx SW shall not be distributed  nor used for commercial product*/
/*  it is exposed just to demonstrate CC1101 capability to reader water meter indexes */
/*  there is no Warranty on radian_trx SW */

#include <arduino.h>

#include <SPI.h>
#include "cc1101.h"
#include "cc1101_registers.h"
#include "everblu_log.h"

uint8_t PA[] = {
    0x60,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};

#define SPI_SPEED 500000UL
#define STATE_CHG_TMO 25 // State change timeout
#define CHIP_RDY_TMO 100 // CHIP_RDYn (XOSC stable) timeout

CC1101::CC1101(uint8_t gd0_pin)
{
  _gdo_pin = gd0_pin;
}

void CC1101::init(void)
{
  pinMode(_gdo_pin, INPUT_PULLUP);

  spiConfigure(0, SPI_SPEED);
  reset();

  // The identity is not read here: checkWiring() reads it and judges it, and
  // the caller runs that immediately after. Reading it twice would only put
  // two copies of the same two bytes in the log.
}

void CC1101::setFrequency(float freq)
{
  configureRF_0(freq);
}

ChipIdentity CC1101::readIdentity(void)
{
  _identity.partNumber = halRfReadReg(PARTNUM_ADDR);
  _identity.version = halRfReadReg(VERSION_ADDR);

  // A part number of 0x00 with a version of 0x14 is the expected answer;
  // 0x00/0xFF or 0xFF/0xFF means SPI is not reaching the chip.
  LOG("[CC1101] Partnumber: 0x%02X\n", _identity.partNumber);
  LOG("[CC1101] Version: 0x%02X\n", _identity.version);
  delay(1);

  return _identity;
}

WiringCheckResult CC1101::checkWiring(void)
{
  readIdentity();

  if (_identity.partNumber != CC1101_EXPECTED_PARTNUM ||
      _identity.version != CC1101_EXPECTED_VERSION)
  {
    LOG("[Wiring] SPI does not reach the chip, check MOSI/MISO/SCK/CSn\n");
    return WIRING_SPI_FAILED;
  }

  // Drive GDO0 low from the chip and read it back. This is the load-bearing
  // half: the pin is INPUT_PULLUP, so a wire that is missing, on the wrong pin
  // or broken reads high here.
  halRfWriteReg(IOCFG0, IOCFG_GDO_HW_TO_0);
  delay(1);
  bool lowSeen = (digitalRead(_gdo_pin) == LOW);

  // Then the inverse, which is what catches a pin shorted to ground. Without
  // it a permanently-low pin would pass the test above and look perfect.
  halRfWriteReg(IOCFG0, IOCFG_GDO_HW_TO_0 | IOCFG_GDO_INV);
  delay(1);
  bool highSeen = (digitalRead(_gdo_pin) == HIGH);

  // Restored before returning on either path. Leaving IOCFG0 on a constant
  // level would break every subsequent interrogation by way of the diagnostic
  // meant to protect them — waitForGdo0Assert() would wait forever, or return
  // instantly, and the meter would be blamed.
  halRfWriteReg(IOCFG0, IOCFG_GDO0_SYNC);

  if (!lowSeen || !highSeen)
  {
    LOG("[Wiring] GDO0 did not follow the chip (low %s, high %s), check the GDO0 wire\n",
        lowSeen ? "ok" : "FAIL", highSeen ? "ok" : "FAIL");
    return WIRING_GDO0_FAILED;
  }

  LOG("[Wiring] SPI and GDO0 ok\n");
  return WIRING_OK;
}

bool CC1101::waitForGdo0Assert(uint32_t timeoutMs)
{
  uint32_t elapsedTime = 0;
  bool gdo0_has_asserted = false;

  // IOCFG0 = 0x06: GDO0 goes HIGH when the sync word is received. It is LOW
  // when we get here, so we wait for the rising edge. Waiting for it to go LOW
  // instead returns immediately and reads the FIFO before the frame arrives.
  while ((digitalRead(_gdo_pin) == LOW) && (elapsedTime < timeoutMs))
  {
    delay(1);
    elapsedTime++;
  }

  if (elapsedTime >= timeoutMs)
  {
    LOG("[CC1101] TMO waiting for GDO0 to assert after %ums\n", timeoutMs);
  }
  else
  {
    gdo0_has_asserted = true;
  }

  return gdo0_has_asserted;
}

bool CC1101::waitForState(ChipStatusState state)
{
  uint16_t timeout = 0;

  do
  {
    halRfReadReg(MARCSTATE_ADDR); // Side effect: refreshes status_state
    if (status_state == state)
      return true;
    delay(1);
    timeout += 1;
  } while (timeout < STATE_CHG_TMO);

  LOG("[CC1101] TMO: Actual state %u != %u\n", (uint8_t)status_state, (uint8_t)state);
  return false;
}

bool CC1101::strobeAndWait(uint8_t strobe, ChipStatusState expected, uint32_t settleMs)
{
  writeCmd(strobe);
  delay(settleMs); // Cover the state transition (Table 34)
  return waitForState(expected);
}

void CC1101::idleAndFlush(void)
{
  // Table 42: SFTX/SFRX are only legal in IDLE, TXFIFO_UNDERFLOW or
  // RXFIFO_OVERFLOW. Forcing IDLE first makes the flush legal from any state.
  writeCmd(SIDLE);
  waitForState(CHIP_SS_IDLE);
  writeCmd(SFTX);
  writeCmd(SFRX);
}

uint8_t CC1101::readStatusReg(uint8_t addr)
{
  // Errata: a status register must read the same value twice in a row,
  // otherwise the value may be a mix of two internal states.
  uint8_t previous = halRfReadReg(addr);
  for (uint8_t attempt = 0; attempt < 8; attempt++)
  {
    uint8_t current = halRfReadReg(addr);
    if (current == previous)
      return current;
    previous = current;
  }
  return previous;
}

bool CC1101::waitTxFifoFree(uint8_t minFreeBytes, uint32_t timeoutMs)
{
  uint32_t start = millis();

  do
  {
    uint8_t txBytes = readStatusReg(TXBYTES_ADDR);

    if (txBytes & TXBYTES_UNDERFLOW)
    {
      LOG("[CC1101] TX FIFO underflowed, transmission is lost\n");
      return false;
    }
    if ((CC1101_FIFO_SIZE - (txBytes & TXBYTES_MASK)) >= minFreeBytes)
      return true;

    delay(1);
  } while ((millis() - start) < timeoutMs);

  LOG("[CC1101] TMO waiting for %u free bytes in TX FIFO\n", minFreeBytes);
  return false;
}

bool CC1101::waitTxFifoDrained(uint32_t timeoutMs)
{
  uint32_t start = millis();

  do
  {
    uint8_t txBytes = readStatusReg(TXBYTES_ADDR);

    // In infinite packet length mode running dry IS the end of transmission.
    if ((txBytes & TXBYTES_UNDERFLOW) || ((txBytes & TXBYTES_MASK) == 0))
      return true;

    delay(1);
  } while ((millis() - start) < timeoutMs);

  LOG("[CC1101] TMO waiting for TX FIFO to drain\n");
  return false;
}

void CC1101::getRxStats()
{
  lqi = halRfReadReg(LQI_ADDR);
  freqEst = halRfReadReg(FREQEST_ADDR);
  rssiDbm = rssiTo2dbm(halRfReadReg(RSSI_ADDR));

  LOG("[CC1101] Rx statistics:\n");
  LOG("\tRSSI=%ddBm\n", rssiDbm);
  LOG("\tLQI=%u\n", lqi);
  LOG("\tFreq Est=%u\n", freqEst);
}

uint32_t CC1101::readFifoData(uint32_t timeoutMs, uint32_t totalSizeBytes, uint8_t *rxBuffer)
{
  uint32_t totalBytesReceived = 0;
  uint8_t bytesInRx = 0;
  uint32_t elapsedTime = 0;

  while ((totalBytesReceived < totalSizeBytes) && (elapsedTime < timeoutMs))
  {
    delay(5);
    elapsedTime += 5;

    uint8_t rxStatus = readStatusReg(RXBYTES_ADDR);
    if (rxStatus & RXBYTES_OVERFLOW)
    {
      LOG("[CC1101] RX FIFO overflowed, data is lost\n");
      break;
    }

    bytesInRx = rxStatus & RXBYTES_MASK;
    if (bytesInRx > 0)
    {
      // Never read past the caller's buffer: the FIFO can hold more than the
      // remaining expected bytes, and reading past the empty value corrupts
      // the data read out (datasheet 10.1).
      uint32_t remaining = totalSizeBytes - totalBytesReceived;
      if (bytesInRx > remaining)
        bytesInRx = (uint8_t)remaining;

      readBurstReg(RX_FIFO_ADDR, &rxBuffer[totalBytesReceived], bytesInRx);
      totalBytesReceived += bytesInRx;
    }
  }
  LOG("[CC1101] Bytes received in FIFO: %u (expected: %u)\n", totalBytesReceived, totalSizeBytes);
  return totalBytesReceived;
}

/**********
 * CC1101 helpers
 */

void CC1101::halRfWriteReg(uint8_t reg_addr, uint8_t value)
{
  uint8_t tbuf[2] = {0};
  tbuf[0] = reg_addr | WRITE_SINGLE_BYTE;
  tbuf[1] = value;
  uint8_t len = 2;
  spiTransfert(0, tbuf, len);
  status_state = static_cast<ChipStatusState>((tbuf[0] >> 4) & 0x07);
}

uint8_t CC1101::halRfReadReg(uint8_t spi_instr)
{
  uint8_t value;
  uint8_t rbuf[2] = {0};
  uint8_t len = 2;

  rbuf[0] = spi_instr | READ_SINGLE_BYTE;
  rbuf[1] = 0;
  spiTransfert(0, rbuf, len);
  // Two bytes are received:
  // 1st: Chip status byte (status + fifo byte)
  // 2nd: Registry content
  // Table 23: bit 7 is CHIP_RDYn, bits 6:4 are STATE, bits 3:0 are
  // FIFO_BYTES_AVAILABLE. STATE is three bits, so masking four would fold
  // CHIP_RDYn into the state and make every comparison fail once the crystal
  // is not yet stable.
  status_state = static_cast<ChipStatusState>((rbuf[0] >> 4) & 0x07);
  value = rbuf[1];
  return value;
}

void CC1101::writeCmd(uint8_t spi_instr)
{
  uint8_t tbuf[1] = {0};
  tbuf[0] = spi_instr | WRITE_SINGLE_BYTE;
  spiTransfert(0, tbuf, 1);
  // When writing a command strobe the chip status byte is returned on SO.
  status_state = static_cast<ChipStatusState>((tbuf[0] >> 4) & 0x07);
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
  status_state = static_cast<ChipStatusState>((rbuf[0] >> 4) & 0x07);
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
  status_state = static_cast<ChipStatusState>((tbuf[len] >> 4) & 0x07);
}

int8_t CC1101::rssiTo2dbm(uint8_t rssi_dec)
{
  int8_t rssi_dbm;
  if (rssi_dec >= 128)
  {
    rssi_dbm = ((rssi_dec - 256) / 2) - 74; // rssi_offset via datasheet
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
bool CC1101::waitMisoLow(uint32_t timeoutMs)
{
  uint32_t start = millis();

  while (digitalRead(PIN_SPI_MISO))
  {
    if ((millis() - start) >= timeoutMs)
    {
      LOG("[CC1101] TMO waiting for CHIP_RDYn, check wiring/power\n");
      return false;
    }
    yield(); // Feed the watchdog: a missing chip must not reboot the ESP
  }
  return true;
}

void CC1101::reset(void)
{
  // Power-on start-up sequence, datasheet 19.1.1.
  // CSn must stay LOW across the SRES strobe and the CHIP_RDYn waits, so the
  // strobe is issued inline instead of through writeCmd() which toggles CSn.
  digitalWrite(PIN_SPI_SS, HIGH);
  delayMicroseconds(5);
  digitalWrite(PIN_SPI_SS, LOW);
  delayMicroseconds(45); // 19.1.1: hold CSn low for at least 40us
  digitalWrite(PIN_SPI_SS, HIGH);
  delayMicroseconds(45); // 19.1.1: and then high for at least 40us

  SPI.beginTransaction(SPISettings(_spi_speed, MSBFIRST, SPI_MODE0));

  // Pull CSn low and wait for SO to go low (CHIP_RDYn: XOSC stable)
  digitalWrite(PIN_SPI_SS, LOW);
  waitMisoLow(CHIP_RDY_TMO);

  // Issue SRES on SI while CSn is held low
  SPI.transfer(SRES | WRITE_SINGLE_BYTE);

  // When SO goes low again the reset is complete and the chip is in IDLE
  waitMisoLow(CHIP_RDY_TMO);

  digitalWrite(PIN_SPI_SS, HIGH);
  SPI.endTransaction();

  // Chip is in IDLE, so flushing both FIFOs is legal (Table 42)
  writeCmd(SFTX);
  writeCmd(SFRX);

  delay(1);
}

void CC1101::configureRF_0(float freq)
{
  halRfWriteReg(IOCFG2, 0x0D);  // GDO2 Output Pin Configuration : Serial Data Output
  halRfWriteReg(IOCFG0, IOCFG_GDO0_SYNC); // GDO0 Output Pin Configuration : Asserts when sync word has been sent / received, and de-asserts at the end of the packet.
  halRfWriteReg(FIFOTHR, 0x47); // 0x4? adc with bandwith< 325khz
  halRfWriteReg(SYNC1, 0x55);   // 01010101
  halRfWriteReg(SYNC0, 0x00);   // 00000000

  halRfWriteReg(PKTCTRL1, 0x00); // Preamble quality estimator threshold=0   ; APPEND_STATUS=0; no addr check
  halRfWriteReg(PKTCTRL0, 0x00); // fix length , no CRC
  halRfWriteReg(FSCTRL1, 0x08);  // Frequency Synthesizer Control

  writeFrequency(freq); // Write frequency

  halRfWriteReg(MDMCFG4, 0xF6);  // Modem Configuration   RX filter BW = 58Khz
  halRfWriteReg(MDMCFG3, 0x83);  // Modem Configuration   26M*((256+83h)*2^6)/2^28 = 2.4kbps
  halRfWriteReg(MDMCFG2, 0x02);  // Modem Configuration   2-FSK;  no Manchester ; 16/16 sync word bits detected
  halRfWriteReg(MDMCFG1, 0x00);  // Modem Configuration num preamble 2=>0 , Channel spacing_exp
  halRfWriteReg(MDMCFG0, 0x00);  // MDMCFG0 Channel spacing = 25Khz
  halRfWriteReg(DEVIATN, 0x15);  // 5.157471khz
  halRfWriteReg(MCSM1, 0x00);    // CCA always ; default mode IDLE
  halRfWriteReg(MCSM0, 0x18);    // Main Radio Control State Machine Configuration
  halRfWriteReg(FOCCFG, 0x1D);   // Frequency Offset Compensation Configuration
  halRfWriteReg(BSCFG, 0x1C);    // Bit Synchronization Configuration
  halRfWriteReg(AGCCTRL2, 0xC7); // AGC Control
  halRfWriteReg(AGCCTRL1, 0x00); // AGC Control
  halRfWriteReg(AGCCTRL0, 0xB2); // AGC Control
  halRfWriteReg(WORCTRL, 0xFB);  // Wake On Radio Control
  halRfWriteReg(FREND1, 0xB6);   // Front End RX Configuration
  halRfWriteReg(FSCAL3, 0xE9);   // Frequency Synthesizer Calibration
  halRfWriteReg(FSCAL2, 0x2A);   // Frequency Synthesizer Calibration
  halRfWriteReg(FSCAL1, 0x00);   // Frequency Synthesizer Calibration
  halRfWriteReg(FSCAL0, 0x1F);   // Frequency Synthesizer Calibration
  halRfWriteReg(TEST2, 0x81);    // Various Test Settings link to adc retention
  halRfWriteReg(TEST1, 0x35);    // Various Test Settings link to adc retention
  halRfWriteReg(TEST0, 0x09);    // Various Test Settings link to adc retention

  writeBurstReg(PATABLE_ADDR, PA, 8);
}

void CC1101::writeFrequency(float mhz)
{
  byte freq2 = 0;
  byte freq1 = 0;
  byte freq0 = 0;

  // LOG("%.4f Mhz : ", mhz);

  for (bool i = 0; i == 0;)
  {
    if (mhz >= 26)
    {
      mhz -= 26;
      freq2 += 1;
    }
    else if (mhz >= 0.1015625)
    {
      mhz -= 0.1015625;
      freq1 += 1;
    }
    else if (mhz >= 0.00039675)
    {
      mhz -= 0.00039675;
      freq0 += 1;
    }
    else
    {
      i = 1;
    }
  }
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