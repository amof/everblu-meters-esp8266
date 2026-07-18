#ifndef __EVERBLU_LOG_H__
#define __EVERBLU_LOG_H__

#include <Arduino.h>

// Log lines are queued rather than published as they are produced. Publishing
// performs network I/O, and much of what we log happens inside timing-sensitive
// radio sequences where a stall would break the exchange. The queue is drained
// at points the caller declares safe.
#define LOG_LINE_MAX 128
#define LOG_LINE_COUNT 24

typedef void (*LogSink)(const char *line);

/**
 * @brief Where flushed log lines are delivered, e.g. an MQTT publish.
 *        Until a sink is set, lines go to Serial only and nothing is queued.
 */
void logSetSink(LogSink sink);

/**
 * @brief Write a line to Serial and queue it for the sink.
 *        Safe to call from timing-critical code: it never does I/O beyond Serial.
 */
void logPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * @brief Deliver queued lines to the sink. Call only where a stall is tolerable.
 *        Re-entrant calls are ignored, so a sink may itself log without looping.
 */
void logFlush(void);

#define LOG(...) logPrintf(__VA_ARGS__)

#endif // __EVERBLU_LOG_H__
