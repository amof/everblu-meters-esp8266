// Tier 3 of ADR-0003: pin frame construction and validation. The vectors here
// are the ones published on the Maison Simon wiki (docs/cyble/radian-protocol.md)
// plus synthetic frames built with arbitrary values — no real meter data. The
// RX-timing tests that needed a live capture live outside the repository with
// the capture itself; ADR-0003 explains why.

#include <unity.h>
#include <string.h>
#include <Arduino.h>
#include <SPI.h>
#include <utils.h>
#include <everblu_log.h>

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

// A capture that decodes to the wrong bytes is worse than no capture: it would
// be checked in as a fixture and every later decoder conclusion drawn from it.
// These are the RFC 4648 vectors, covering all three padding cases.
void test_base64_encodes_known_vectors(void)
{
    char out[8];

    TEST_ASSERT_EQUAL_UINT32(4, base64_encode((const uint8_t *)"Man", 3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TWFu", out);

    TEST_ASSERT_EQUAL_UINT32(4, base64_encode((const uint8_t *)"Ma", 2, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TWE=", out);

    TEST_ASSERT_EQUAL_UINT32(4, base64_encode((const uint8_t *)"M", 1, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("TQ==", out);
}

// High bytes exercise the far end of the alphabet, which a text-only vector
// never reaches — and a radio capture is mostly high bytes.
void test_base64_encodes_binary_bytes(void)
{
    const uint8_t raw[] = {0xFF, 0xEF, 0xBE};
    char out[8];

    TEST_ASSERT_EQUAL_UINT32(4, base64_encode(raw, sizeof(raw), out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("/+++", out);
}

// Refuses rather than truncating: half a capture would decode to a frame that
// was never transmitted.
void test_base64_refuses_a_buffer_that_is_too_small(void)
{
    char out[4]; // needs 5 for one group plus terminator

    TEST_ASSERT_EQUAL_UINT32(0, base64_encode((const uint8_t *)"Man", 3, out, sizeof(out)));
}

// The real case: a full response capture must fit the buffer the log module
// sizes for it, or captures silently stop being published.
void test_base64_response_capture_fits_the_publish_buffer(void)
{
    uint8_t raw[684];
    char out[CAPTURE_B64_MAX];
    memset(raw, 0xA5, sizeof(raw));

    TEST_ASSERT_EQUAL_UINT32(912, base64_encode(raw, sizeof(raw), out, sizeof(out)));
}

// REGRESSION. The checksum must be taken over the frame's declared length, not
// over however many bytes the decoder produced. Once the capture has slack the
// decoder overruns the frame into the noise behind it, and summing to the
// decoded length folds that noise in — failing a frame that is perfectly good,
// on the one criterion the reader uses to accept a reading.
void test_frame_length_is_bounded_by_the_declared_length(void)
{
    // The wiki ack, complete with its checksum, followed by post-frame noise of
    // exactly the kind a roomy capture decodes.
    uint8_t frame[24] = {0x12, 0x06, 0x00, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x00,
                         0x45, 0x10, 0x01, 0xE2, 0x40, 0x00, 0x0A, 0x90, 0x9E,
                         0xFF, 0x3C, 0xA5, 0x00, 0xFF, 0x17};

    TEST_ASSERT_EQUAL_UINT8(18, radian_frame_length(frame, sizeof(frame)));

    // Summed to the declared length the checksum is the wiki's; summed to the
    // decoded length it is not, which is the bug this guards.
    uint8_t n = radian_frame_length(frame, sizeof(frame));
    TEST_ASSERT_EQUAL_HEX16(0x909E, crc_kermit(frame, n - 2));
    TEST_ASSERT_NOT_EQUAL(0x909E, crc_kermit(frame, sizeof(frame) - 2));
}

// The other direction: a short capture decodes fewer bytes than L, and the
// frame length can never claim more data than actually arrived.
void test_frame_length_never_exceeds_what_was_decoded(void)
{
    // The wiki ack again, one byte short of what its own L declares — the shape
    // a capture with too few samples produces.
    uint8_t truncated[17] = {0x12, 0x06, 0x00, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x00,
                             0x45, 0x10, 0x01, 0xE2, 0x40, 0x00, 0x0A, 0x90};

    // L says 18, only 17 arrived: the checksum is simply not reachable.
    TEST_ASSERT_EQUAL_UINT8(17, radian_frame_length(truncated, sizeof(truncated)));
}

// A decode that never synchronised puts garbage in byte 0. Falling back to the
// decoded size keeps the caller's bounds checks honest instead of trusting a
// length that means nothing.
void test_frame_length_falls_back_when_L_is_not_a_length(void)
{
    uint8_t garbage[8] = {0x00, 0xFF, 0xFF, 0x12, 0x34, 0x56, 0x78, 0x9A};

    TEST_ASSERT_EQUAL_UINT8(8, radian_frame_length(garbage, sizeof(garbage)));
    TEST_ASSERT_EQUAL_UINT8(0, radian_frame_length(garbage, 0));
}

// The checksum gate that decides whether a response becomes a reading. The wiki
// ack is a complete frame (L=18) whose checksum is public, so it stands in for
// any whole frame.
void test_checksum_accepts_a_whole_frame(void)
{
    uint8_t frame[18] = {0x12, 0x06, 0x00, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x00,
                         0x45, 0x10, 0x01, 0xE2, 0x40, 0x00, 0x0A, 0x90, 0x9E};

    TEST_ASSERT_TRUE(radian_checksum_ok(frame, 18));
}

// A single flipped byte anywhere before the checksum must fail it — this is the
// whole point of gating on it rather than on length.
void test_checksum_rejects_a_corrupted_frame(void)
{
    uint8_t frame[18] = {0x12, 0x06, 0x00, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0x00,
                         0x45, 0x10, 0x01, 0xE2, 0x40, 0x00, 0x0A, 0x90, 0x9E};
    frame[7] ^= 0x01; // corrupt one address byte

    TEST_ASSERT_FALSE(radian_checksum_ok(frame, 18));
}

// Nonsense lengths are refused rather than read out of bounds.
void test_checksum_refuses_an_implausible_length(void)
{
    uint8_t frame[4] = {0x12, 0x06, 0x00, 0x45};

    TEST_ASSERT_FALSE(radian_checksum_ok(NULL, 18));
    TEST_ASSERT_FALSE(radian_checksum_ok(frame, 2));
}

// A full-length response, stood up from nothing: arbitrary field values, a
// genuine Kermit checksum, and the real 124-byte layout. It stands in for the
// live capture the RX regression once used, exercising the accept path at the
// real frame size — the length cap, the checksum over 122 bytes, the rejection
// of a single flipped byte — without shipping any real meter's data.
static void buildSyntheticResponse(uint8_t frame[124])
{
    memset(frame, 0, 124);
    frame[0] = 0x7C; // L = 124
    frame[1] = 0x11; // control byte: response

    // Current index, little endian: 0x0000C350 = 50000 L. Arbitrary.
    frame[18] = 0x50;
    frame[19] = 0xC3;

    // Thirteen monthly indexes, oldest first, rising as a cumulative index must.
    for (uint8_t m = 0; m < 13; m++)
    {
        uint32_t value = 4000u * (m + 1); // 4000, 8000, ... 52000
        uint8_t at = 70 + m * 4;
        frame[at + 0] = (uint8_t)(value);
        frame[at + 1] = (uint8_t)(value >> 8);
        frame[at + 2] = (uint8_t)(value >> 16);
        frame[at + 3] = (uint8_t)(value >> 24);
    }

    // Real checksum over the 122 bytes before it, high byte first — the same
    // convention createRadianMasterRequest and the reader both use.
    uint16_t crc = crc_kermit(frame, 122);
    frame[122] = (uint8_t)(crc >> 8);
    frame[123] = (uint8_t)(crc & 0xFF);
}

void test_full_response_passes_length_and_checksum(void)
{
    uint8_t frame[124];
    buildSyntheticResponse(frame);

    TEST_ASSERT_EQUAL_UINT8(124, radian_frame_length(frame, 124));
    TEST_ASSERT_TRUE(radian_checksum_ok(frame, radian_frame_length(frame, 124)));
}

void test_full_response_rejected_when_a_payload_byte_flips(void)
{
    uint8_t frame[124];
    buildSyntheticResponse(frame);
    frame[70] ^= 0xFF; // corrupt the oldest history entry

    TEST_ASSERT_FALSE(radian_checksum_ok(frame, 124));
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
    RUN_TEST(test_base64_encodes_known_vectors);
    RUN_TEST(test_base64_encodes_binary_bytes);
    RUN_TEST(test_base64_refuses_a_buffer_that_is_too_small);
    RUN_TEST(test_base64_response_capture_fits_the_publish_buffer);
    RUN_TEST(test_frame_length_is_bounded_by_the_declared_length);
    RUN_TEST(test_frame_length_never_exceeds_what_was_decoded);
    RUN_TEST(test_frame_length_falls_back_when_L_is_not_a_length);
    RUN_TEST(test_checksum_accepts_a_whole_frame);
    RUN_TEST(test_checksum_rejects_a_corrupted_frame);
    RUN_TEST(test_checksum_refuses_an_implausible_length);
    RUN_TEST(test_full_response_passes_length_and_checksum);
    RUN_TEST(test_full_response_rejected_when_a_payload_byte_flips);
    return UNITY_END();
}
