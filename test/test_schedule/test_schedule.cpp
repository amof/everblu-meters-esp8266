// The daily-reading schedule: when a tick counts as an attempt, and when it is
// simply not due.
//
// The distinction matters beyond the radio. The caller publishes the result of
// every tick it is told was performed, so a tick misclassified as an attempt
// costs a log line and an MQTT status republish once a minute for as long as
// the misclassification lasts — which, for the Sunday case below, is a whole
// day. See docs/adr/0002-sweep-is-crystal-calibration-not-meter-discovery.md
// for why transmitting outside the window cannot succeed in the first place.

#include <unity.h>
#include <string.h>
#include <time.h>
#include <Arduino.h>
#include <SPI.h>
#include <EEPROM.h>
#include <everblu_cyble.h>

FakeSerial Serial;
FakeSPI SPI;

#define TEST_GDO0_PIN 5
#define TEST_METER_YEAR 20
#define TEST_METER_SERIAL 123456

// Well inside the default 06:00-18:00 window, so a test that moves the clock
// out of the window is doing so deliberately rather than by accident.
#define SCHEDULE_HOUR 12
#define SCHEDULE_MINUTE 0

void setUp(void)
{
    fakeChip().reset();
    fakeMillis() = 0;
    EEPROM.reset();
}

void tearDown(void) {}

/**
 * A local wall-clock time as time_t.
 *
 * Built through mktime() rather than from a fixed epoch value, so the tests
 * assert the same wall-clock behaviour whatever timezone the desktop running
 * them is in. The code under test reads local time; hard-coding UTC offsets
 * here would make the suite pass or fail on the tester's location.
 */
static time_t localTime(int year, int month, int day, int hour, int minute)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_isdst = -1; // Let the C library decide; the dates below straddle a DST change.
    return mktime(&t);
}

/** Weekday of a time_t, 0 = Sunday, matching struct tm. */
static int weekdayOf(time_t when)
{
    struct tm local;
    memset(&local, 0, sizeof(local));
    localtime_r(&when, &local);
    return local.tm_wday;
}

/** Fresh reader over erased EEPROM, initialised as the sketch does. */
static EverbluCyble *bringUp()
{
    static EverbluCyble *reader = NULL;
    delete reader;
    reader = new EverbluCyble(TEST_GDO0_PIN, TEST_METER_YEAR, TEST_METER_SERIAL);
    reader->init();
    reader->setScheduledTime(SCHEDULE_HOUR, SCHEDULE_MINUTE);
    return reader;
}

// 2026-07-19 is the Sunday from the report that prompted these tests; the days
// around it are the corresponding weekdays.
#define SUNDAY 2026, 7, 19
#define MONDAY 2026, 7, 20
#define SATURDAY 2026, 7, 18

void test_the_dates_these_tests_assume(void)
{
    // Every case below is named after a weekday. If the calendar arithmetic is
    // wrong, those tests still pass while asserting something else entirely,
    // so the assumption is checked rather than trusted.
    TEST_ASSERT_EQUAL_INT(0, weekdayOf(localTime(SUNDAY, 12, 0)));
    TEST_ASSERT_EQUAL_INT(1, weekdayOf(localTime(MONDAY, 12, 0)));
    TEST_ASSERT_EQUAL_INT(6, weekdayOf(localTime(SATURDAY, 12, 0)));
}

void test_meter_is_deaf_on_sunday(void)
{
    EverbluCyble *reader = bringUp();
    TEST_ASSERT_FALSE(reader->isMeterAwake(localTime(SUNDAY, 12, 0)));
}

void test_meter_is_awake_midweek_inside_the_window(void)
{
    EverbluCyble *reader = bringUp();
    TEST_ASSERT_TRUE(reader->isMeterAwake(localTime(MONDAY, 12, 0)));
}

void test_saturday_is_a_working_day(void)
{
    // Only Sunday is excluded. Treating the whole weekend as dead would lose a
    // day's reading every week.
    EverbluCyble *reader = bringUp();
    TEST_ASSERT_TRUE(reader->isMeterAwake(localTime(SATURDAY, 12, 0)));
}

void test_window_edges(void)
{
    EverbluCyble *reader = bringUp();
    // Start hour is inclusive, stop hour exclusive: at 18:00 the meter has
    // already stopped answering.
    TEST_ASSERT_FALSE(reader->isMeterAwake(localTime(MONDAY, 5, 59)));
    TEST_ASSERT_TRUE(reader->isMeterAwake(localTime(MONDAY, 6, 0)));
    TEST_ASSERT_TRUE(reader->isMeterAwake(localTime(MONDAY, 17, 59)));
    TEST_ASSERT_FALSE(reader->isMeterAwake(localTime(MONDAY, 18, 0)));
}

void test_nothing_is_attempted_before_the_scheduled_time(void)
{
    EverbluCyble *reader = bringUp();
    bool performed = true;
    reader->readIfDue(localTime(MONDAY, SCHEDULE_HOUR - 1, 0), &performed);
    TEST_ASSERT_FALSE(performed);
}

/**
 * Bring up a reader that already knows its meter's frequency.
 *
 * The record is seeded straight into flash, so init() sees exactly what a
 * reboot sees. Magic and schema are duplicated from everblu_cyble.cpp; if this
 * stops compiling or the reader comes up unprovisioned, the on-flash layout
 * changed and this test wants to know.
 */
static EverbluCyble *bringUpProvisioned(float frequency)
{
    MeterProfile stored;
    memset(&stored, 0, sizeof(stored));
    stored.magic = 0x45564231UL; // "EVB1"
    stored.schema = 1;
    stored.frequency = frequency;
    stored.wakeupStart = 6;
    stored.wakeupStop = 18;

    EEPROM.begin(64);
    EEPROM.put(0, stored);
    EEPROM.commit();

    static EverbluCyble *reader = NULL;
    delete reader;
    reader = new EverbluCyble(TEST_GDO0_PIN, TEST_METER_YEAR, TEST_METER_SERIAL);
    reader->init();
    reader->setScheduledTime(SCHEDULE_HOUR, SCHEDULE_MINUTE);
    return reader;
}

/**
 * A failed automatic read must not escalate to a full sweep.
 *
 * Every frequency tried costs the meter a ~2 second wake-up preamble, and the
 * meter is battery powered and answers a limited number of times. Escalating
 * cost 69 preambles per failed read against a budget of five reads a day: about
 * twelve minutes of forced wake-ups daily, enough to stop a meter answering at
 * all — so the recovery attempt became the thing preventing recovery.
 *
 * A provisioned reader may try its stored frequency and the re-centring sweep
 * that tracks crystal drift, and nothing more. Per ADR-0002 the full sweep
 * exists for crystal error, which drifts slowly; a reader that hears nothing
 * across seven frequencies has a silent meter, not a crystal that moved 60kHz.
 */
void test_a_failed_automatic_read_does_not_escalate_to_a_full_sweep(void)
{
    EverbluCyble *reader = bringUpProvisioned(433.79f);
    fakeChip().frequencyWrites = 0;

    bool performed = false;
    MeterReadResult result = reader->readIfDue(localTime(MONDAY, SCHEDULE_HOUR, 0), &performed);

    TEST_ASSERT_TRUE(performed);
    TEST_ASSERT_EQUAL(METER_NO_RESPONSE, result);

    // 1 at the stored frequency, then +/-3 steps either side: 1 + 7 = 8.
    // A full sweep would be 61 more.
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(8, fakeChip().frequencyWrites);
}

/** The full sweep is still available when asked for by hand. */
void test_a_requested_sweep_still_searches_the_whole_band(void)
{
    EverbluCyble *reader = bringUpProvisioned(433.79f);
    fakeChip().frequencyWrites = 0;

    reader->sweepForMeter(localTime(MONDAY, SCHEDULE_HOUR, 0));

    // Far more than the 8 an automatic read is allowed to spend.
    TEST_ASSERT_GREATER_THAN_UINT16(50, fakeChip().frequencyWrites);
}

void test_a_due_reading_is_attempted(void)
{
    // The baseline the rest of the file is defined against: without this, every
    // "not performed" assertion below would pass for a reader that never reads.
    EverbluCyble *reader = bringUp();
    bool performed = false;
    MeterReadResult result = reader->readIfDue(localTime(MONDAY, SCHEDULE_HOUR, 0), &performed);

    TEST_ASSERT_TRUE(performed);
    // The chip model does not answer as a meter, so the sweep runs and finds
    // nothing. What matters here is that the radio was reached at all.
    TEST_ASSERT_EQUAL(METER_NO_RESPONSE, result);
}

void test_sunday_ticks_are_not_attempts(void)
{
    // The regression. Sunday satisfies both scheduling conditions — the time
    // has passed and the day is unread — so before the wakeup check moved
    // ahead of it, every tick was reported as an attempt that then refused
    // itself, all day long.
    EverbluCyble *reader = bringUp();

    for (int minute = 0; minute < 10; minute++)
    {
        bool performed = true;
        MeterReadResult result = reader->readIfDue(localTime(SUNDAY, 13, minute), &performed);
        TEST_ASSERT_FALSE(performed);
        TEST_ASSERT_EQUAL(METER_READ_OK, result);
    }
}

void test_ticks_after_the_window_closes_are_not_attempts(void)
{
    // The same bug, six days a week: once the window shuts on a day that was
    // never successfully read, the reading stays due until midnight.
    EverbluCyble *reader = bringUp();
    bool performed = true;
    reader->readIfDue(localTime(MONDAY, 19, 30), &performed);
    TEST_ASSERT_FALSE(performed);
}

void test_a_reading_missed_on_sunday_is_attempted_on_monday(void)
{
    // Suppressing the Sunday ticks must not also cancel the reading. Skipping
    // and forgetting look identical from inside a single tick, and only this
    // distinguishes them.
    EverbluCyble *reader = bringUp();

    bool performed = true;
    reader->readIfDue(localTime(SUNDAY, 13, 0), &performed);
    TEST_ASSERT_FALSE(performed);

    performed = false;
    reader->readIfDue(localTime(MONDAY, SCHEDULE_HOUR, 0), &performed);
    TEST_ASSERT_TRUE(performed);
}

void test_a_reading_due_before_the_window_opens_waits_for_it(void)
{
    // A schedule can legitimately be set outside the window. The reading is
    // then due from midnight, but must wait quietly rather than hammering the
    // radio for six hours.
    EverbluCyble *reader = bringUp();
    reader->setScheduledTime(3, 0);

    bool performed = true;
    reader->readIfDue(localTime(MONDAY, 4, 0), &performed);
    TEST_ASSERT_FALSE(performed);

    performed = false;
    reader->readIfDue(localTime(MONDAY, 6, 0), &performed);
    TEST_ASSERT_TRUE(performed);
}

// Must match MAX_READ_ATTEMPTS_PER_DAY in everblu_cyble.cpp. Duplicated rather
// than exported, so the tests pin the documented number instead of agreeing
// with whatever the source currently says.
#define EXPECTED_MAX_ATTEMPTS 5

/** Tick every 5 minutes from the scheduled hour, as the sketch does. */
static int attemptsOverAfternoon(EverbluCyble *reader, int year, int month, int day)
{
    int attempts = 0;
    for (int tick = 0; tick < 60; tick++)
    {
        bool performed = false;
        int minutes = tick * 5;
        reader->readIfDue(localTime(year, month, day, SCHEDULE_HOUR + (minutes / 60), minutes % 60),
                          &performed);
        if (performed)
            attempts++;
    }
    return attempts;
}

void test_a_failing_meter_is_given_up_on_after_the_budget(void)
{
    // The chip model never answers, so every attempt fails. Without a budget
    // this transmits a full sweep on every tick until the window closes.
    EverbluCyble *reader = bringUp();
    TEST_ASSERT_EQUAL_INT(EXPECTED_MAX_ATTEMPTS, attemptsOverAfternoon(reader, MONDAY));
    TEST_ASSERT_EQUAL_UINT8(EXPECTED_MAX_ATTEMPTS, reader->attemptsToday());
}

void test_the_budget_resets_the_next_day(void)
{
    // Giving up must be for the day, not for good. A budget that never reset
    // would silently retire the device after one bad afternoon.
    EverbluCyble *reader = bringUp();
    attemptsOverAfternoon(reader, SATURDAY);
    TEST_ASSERT_EQUAL_UINT8(EXPECTED_MAX_ATTEMPTS, reader->attemptsToday());

    // Sunday is skipped entirely, so Monday is the next day that can attempt.
    bool performed = false;
    reader->readIfDue(localTime(MONDAY, SCHEDULE_HOUR, 0), &performed);
    TEST_ASSERT_TRUE(performed);
    TEST_ASSERT_EQUAL_UINT8(1, reader->attemptsToday());
}

void test_ticks_after_the_budget_are_not_attempts(void)
{
    // The budget must suppress the tick the same way the wakeup window does:
    // silently, without reporting a refusal the caller would publish.
    EverbluCyble *reader = bringUp();
    attemptsOverAfternoon(reader, MONDAY);

    bool performed = true;
    MeterReadResult result = reader->readIfDue(localTime(MONDAY, 17, 0), &performed);
    TEST_ASSERT_FALSE(performed);
    TEST_ASSERT_EQUAL(METER_READ_OK, result);
}

void test_sunday_ticks_do_not_consume_the_budget(void)
{
    // Ordering check: the wakeup-window test must come first. If the budget
    // were charged before it, a Sunday would burn the whole allowance and
    // Monday would start already exhausted.
    EverbluCyble *reader = bringUp();

    for (int tick = 0; tick < 20; tick++)
    {
        bool performed = false;
        reader->readIfDue(localTime(SUNDAY, 13, tick), &performed);
    }
    TEST_ASSERT_EQUAL_UINT8(0, reader->attemptsToday());

    bool performed = false;
    reader->readIfDue(localTime(MONDAY, SCHEDULE_HOUR, 0), &performed);
    TEST_ASSERT_TRUE(performed);
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_dates_these_tests_assume);
    RUN_TEST(test_meter_is_deaf_on_sunday);
    RUN_TEST(test_meter_is_awake_midweek_inside_the_window);
    RUN_TEST(test_saturday_is_a_working_day);
    RUN_TEST(test_window_edges);
    RUN_TEST(test_nothing_is_attempted_before_the_scheduled_time);
    RUN_TEST(test_a_due_reading_is_attempted);
    RUN_TEST(test_a_failed_automatic_read_does_not_escalate_to_a_full_sweep);
    RUN_TEST(test_a_requested_sweep_still_searches_the_whole_band);
    RUN_TEST(test_sunday_ticks_are_not_attempts);
    RUN_TEST(test_ticks_after_the_window_closes_are_not_attempts);
    RUN_TEST(test_a_reading_missed_on_sunday_is_attempted_on_monday);
    RUN_TEST(test_a_reading_due_before_the_window_opens_waits_for_it);
    RUN_TEST(test_a_failing_meter_is_given_up_on_after_the_budget);
    RUN_TEST(test_the_budget_resets_the_next_day);
    RUN_TEST(test_ticks_after_the_budget_are_not_attempts);
    RUN_TEST(test_sunday_ticks_do_not_consume_the_budget);
    return UNITY_END();
}
