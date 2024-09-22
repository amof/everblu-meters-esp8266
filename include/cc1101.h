#ifndef __CC1101_H__
#define __CC1101_H__

class CC1101
{
public:
  enum ChipStatusState {
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
   * @param voltageLevel 
   * @param timeoutMs 
   * @return true 
   * @return false 
   */
  bool waitForGdo0Change(uint8_t voltageLevel, uint32_t timeoutMs);
  /**
   * @brief Wait the chip status to be in the desired state
   * 
   * @param state 
   * @return true 
   * @return false 
   */
  bool waitForState(ChipStatusState state);
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
   * @brief Go into IDLE and flush FIFO RX/TX
   * 
   */
  void reset(void);
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