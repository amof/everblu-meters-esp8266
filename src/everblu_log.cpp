#include <stdarg.h>
#include <time.h>
#include "everblu_log.h"
#include "utils.h"

static char _lines[LOG_LINE_COUNT][LOG_LINE_MAX];
static uint8_t _head = 0;   // Next slot to write
static uint8_t _tail = 0;   // Oldest queued line
static uint8_t _count = 0;  // Lines currently queued
static uint8_t _stored = 0; // Lines present in _lines, delivered or not
static uint32_t _dropped = 0;
static LogSink _sink = NULL;
static bool _flushing = false;

// Flushing advances _tail but never erases the entries, so _lines is already a
// history of the last LOG_LINE_COUNT lines. The snapshot publishes that history
// as one retained message, so a client that was not connected when a line was
// produced can still see it. Only the joined text needs a buffer.
static char _snapshot[LOG_SNAPSHOT_MAX];
static LogSink _snapshotSink = NULL;
static bool _snapshotStale = false;

static CaptureSink _captureSink = NULL;
static char _capture[CAPTURE_B64_MAX];

void logSetSink(LogSink sink)
{
    _sink = sink;
}

void logSetCaptureSink(CaptureSink sink)
{
    _captureSink = sink;
}

void logCapture(const char *what, const uint8_t *data, size_t len)
{
    if (_captureSink == NULL || data == NULL || len == 0)
        return;

    size_t written = base64_encode(data, len, _capture, sizeof(_capture));
    if (written == 0)
    {
        // Encoding only refuses when the buffer is too small, which means the
        // capture is larger than anything the protocol should produce. Say so
        // rather than publishing a fragment that would decode to a wrong frame.
        logPrintf("[capture] %s too large to publish: %u bytes\n", what, (unsigned)len);
        return;
    }

    _captureSink(what, _capture);
}

void logSetSnapshotSink(LogSink sink)
{
    _snapshotSink = sink;
    // Publish once on connect so the topic exists even before anything new is
    // logged, carrying whatever history survived from before the connection.
    _snapshotStale = (_stored > 0);
}

/**
 * @brief Timestamp for a queued line, taken when the event happened.
 *
 * Lines are published from the main loop, not at the point of logging, so a
 * line produced inside a radio sequence can reach the broker seconds late.
 * Stamping at enqueue is what makes the captured log usable for timing.
 */
static void formatStamp(char *buf, size_t size)
{
    time_t now = time(NULL);

    // The ESP's clock starts at the epoch and only becomes meaningful once NTP
    // has run. Anything past 2020 means it has.
    if (now > 1577836800)
    {
        struct tm local;
        localtime_r(&now, &local);
        strftime(buf, size, "%Y-%m-%d %H:%M:%S", &local);
    }
    else
    {
        uint32_t ms = millis();
        snprintf(buf, size, "boot+%lu.%03lu",
                 (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    }
}

void logPrintf(const char *format, ...)
{
    char line[LOG_LINE_MAX];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    Serial.print(line);

    if (_sink == NULL)
        return;

    // Each queued entry is one message; the trailing newline is Serial's
    // formatting, not part of the content.
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    if (len == 0)
        return;

    // Prefer recent lines over old ones: during a sweep the tail of the log is
    // what explains the outcome.
    if (_count == LOG_LINE_COUNT)
    {
        _tail = (_tail + 1) % LOG_LINE_COUNT;
        _count--;
        _dropped++;
    }

    // Stamp first, then append as much of the message as still fits. Written in
    // two steps rather than one snprintf so the bound on the message is
    // explicit: a long line loses its tail, never the timestamp.
    char stamp[24];
    formatStamp(stamp, sizeof(stamp));
    int prefix = snprintf(_lines[_head], LOG_LINE_MAX, "%s ", stamp);
    if (prefix < 0 || prefix >= LOG_LINE_MAX)
        prefix = 0;
    strncpy(_lines[_head] + prefix, line, LOG_LINE_MAX - prefix - 1);
    _lines[_head][LOG_LINE_MAX - 1] = '\0';
    _head = (_head + 1) % LOG_LINE_COUNT;
    _count++;
    if (_stored < LOG_LINE_COUNT)
        _stored++;
    _snapshotStale = true;
}

/**
 * @brief Join the most recent stored lines into _snapshot, oldest first.
 *
 * Takes as many lines as fit in LOG_SNAPSHOT_MAX, newest end preserved: when
 * the history is too long to publish whole, the tail is what explains the
 * outcome, so the oldest lines are the ones dropped.
 */
static void buildSnapshot(void)
{
    // Walk backwards from the newest line to find how many will fit.
    uint8_t include = 0;
    size_t total = 0;

    while (include < _stored)
    {
        uint8_t idx = (_head + LOG_LINE_COUNT - 1 - include) % LOG_LINE_COUNT;
        size_t len = strlen(_lines[idx]) + 1; // + separating newline
        if (total + len >= LOG_SNAPSHOT_MAX)
            break;
        total += len;
        include++;
    }

    // Then emit them forward, so the snapshot reads in chronological order.
    size_t used = 0;
    for (uint8_t i = include; i > 0; i--)
    {
        uint8_t idx = (_head + LOG_LINE_COUNT - i) % LOG_LINE_COUNT;
        int written = snprintf(_snapshot + used, LOG_SNAPSHOT_MAX - used,
                               "%s\n", _lines[idx]);
        if (written < 0 || (size_t)written >= LOG_SNAPSHOT_MAX - used)
            break;
        used += written;
    }

    _snapshot[used] = '\0';
}

void logFlush(void)
{
    // A sink that logs would otherwise re-enter this loop while it is running.
    if (_flushing || _sink == NULL)
        return;

    _flushing = true;

    while (_count > 0)
    {
        _sink(_lines[_tail]);
        _tail = (_tail + 1) % LOG_LINE_COUNT;
        _count--;
    }

    if (_dropped > 0)
    {
        char stamp[24];
        char notice[96];
        formatStamp(stamp, sizeof(stamp));
        snprintf(notice, sizeof(notice), "%s [log] %u lines dropped", stamp, _dropped);
        _dropped = 0;
        _sink(notice);
    }

    // Republish only when the history actually changed: logFlush runs on every
    // loop iteration, and a retained publish per iteration would hammer the
    // broker for no reason.
    if (_snapshotStale && _snapshotSink != NULL)
    {
        buildSnapshot();
        _snapshotSink(_snapshot);
        _snapshotStale = false;
    }

    _flushing = false;
}
