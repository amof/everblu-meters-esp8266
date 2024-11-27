#ifndef __UTILS_H__
#define __UTILS_H__

#include <Arduino.h>

/**
 * @brief Calculates the 16-bit Kermit CRC checksum for the given data.
 *
 * @param[in] input_ptr Pointer to the input data.
 * @param[in] num_bytes Number of bytes to process.
 * @return The 16-bit CRC Kermit value with bytes swapped.
 */
uint16_t crc_kermit(const unsigned char *input_ptr, size_t num_bytes);

/**
 * Reverses the bit order of the input data and adds a start bit before and a stop bit
 * after each byte.
 *
 * @param inputBuffer Points to the unencoded data.
 * @param inputBufferLen Number of bytes of unencoded data.
 * @param outputBuffer Points to the encoded data.
 * @return Number of bytes written to outputBuffer.
 */
uint32_t encode2serial_1_3(uint8_t *inputBuffer, uint32_t inputBufferLen, uint8_t *outputBuffer);

/**
 * @brief Decodes a bitstream by removing start and stop bits and handling oversampled bits.
 *
 * This function processes a bitstream encoded with start and stop bits, reversing the encoding
 * to recover the original data. It also decodes oversampled bits, where 0xF0 is treated as `1,0`.
 *
 * @param[in] rxBuffer Pointer to the input bitstream.
 * @param[in] l_total_byte Length of the input bitstream in bytes.
 * @param[out] decoded_buffer Buffer to store the decoded data.
 * @return Number of bytes written to the decoded_buffer.
 */
uint8_t decode_4bitpbit_serial(const uint8_t *rxBuffer, int l_total_byte, uint8_t *decoded_buffer);

void show_in_hex_array(uint8_t *buffer, uint32_t len);
void show_in_hex_one_line(uint8_t *buffer, size_t len);

#endif // __UTILS_H__