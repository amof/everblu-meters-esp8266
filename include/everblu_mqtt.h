#ifndef EVERBLU_MQTT_H
#define EVERBLU_MQTT_H

// Generated from git before each build; see scripts/version.py. It is a string
// literal, so it concatenates straight into the discovery payloads below and
// they stay compile-time constants.
#include "version.h"

// constexpr gives these internal linkage, so this header stays safe to include
// from more than one translation unit. A plain `const char *` would not: only
// the pointee is const, so the pointer itself has external linkage.

// State topics - the device publishes, Home Assistant reads
constexpr const char *indexStateTopic = "everblu/cyble/index";
constexpr const char *batteryStateTopic = "everblu/cyble/battery";
constexpr const char *readingsStateTopic = "everblu/cyble/readings";
// The index as it stood at the end of last month, and everything else the meter
// says about itself: the other twelve monthly indexes, its own clock and its own
// serial. One sensor with attributes rather than thirteen entities, because
// "M-1" names a different month every month while the sensor does not.
constexpr const char *previousIndexStateTopic = "everblu/cyble/previous_index";
constexpr const char *meterAttributesTopic = "everblu/cyble/meter/attributes";
// The outcome of the most recent interrogation, or what one is doing right now.
// The full vocabulary, since it is spread across several call sites:
//   ok             a read completed and the values above were updated
//   reading        a read is in flight
//   sweeping       a full frequency sweep is in flight
//   busy           a request arrived while the radio was already in use
//   asleep         asked to read outside the meter's wakeup window
//   no_response    the meter did not answer on any frequency tried
//   not_provisioned  as above, but no frequency has ever been found
//   unreadable     the meter answered, but its response could not be decoded
//   gave_up        the day's attempt budget is spent; waiting for tomorrow
//   no_clock       asked to transmit before NTP had set the clock
constexpr const char *statusStateTopic = "everblu/cyble/status";
// When the last successful read completed, ISO 8601 in UTC. Distinct from the
// index's own timestamp, which Home Assistant sets on every publish including
// the retained one replayed at reconnect — this one only moves when the meter
// actually answered, which is what "is the reader still working" depends on.
constexpr const char *lastReadStateTopic = "everblu/cyble/last_read";
constexpr const char *scheduleTimeStateTopic = "everblu/cyble/schedule/time";
// The frequency a single-attempt test will use, in MHz. Settable, and separate
// from the working frequency in the meter profile: this one is aimed by hand.
constexpr const char *testFrequencyStateTopic = "everblu/cyble/test_frequency";
constexpr const char *testFrequencySetTopic = "everblu/cyble/test_frequency/set";
constexpr const char *testFrequencyCommandTopic = "everblu/cyble/command/test_frequency";
// Raw oversampled radio captures, base64, retained: one topic per frame so an
// ack does not overwrite the response it preceded. Retained because a capture
// is worth having when you next connect, not only if you were already watching.
constexpr const char *captureTopicPrefix = "everblu/cyble/capture/";
// Verdict of the wiring check: "ok", "spi_failed" or "gdo0_failed". Separate
// from status because the two have different lifetimes — status is the latest
// event, wiring fitness is sticky. Writing one into the other would destroy the
// last reading's outcome every time the check ran.
constexpr const char *wiringStateTopic = "everblu/cyble/wiring";
// PARTNUM/VERSION, as attributes of the entity above rather than an entity of
// their own: their only real use is as the evidence behind a "spi_failed"
// verdict, so they belong next to it.
constexpr const char *wiringAttributesTopic = "everblu/cyble/wiring/attributes";
// Debug log, one line per message. Not autodiscovered: it is far too chatty for
// Home Assistant's recorder, and a state value is capped at 255 characters.
// Follow it with: mosquitto_sub -t 'everblu/cyble/log' -v
constexpr const char *logTopic = "everblu/cyble/log";
// The same lines, joined and published retained, so a client that connects
// after the fact can still read recent history. The live topic above is not
// retained and is therefore invisible to anyone who was not already listening.
constexpr const char *logRecentTopic = "everblu/cyble/log/recent";
// Whether the device is alive, as opposed to what it is doing. Published
// retained on connect, and set to "offline" by the broker's last will if the
// device stops answering. Without it every entity keeps showing its last
// retained value forever, so a device that never came back from an OTA update
// still reads "ok" in Home Assistant.
constexpr const char *availabilityTopic = "everblu/cyble/availability";
// The defaults Home Assistant expects, so the discovery payloads do not have to
// override payload_available / payload_not_available.
constexpr const char *availabilityOnline = "online";
constexpr const char *availabilityOffline = "offline";

// Command topics - Home Assistant publishes, the device subscribes
constexpr const char *scheduleTimeSetTopic = "everblu/cyble/schedule/time/set";
constexpr const char *commandReadTopic = "everblu/cyble/command/read";
constexpr const char *commandSweepTopic = "everblu/cyble/command/sweep";
constexpr const char *commandWiringTopic = "everblu/cyble/command/wiring";

// Discovery topics
constexpr const char *indexConfigTopic = "homeassistant/sensor/everblucyble01a/index/config";
constexpr const char *batteryConfigTopic = "homeassistant/sensor/everblucyble01a/battery/config";
constexpr const char *readingsConfigTopic = "homeassistant/sensor/everblucyble01a/readings/config";
constexpr const char *statusConfigTopic = "homeassistant/sensor/everblucyble01a/status/config";
constexpr const char *lastReadConfigTopic = "homeassistant/sensor/everblucyble01a/last_read/config";
constexpr const char *scheduleTimeConfigTopic = "homeassistant/text/everblucyble01a/reading_time/config";
constexpr const char *buttonReadConfigTopic = "homeassistant/button/everblucyble01a/read/config";
constexpr const char *buttonSweepConfigTopic = "homeassistant/button/everblucyble01a/sweep/config";
constexpr const char *wiringConfigTopic = "homeassistant/sensor/everblucyble01a/wiring/config";
constexpr const char *buttonWiringConfigTopic = "homeassistant/button/everblucyble01a/wiring/config";
constexpr const char *previousIndexConfigTopic = "homeassistant/sensor/everblucyble01a/previous_index/config";
constexpr const char *testFrequencyConfigTopic = "homeassistant/number/everblucyble01a/test_frequency/config";
constexpr const char *buttonTestFrequencyConfigTopic = "homeassistant/button/everblucyble01a/test_frequency/config";

// Availability is per-entity in Home Assistant, so every payload has to carry
// this too — an entity without it is never greyed out, however dead the device.
#define EVERBLU_AVAILABILITY_JSON \
    "\"availability_topic\": \"everblu/cyble/availability\","

// Every entity must carry the same device block to be grouped under one device
// in Home Assistant.
#define EVERBLU_DEVICE_JSON                       \
    "\"device\" : {"                              \
    "\"identifiers\" : [ \"everblucyble01a\" ],"  \
    "\"name\": \"Everblu Cyble\","                \
    "\"model\": \"Everblu Cyble ESP8266/ESP32\"," \
    "\"manufacturer\": \"ITRON\","                \
    "\"sw_version\": \"" FIRMWARE_VERSION "\""    \
    "}"

// JSON payload for water meter index sensor
constexpr const char *indexConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Current Index\","
    "\"state_topic\": \"everblu/cyble/index\","
    "\"device_class\": \"water\","
    "\"state_class\": \"total_increasing\","
    "\"unique_id\": \"everblu_cyble_index\","
    "\"unit_of_measurement\": \"L\","
    "\"icon\": \"mdi:water\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// JSON payload for water meter battery sensor
constexpr const char *batteryConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Battery\","
    "\"unique_id\": \"everblu_cyble_battery\","
    "\"state_topic\": \"everblu/cyble/battery\","
    "\"unit_of_measurement\": \"months\","
    "\"icon\": \"mdi:battery\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// The meter's own count of how many times it has been interrogated. Diagnostic
// rather than primary: nobody wants it on a dashboard next to the water usage,
// but it is the one value that distinguishes "the meter never heard us" from
// "the meter answered and we failed to decode it".
constexpr const char *readingsConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Readings\","
    "\"unique_id\": \"everblu_cyble_readings\","
    "\"state_topic\": \"everblu/cyble/readings\","
    "\"entity_category\": \"diagnostic\","
    "\"icon\": \"mdi:counter\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// When the meter last actually answered. The reader is a once-a-day device, so
// a fault is invisible in the index itself — that just stops changing, which is
// also what a closed tap looks like. This is what an automation can alert on.
constexpr const char *lastReadConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Last Read\","
    "\"unique_id\": \"everblu_cyble_last_read\","
    "\"state_topic\": \"everblu/cyble/last_read\","
    "\"device_class\": \"timestamp\","
    "\"entity_category\": \"diagnostic\","
    "\"icon\": \"mdi:clock-check-outline\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// What the reader is currently doing, so a manual trigger gives visible feedback
constexpr const char *statusConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Status\","
    "\"unique_id\": \"everblu_cyble_status\","
    "\"state_topic\": \"everblu/cyble/status\","
    "\"icon\": \"mdi:information-outline\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Time of day at which the meter is interrogated automatically. A text entity
// rather than a number so hour and minute stay one field: "12:30".
// The pattern is enforced by Home Assistant before the value is ever published.
constexpr const char *scheduleTimeConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Reading Time\","
    "\"unique_id\": \"everblu_cyble_reading_time\","
    "\"command_topic\": \"everblu/cyble/schedule/time/set\","
    "\"state_topic\": \"everblu/cyble/schedule/time\","
    "\"pattern\": \"^([01][0-9]|2[0-3]):[0-5][0-9]$\","
    "\"min\": 5, \"max\": 5,"
    "\"entity_category\": \"config\","
    "\"icon\": \"mdi:clock-outline\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Read now, using the frequency already known
constexpr const char *buttonReadConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Read Now\","
    "\"unique_id\": \"everblu_cyble_read_now\","
    "\"command_topic\": \"everblu/cyble/command/read\","
    "\"icon\": \"mdi:refresh\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Whether the transceiver is wired up, as distinct from whether the radio
// works. The chip is named here rather than in the glossary term because this
// string is read in a list of unrelated entities, where "wiring" alone says
// nothing about what to go and re-seat.
constexpr const char *wiringConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble CC1101 Wiring\","
    "\"unique_id\": \"everblu_cyble_wiring\","
    "\"state_topic\": \"everblu/cyble/wiring\","
    "\"json_attributes_topic\": \"everblu/cyble/wiring/attributes\","
    "\"entity_category\": \"diagnostic\","
    "\"icon\": \"mdi:cable-data\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Re-run the wiring check, for someone standing at the device with a
// screwdriver who would rather not power-cycle it.
constexpr const char *buttonWiringConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Check Wiring\","
    "\"unique_id\": \"everblu_cyble_check_wiring\","
    "\"command_topic\": \"everblu/cyble/command/wiring\","
    "\"entity_category\": \"diagnostic\","
    "\"icon\": \"mdi:cable-data\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Forget the stored frequency and sweep for it again
constexpr const char *buttonSweepConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Full Sweep\","
    "\"unique_id\": \"everblu_cyble_full_sweep\","
    "\"command_topic\": \"everblu/cyble/command/sweep\","
    "\"entity_category\": \"config\","
    "\"icon\": \"mdi:radar\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Last month's closing index. total_increasing like the current index, since it
// is the same cumulative counter sampled a month earlier — not a consumption.
constexpr const char *previousIndexConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Previous Month Index\","
    "\"unique_id\": \"everblu_cyble_previous_index\","
    "\"state_topic\": \"everblu/cyble/previous_index\","
    "\"json_attributes_topic\": \"everblu/cyble/meter/attributes\","
    "\"device_class\": \"water\","
    "\"state_class\": \"total_increasing\","
    "\"unit_of_measurement\": \"L\","
    "\"icon\": \"mdi:calendar-arrow-left\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Aim a single attempt by hand. Bounds and step match the sweep's own band and
// 2kHz grid, so the entity cannot ask for a frequency the reader would refuse.
constexpr const char *testFrequencyConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Test Frequency\","
    "\"unique_id\": \"everblu_cyble_test_frequency\","
    "\"command_topic\": \"everblu/cyble/test_frequency/set\","
    "\"state_topic\": \"everblu/cyble/test_frequency\","
    "\"min\": 433.76, \"max\": 433.89, \"step\": 0.002,"
    "\"mode\": \"box\","
    "\"unit_of_measurement\": \"MHz\","
    "\"entity_category\": \"diagnostic\","
    "\"icon\": \"mdi:sine-wave\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// One exchange at the frequency above, never a sweep. A failed sweep costs the
// meter 61 wake-up preambles; this costs it one.
constexpr const char *buttonTestFrequencyConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Test Frequency Now\","
    "\"unique_id\": \"everblu_cyble_test_frequency_now\","
    "\"command_topic\": \"everblu/cyble/command/test_frequency\","
    "\"entity_category\": \"diagnostic\","
    "\"icon\": \"mdi:target-variant\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

#endif // EVERBLU_MQTT_H
