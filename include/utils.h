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

/**
 * @brief How many bytes of a decoded buffer belong to the frame.
 *
 * Byte 0 of any Radian frame is L, its own total length. That is not the same
 * as how many bytes came out of the decoder, and it differs in both directions:
 * a capture with too few samples decodes fewer bytes than L, while a capture
 * with slack decodes MORE, because the decoder carries on into whatever follows
 * the frame until the samples run out.
 *
 * Getting this wrong is not a subtle failure. Checksumming to the decoded
 * length folds post-frame noise into the sum and fails a frame that is entirely
 * correct.
 *
 * @return min(L, decodedSize), or decodedSize when L is not a plausible length.
 */
uint8_t radian_frame_length(const uint8_t *frame, uint8_t decodedSize);

/**
 * @brief Does a frame's trailing Kermit checksum match its contents?
 *
 * The only test that proves a frame is the one the meter sent, rather than one
 * the decoder happened to produce of the right length. @p declaredLen is the
 * frame's own L (byte 0), which is what the checksum covers — pass the result
 * of radian_frame_length, never the raw decoded length, or trailing noise gets
 * folded into the sum. Same swapped-byte convention as crc_kermit.
 */
bool radian_checksum_ok(const uint8_t *frame, uint8_t declaredLen);

/**
 * @brief Standard base64, NUL-terminated.
 *
 * Written here rather than using the ESP8266 core's base64.h because this file
 * is compiled into the native test build, which has no such header.
 *
 * @return Characters written excluding the terminator, or 0 if outSize is too
 *         small — a truncated capture is worse than none, so it is not emitted.
 */
size_t base64_encode(const uint8_t *in, size_t inLen, char *out, size_t outSize);

#endif // __UTILS_H__