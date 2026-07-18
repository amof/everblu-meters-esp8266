#include <stdarg.h>
#include "everblu_log.h"

static char _lines[LOG_LINE_COUNT][LOG_LINE_MAX];
static uint8_t _head = 0;  // Next slot to write
static uint8_t _tail = 0;  // Oldest queued line
static uint8_t _count = 0; // Lines currently queued
static uint32_t _dropped = 0;
static LogSink _sink = NULL;
static bool _flushing = false;

void logSetSink(LogSink sink)
{
    _sink = sink;
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

    strncpy(_lines[_head], line, LOG_LINE_MAX - 1);
    _lines[_head][LOG_LINE_MAX - 1] = '\0';
    _head = (_head + 1) % LOG_LINE_COUNT;
    _count++;
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
        char notice[64];
        snprintf(notice, sizeof(notice), "[log] %u lines dropped", _dropped);
        _dropped = 0;
        _sink(notice);
    }

    _flushing = false;
}
