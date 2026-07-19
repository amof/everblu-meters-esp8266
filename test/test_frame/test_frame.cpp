// Tier 3 of ADR-0003: pin frame construction to the vectors published on the
// Maison Simon wiki (docs/cyble/radian-protocol.md). These are ground truth
// independent of both this codebase and the pre-rewrite one, captured off the
// air from real meters.

#include <unity.h>
#include <string.h>
#include <Arduino.h>
#include <SPI.h>
#include <utils.h>

FakeSerial Serial;
// Nothing here touches SPI, but cc1101.cpp is linked into every test binary in
// this environment and refers to the instance. Defining it costs a line and
// keeps the source filter uniform across test directories.
FakeSPI SPI;

// The wiki's worked example: serial 16-0123456.
// 13 10 00 45 10 01 E2 40 00 45 67 89 AB CD 00 0A 40 | DA DC
static const uint8_t WIKI_MASTER_REQUEST_BODY[] = {
    0x13, 0x10, 0x00, 0x45, 0x10, 0x01, 0xE2, 0x40, 0x00,
    0x45, 0x67, 0x89, 0xAB, 0xCD, 0x00, 0x0A, 0x40};

// 12 06 00 45 67 89 AB CD 00 45 10 01 E2 40 00 0A | 90 9E
static const uint8_t WIKI_METER_ACK_BODY[] = {
    0x12, 0x06, 0x00, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x00,
    0x45, 0x10, 0x01, 0xE2, 0x40, 0x00, 0x0A};

void test_crc_matches_wiki_master_request(void)
{
    uint16_t crc = crc_kermit(WIKI_MASTER_REQUEST_BODY,
                              sizeof(WIKI_MASTER_REQUEST_BODY));
    TEST_ASSERT_EQUAL_HEX16(0xDADC, crc);
}

void test_crc_matches_wiki_meter_ack(void)
{
    uint16_t crc = crc_kermit(WIKI_METER_ACK_BODY, sizeof(WIKI_METER_ACK_BODY));
    TEST_ASSERT_EQUAL_HEX16(0x909E, crc);
}

void test_crc_of_empty_input_is_zero(void)
{
    // Kermit's initial value is 0, and the byte swap of 0 is still 0.
    TEST_ASSERT_EQUAL_HEX16(0x0000, crc_kermit((const unsigned char *)"", 0));
}

/**
 * The on-wire length is forced by the serial framing: every byte becomes
 * 1 start bit + 8 data bits + 3 stop bits = 12 bits. 19 bytes therefore
 * occupy 228 bits = 28.5 bytes, padded to 29, plus a trailing 0xFF.
 *
 * This is what makes TX_BUFFER_SIZE 39: a 9-byte sync pattern precedes it.
 */
void test_encoded_length_is_twelve_bits_per_byte(void)
{
    uint8_t input[19];
    uint8_t output[64];
    memset(input, 0x00, sizeof(input));
    memset(output, 0x00, sizeof(output));

    uint32_t len = encode2serial_1_3(input, sizeof(input), output);

    TEST_ASSERT_EQUAL_UINT32(30, len);
    TEST_ASSERT_EQUAL_UINT32(39, 9 + len); // the sync pattern, then this
}

/**
 * Guards the framing itself rather than a length: an all-zero input must come
 * out as start bit 0, eight data 0s, then three stop 1s, repeating. The first
 * byte is therefore 0b0000_0000 -> 0x00 and the pattern of stop bits appears
 * from bit 9 onward.
 */
void test_encoding_inserts_start_and_stop_bits(void)
{
    uint8_t input[2] = {0x00, 0x00};
    uint8_t output[8];
    memset(output, 0xAA, sizeof(output));

    encode2serial_1_3(input, sizeof(input), output);

    // bits 0..8   = start bit + 8 zero data bits -> first byte all zero
    TEST_ASSERT_EQUAL_HEX8(0x00, output[0]);
    // bits 9,10,11 = stop bits (1), bit 12 = next start bit (0)
    TEST_ASSERT_EQUAL_HEX8(0x70, output[1] & 0xF0);
}

/** Expand each transmitted bit to the 4 samples it occupies on the wire. */
static uint32_t upsample4(const uint8_t *in, uint32_t nbits, uint8_t *out)
{
    uint32_t j = 0;
    for (uint32_t i = 0; i < nbits; i++)
    {
        uint8_t bit = (in[i / 8] >> (7 - i % 8)) & 1;
        for (int k = 0; k < 4; k++, j++)
        {
            if (bit)
                out[j / 8] |= 1 << (7 - j % 8);
            else
                out[j / 8] &= ~(1 << (7 - j % 8));
        }
    }
    return (j + 7) / 8;
}

/**
 * CHARACTERISATION TEST, not a correctness test.
 *
 * Feeding decode_4bitpbit_serial a synthetic 4x-oversampled stream built from
 * encode2serial_1_3 recovers the body of the frame byte-for-byte, but not its
 * first byte or its last: the decoder seeds bit_pol from the very first sample
 * with bit_cnt = 0, so it swallows one bit of whatever starts the buffer, and
 * it stops one byte early.
 *
 * On the air that start-up transient lands in the sync pattern rather than in
 * the data, which is presumably why this has never mattered. But that is an
 * assumption, not something we have confirmed - prefixing the stream with idle
 * bits here does NOT fix it, it trips the dest_bit_cnt == 10 early return
 * instead. What the real RX buffer looks like at its first sample is an
 * empirical question that needs a capture off a live meter to settle.
 *
 * This test therefore locks down the 17 bytes that do round-trip, so a
 * regression in the bulk of the decoder is caught, and deliberately does not
 * assert that the edges are correct. See ADR-0003 on the RX side being the
 * half we cannot yet verify.
 */
void test_decoder_round_trips_frame_body(void)
{
    uint8_t frame[19];
    uint8_t encoded[64];
    uint8_t wire[256];
    uint8_t decoded[64];

    for (uint8_t i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)(0x13 + i * 7); // arbitrary but fixed
    memset(encoded, 0, sizeof(encoded));
    memset(wire, 0, sizeof(wire));
    memset(decoded, 0, sizeof(decoded));

    encode2serial_1_3(frame, sizeof(frame), encoded);
    uint32_t wireLen = upsample4(encoded, sizeof(frame) * 12, wire);
    uint8_t n = decode_4bitpbit_serial(wire, (int)wireLen, decoded);

    TEST_ASSERT_EQUAL_UINT8(sizeof(frame) - 1, n); // one byte short, as above
    // Byte 0 is the known transient; compare from byte 1 to the last decoded.
    TEST_ASSERT_EQUAL_HEX8_ARRAY(&frame[1], &decoded[1], sizeof(frame) - 2);
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_matches_wiki_master_request);
    RUN_TEST(test_crc_matches_wiki_meter_ack);
    RUN_TEST(test_crc_of_empty_input_is_zero);
    RUN_TEST(test_encoded_length_is_twelve_bits_per_byte);
    RUN_TEST(test_encoding_inserts_start_and_stop_bits);
    RUN_TEST(test_decoder_round_trips_frame_body);
    return UNITY_END();
}
