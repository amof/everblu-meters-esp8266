#ifndef __EVERBLU_LOG_H__
#define __EVERBLU_LOG_H__

#include <Arduino.h>

// Log lines are queued rather than published as they are produced. Publishing
// performs network I/O, and much of what we log happens inside timing-sensitive
// radio sequences where a stall would break the exchange. The queue is drained
// at points the caller declares safe.
#define LOG_LINE_MAX 128
// One full exchange now logs around 38 lines — capture, decode verdicts, every
// decoded field and the monthly history — and none of them can be flushed until
// the radio sequence returns to the main loop. At 24 the queue silently dropped
// the earliest, which is where the decode verdicts are.
#define LOG_LINE_COUNT 48

// Size of the joined history published as one retained message. Must leave room
// for the topic and MQTT header inside the client's maximum packet size, which
// the sketch raises to 1024.
#define LOG_SNAPSHOT_MAX 900

// A raw radio capture is far too large for the line log: a 748-byte response is
// 2244 characters as hex, against a 128-character line. Base64 brings it to
// 1000, so captures go out whole on their own topic instead of being chunked
// across the log. The sketch raises the MQTT packet limit to suit.
#define CAPTURE_MAX_BYTES 768
#define CAPTURE_B64_MAX (((CAPTURE_MAX_BYTES + 2) / 3) * 4 + 1)

typedef void (*LogSink)(const char *line);

/** @brief Where a base64 capture is delivered, tagged with what it is. */
typedef void (*CaptureSink)(const char *what, const char *base64);

/**
 * @brief Where flushed log lines are delivered, e.g. an MQTT publish.
 *        Until a sink is set, lines go to Serial only and nothing is queued.
 */
void logSetSink(LogSink sink);

/**
 * @brief Where the retained history snapshot is delivered.
 *
 * The live sink only reaches clients connected at the moment a line is
 * produced. This one is handed the last few lines joined together, whenever
 * they change, so it can be published retained — letting a client that
 * connects later still see what happened. Optional; unset means no snapshot.
 */
void logSetSnapshotSink(LogSink sink);

/**
 * @brief Where raw captures are delivered. Optional; unset means captures are
 *        discarded rather than queued, since nothing else can carry them.
 */
void logSetCaptureSink(CaptureSink sink);

/**
 * @brief Publish a raw radio capture, base64-encoded.
 *
 * Unlike logPrintf this performs I/O through the sink immediately, so it must
 * only be called once an exchange has closed — never between a meter ack and
 * the response that follows it.
 */
void logCapture(const char *what, const uint8_t *data, size_t len);

/**
 * @brief Write a line to Serial and queue it for the sink.
 *        Safe to call from timing-critical code: it never does I/O beyond Serial.
 *
 * Queued lines are prefixed with the local time at which they were logged, so a
 * captured log reflects when things happened rather than when they were
 * published. Serial output is left unprefixed: some callers build one line from
 * several calls, which a per-call stamp would break.
 */
void logPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Deliver queued lines to the sink. Call only where a stall is tolerable.
 *        Re-entrant calls are ignored, so a sink may itself log without looping.
 */
void logFlush(void);

#define LOG(...) logPrintf(__VA_ARGS__)

#endif // __EVERBLU_LOG_H__
