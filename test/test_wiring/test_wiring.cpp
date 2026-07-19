// The wiring check, driven against a chip model rather than a chip.
//
// The physical wire is the *input* here, not the thing under test: the model
// says how GDO0 is connected, and these assert that the check reaches the right
// verdict and leaves the radio in a state the next interrogation can use.
//
// See docs/adr/0005-the-wiring-check-never-transmits-and-never-blocks.md.

#include <unity.h>
#include <string.h>
#include <Arduino.h>
#include <SPI.h>
#include <cc1101.h>
#include <cc1101_registers.h>

FakeSerial Serial;
FakeSPI SPI;

// Any pin that is not MISO; the model routes every other read to GDO0.
#define TEST_GDO0_PIN 5

void setUp(void)
{
    fakeChip().reset();
    fakeMillis() = 0;
}

void tearDown(void) {}

/** Fresh driver over a freshly reset model, initialised as the sketch does. */
static CC1101 *bringUp()
{
    static CC1101 *radio = NULL;
    delete radio;
    radio = new CC1101(TEST_GDO0_PIN);
    radio->init();
    return radio;
}

void test_correctly_wired_board_passes(void)
{
    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_OK, radio->checkWiring());
}

void test_identity_is_reported_alongside_the_verdict(void)
{
    CC1101 *radio = bringUp();
    radio->checkWiring();

    ChipIdentity id = radio->identity();
    TEST_ASSERT_EQUAL_HEX8(CC1101_EXPECTED_PARTNUM, id.partNumber);
    TEST_ASSERT_EQUAL_HEX8(CC1101_EXPECTED_VERSION, id.version);
}

void test_dead_spi_is_reported_as_spi_not_gdo0(void)
{
    fakeChip().spiConnected = false;

    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_SPI_FAILED, radio->checkWiring());
}

void test_a_chip_that_is_not_a_cc1101_fails_the_spi_test(void)
{
    // Everything responds, but with the wrong identity — a different part on
    // the bus, or MISO picking up something else entirely.
    fakeChip().version = 0x03;

    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_SPI_FAILED, radio->checkWiring());
}

void test_gdo0_never_tested_when_spi_is_dead(void)
{
    // With SPI dead, GDO0 would be read through its own pull-up and nothing
    // else. Claiming a GDO0 verdict there would be inventing a result.
    fakeChip().spiConnected = false;
    fakeChip().gdo0 = GDO0_DISCONNECTED;

    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_SPI_FAILED, radio->checkWiring());
}

void test_disconnected_gdo0_is_caught(void)
{
    // The failure this whole feature exists for. The pull-up holds the pin
    // high, so it passes a naive "can I see a high" test and only fails when
    // the chip is asked for a low.
    fakeChip().gdo0 = GDO0_DISCONNECTED;

    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_GDO0_FAILED, radio->checkWiring());
}

void test_gdo0_shorted_to_ground_is_caught(void)
{
    // The mirror image: a permanently-low pin satisfies the drive-low step and
    // is only caught by asking for a high.
    fakeChip().gdo0 = GDO0_SHORTED_LOW;

    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_GDO0_FAILED, radio->checkWiring());
}

void test_iocfg0_is_restored_after_a_pass(void)
{
    CC1101 *radio = bringUp();
    radio->checkWiring();

    TEST_ASSERT_EQUAL_HEX8(IOCFG_GDO0_SYNC, fakeChip().regs[IOCFG0]);
}

void test_iocfg0_is_restored_after_a_gdo0_failure(void)
{
    // The path that matters most: leaving IOCFG0 on a constant level here would
    // break every subsequent interrogation, by way of the diagnostic that was
    // meant to protect them.
    fakeChip().gdo0 = GDO0_DISCONNECTED;

    CC1101 *radio = bringUp();
    radio->checkWiring();

    TEST_ASSERT_EQUAL_HEX8(IOCFG_GDO0_SYNC, fakeChip().regs[IOCFG0]);
}

void test_both_polarities_are_actually_driven(void)
{
    // Asserting the restored value alone would pass for a check that never
    // touched IOCFG0 at all. The sequence is what proves both halves ran.
    CC1101 *radio = bringUp();
    fakeChip().iocfg0WriteCount = 0;
    radio->checkWiring();

    TEST_ASSERT_EQUAL_UINT8(3, fakeChip().iocfg0WriteCount);
    TEST_ASSERT_EQUAL_HEX8(IOCFG_GDO_HW_TO_0, fakeChip().iocfg0Writes[0]);
    TEST_ASSERT_EQUAL_HEX8(IOCFG_GDO_HW_TO_0 | IOCFG_GDO_INV, fakeChip().iocfg0Writes[1]);
    TEST_ASSERT_EQUAL_HEX8(IOCFG_GDO0_SYNC, fakeChip().iocfg0Writes[2]);
}

void test_check_is_repeatable(void)
{
    // The button re-runs it in place. A check that only worked once — because
    // it depended on the post-reset register state — would pass at boot and
    // mislead the person standing at the device with a screwdriver.
    CC1101 *radio = bringUp();
    TEST_ASSERT_EQUAL(WIRING_OK, radio->checkWiring());
    TEST_ASSERT_EQUAL(WIRING_OK, radio->checkWiring());

    fakeChip().gdo0 = GDO0_DISCONNECTED;
    TEST_ASSERT_EQUAL(WIRING_GDO0_FAILED, radio->checkWiring());

    fakeChip().gdo0 = GDO0_CONNECTED;
    TEST_ASSERT_EQUAL(WIRING_OK, radio->checkWiring());
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_correctly_wired_board_passes);
    RUN_TEST(test_identity_is_reported_alongside_the_verdict);
    RUN_TEST(test_dead_spi_is_reported_as_spi_not_gdo0);
    RUN_TEST(test_a_chip_that_is_not_a_cc1101_fails_the_spi_test);
    RUN_TEST(test_gdo0_never_tested_when_spi_is_dead);
    RUN_TEST(test_disconnected_gdo0_is_caught);
    RUN_TEST(test_gdo0_shorted_to_ground_is_caught);
    RUN_TEST(test_iocfg0_is_restored_after_a_pass);
    RUN_TEST(test_iocfg0_is_restored_after_a_gdo0_failure);
    RUN_TEST(test_both_polarities_are_actually_driven);
    RUN_TEST(test_check_is_repeatable);
    return UNITY_END();
}
