#ifndef __CC1101_H__
#define __CC1101_H__

class CC1101
{
public:
  enum ChipStatusState
  {
    CHIP_SS_IDLE = 0,            // IDLE state
    CHIP_SS_RX = 1,              // Receive mode
    CHIP_SS_TX = 2,              // Transmit mode
    CHIP_SS_FSTXON = 3,          // Fast TX ready
    CHIP_SS_CALIBRATE = 4,       // Frequency synthesizer calibration is running
    CHIP_SS_SETTLING = 5,        // PLL is settling
    CHIP_SS_RXFIFO_OVERFLOW = 6, // RX FIFO has overflowed
    CHIP_SS_TXFIFO_UNDERFLOW = 7 // TX FIFO has underflowed
  };

  CC1101(uint8_t gd0_pin);
  /**
   * @brief Configure GDO0 pin in INPUT, configure SPI and print version of chip
   *
   */
  void init(void);
  /**
   * @brief Do a reset + configureRF_0
   *
   * @param freq
   */
  void setFrequency(float freq);
  /**
   * @brief Get the statistics for RF link quality. Update lqi, freqEst and rssiDbm.
   *
   */
  void getRxStats(void);
  /**
   * @brief Read the fifo data in RX from CC1101.
   *
   * @param timeoutMs
   * @param totalSizeBytes
   * @param rxBuffer
   * @return uint32_t
   */
  uint32_t readFifoData(uint32_t timeoutMs, uint32_t totalSizeBytes, uint8_t *rxBuffer);
  /**
   * @brief Print all the registers from CC1101
   *
   */
  void printRegistersSettings(void);
  /**
   * @brief Print the version and part number of CC1101
   *
   */
  void version(void);
  /**
   * @brief Wait for GDO0 to change
   *
   * @param timeoutMs
   * @return true
   * @return false
   */
  bool waitForGdo0Change(uint32_t timeoutMs);
  /**
   * @brief Wait the chip status to be in the desired state
   *
   * @param state
   * @return true
   * @return false
   */
  bool waitForState(ChipStatusState state);
  /**
   * @brief Issue a command strobe and wait for the chip to settle in a state.
   *
   * @param strobe Command strobe to issue
   * @param expected State the chip is expected to reach
   * @param settleMs Delay before polling, to cover the state transition time
   *                 (datasheet Table 34: IDLE->TX/RX with calibration is 799us)
   * @return true if the chip reached the expected state
   */
  bool strobeAndWait(uint8_t strobe, ChipStatusState expected, uint32_t settleMs = 4);
  /**
   * @brief Force the chip to IDLE, then flush both FIFOs.
   *
   * Datasheet Table 42: SFTX and SFRX may only be issued in IDLE,
   * TXFIFO_UNDERFLOW or RXFIFO_OVERFLOW. Going through IDLE first makes the
   * flush legal from any state.
   */
  void idleAndFlush(void);
  /**
   * @brief Read a status register, working around the SPI read errata.
   *
   * The value must be read twice in a row identically, otherwise the value may
   * be a mix of two states.
   *
   * @param addr Status register address (already carries the burst bit)
   * @return Stable register value
   */
  uint8_t readStatusReg(uint8_t addr);
  /**
   * @brief Block until the TX FIFO has room for at least minFreeBytes.
   *
   * This is the backpressure primitive: the caller must never write more than
   * the FIFO can hold (datasheet 10.1 - a TX FIFO overflow corrupts the FIFO
   * content) and must never let it run dry mid-packet (datasheet 15.4 -
   * TXFIFO_UNDERFLOW can only be left via SFTX, and writing to an underflowed
   * FIFO does not restart TX).
   *
   * @param minFreeBytes Free space required before returning
   * @param timeoutMs
   * @return false on timeout or if the FIFO has already underflowed
   */
  bool waitTxFifoFree(uint8_t minFreeBytes, uint32_t timeoutMs);
  /**
   * @brief Block until the TX FIFO has been fully transmitted.
   *
   * In infinite packet length mode the transmission only ends when the FIFO
   * runs dry, which the datasheet defines as TXFIFO_UNDERFLOW. Both an empty
   * FIFO and the underflow flag are therefore accepted as "drained".
   *
   * @param timeoutMs
   * @return false on timeout
   */
  bool waitTxFifoDrained(uint32_t timeoutMs);
  /**
   * @brief Convert RSSI to dbm
   *
   * @param rssi_dec
   * @return int8_t
   */
  int8_t rssiTo2dbm(uint8_t rssi_dec);

  void halRfWriteReg(uint8_t reg_addr, uint8_t value);
  uint8_t halRfReadReg(uint8_t spi_instr);
  void writeCmd(uint8_t spi_instr);
  void readBurstReg(uint8_t spi_instr, uint8_t *pArr, uint8_t len);
  void writeBurstReg(uint8_t spi_instr, uint8_t *pArr, uint8_t len);

  ChipStatusState status_state = CHIP_SS_IDLE;
  uint8_t status_FIFO_FreeByte = 0;
  uint8_t status_FIFO_ReadByte = 0;
  uint8_t lqi = 0;
  uint8_t freqEst = 0;
  uint8_t rssiDbm = 0;

private:
  int _spi_speed = 0;
  uint8_t _gdo_pin = 0;

  // Internal
  /**
   * @brief Reset the chip following the power-on start-up sequence.
   *
   * Datasheet 19.1.1: toggle CSn, pull it low and wait for SO (MISO) to go low
   * (CHIP_RDYn), issue SRES with CSn held low, then wait for SO to go low again.
   */
  void reset(void);
  /**
   * @brief Wait for MISO to go low (CHIP_RDYn), bounded so a dead chip cannot
   *        hang the loop until the watchdog reboots the ESP.
   *
   * @param timeoutMs
   * @return false on timeout
   */
  bool waitMisoLow(uint32_t timeoutMs);
  /**
   * @brief Write calibration
   *
   * @param timeoutMs
   */
  void calibrateAndCompensate(uint32_t timeoutMs);
  /**
   * @brief Initial configuration for all the registers
   *
   * @param freq
   */
  void configureRF_0(float freq);
  /**
   * @brief Calculate the frequency values and write them to the registers
   *
   * @param mhz
   */
  void writeFrequency(float mhz);

  // SPI related functions
  void spiTransfert(int channel, unsigned char *data, int len);
  void spiConfigure(int channel, int speed);
};

#endif // __CC1101_H__