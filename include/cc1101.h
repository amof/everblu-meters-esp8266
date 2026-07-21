#ifndef __CC1101_H__
#define __CC1101_H__

/**
 * What the transceiver reports about itself. Evidence behind a wiring verdict
 * rather than a fact of interest on its own: the expected pair proves SPI is
 * reaching the chip, all-ones proves it is not.
 */
struct ChipIdentity
{
  uint8_t partNumber;
  uint8_t version;
};

/**
 * Outcome of a wiring check.
 *
 * Ordered, not a set. With SPI dead the GDO0 test is meaningless — the pin
 * would only be read through its own pull-up — so a SPI failure short-circuits
 * and GDO0 is never claimed either way.
 */
enum WiringCheckResult
{
  WIRING_OK,          // SPI reaches the chip and GDO0 reaches the expected pin
  WIRING_SPI_FAILED,  // PARTNUM/VERSION are not what a CC1101 answers
  WIRING_GDO0_FAILED, // Chip is there, but driving GDO0 does not move the pin
};

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
   * @brief Read and log the chip's part number and version.
   *
   * Returns them rather than only logging, because the wiring check judges on
   * these same two bytes and must not read them a second time.
   */
  ChipIdentity readIdentity(void);
  /**
   * @brief Prove the transceiver is wired up, without transmitting anything.
   *
   * Checks that SPI reaches the chip, then that GDO0 reaches the pin this
   * driver believes it does, by asking the chip to drive GDO0 to a constant
   * level and reading the pin back. The chip stays in IDLE and the synthesiser
   * never runs, so this is safe on a bench with no antenna attached and
   * outside the meter's wakeup window.
   *
   * Both polarities are tested because the pin is INPUT_PULLUP: a disconnected
   * GDO0 floats high, so only the drive-low step catches a missing wire, and
   * only the drive-high step catches a pin shorted to ground.
   *
   * Proves nothing about the crystal, the PA or the antenna. Only a completed
   * interrogation shows those.
   *
   * See docs/adr/0005-the-wiring-check-never-transmits-and-never-blocks.md
   */
  WiringCheckResult checkWiring(void);
  /** Identity read by the last readIdentity() or checkWiring(). */
  ChipIdentity identity(void) const { return _identity; }
  /**
   * @brief Block until GDO0 asserts, i.e. until the sync word is received.
   *
   * Assumes IOCFG0 = 0x06, where GDO0 goes HIGH on sync word sent/received and
   * LOW again at the end of the packet. The pin is therefore LOW on entry and
   * this waits for the rising edge.
   *
   * The polarity is in the name on purpose: this was previously
   * `waitForGdo0Change`, which said nothing about direction, and a refactor
   * silently inverted it so that it returned immediately and the caller read
   * the FIFO before the frame had arrived.
   *
   * @param timeoutMs
   * @return false on timeout
   */
  bool waitForGdo0Assert(uint32_t timeoutMs);
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
   * Datasheet Table 42 is per-strobe: SFTX only in IDLE or TXFIFO_UNDERFLOW,
   * SFRX only in IDLE or RXFIFO_OVERFLOW. Going through IDLE first makes both
   * flushes legal from any state without having to know which.
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
   * the FIFO can hold (datasheet 20 - a TX FIFO overflow corrupts the FIFO
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
  uint8_t lqi = 0;
  uint8_t freqEst = 0;
  int8_t rssiDbm = 0; // Signed: every real RSSI reading is negative

private:
  int _spi_speed = 0;
  uint8_t _gdo_pin = 0;
  ChipIdentity _identity = {0, 0};

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