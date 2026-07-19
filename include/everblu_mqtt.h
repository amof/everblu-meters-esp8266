#ifndef EVERBLU_MQTT_H
#define EVERBLU_MQTT_H

// constexpr gives these internal linkage, so this header stays safe to include
// from more than one translation unit. A plain `const char *` would not: only
// the pointee is const, so the pointer itself has external linkage.

// State topics - the device publishes, Home Assistant reads
constexpr const char *litersStateTopic = "everblu/cyble/liters";
constexpr const char *batteryStateTopic = "everblu/cyble/battery";
constexpr const char *counterStateTopic = "everblu/cyble/num_readings";
constexpr const char *statusStateTopic = "everblu/cyble/status";
constexpr const char *scheduleTimeStateTopic = "everblu/cyble/schedule/time";
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
constexpr const char *commandScanTopic = "everblu/cyble/command/scan";

// Discovery topics
constexpr const char *indexConfigTopic = "homeassistant/sensor/everblucyble01a/index/config";
constexpr const char *batteryConfigTopic = "homeassistant/sensor/everblucyble01a/battery/config";
constexpr const char *counterConfigTopic = "homeassistant/sensor/everblucyble01a/readings/config";
constexpr const char *statusConfigTopic = "homeassistant/sensor/everblucyble01a/status/config";
constexpr const char *scheduleTimeConfigTopic = "homeassistant/text/everblucyble01a/time/config";
constexpr const char *buttonReadConfigTopic = "homeassistant/button/everblucyble01a/read/config";
constexpr const char *buttonScanConfigTopic = "homeassistant/button/everblucyble01a/scan/config";

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
    "\"manufacturer\": \"ITRON\""                 \
    "}"

// JSON payload for water meter index sensor
constexpr const char *currentIndexConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Current Index\","
    "\"state_topic\": \"everblu/cyble/liters\","
    "\"device_class\": \"water\","
    "\"state_class\": \"total_increasing\","
    "\"unique_id\": \"everblu_cyble_index\","
    "\"unit_of_measurement\": \"L\","
    "\"icon\": \"mdi:water\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// JSON payload for water meter battery sensor
constexpr const char *batteryConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Battery\","
    "\"unique_id\": \"water_meter_battery\","
    "\"state_topic\": \"everblu/cyble/battery\","
    "\"unit_of_measurement\": \"months\","
    "\"icon\": \"mdi:battery\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// JSON payload for water meter counter sensor
constexpr const char *numReadingsConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Num Readings\","
    "\"unique_id\": \"water_meter_readings\","
    "\"state_topic\": \"everblu/cyble/num_readings\","
    "\"icon\": \"mdi:counter\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// What the reader is currently doing, so a manual trigger gives visible feedback
constexpr const char *statusConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Status\","
    "\"unique_id\": \"water_meter_status\","
    "\"state_topic\": \"everblu/cyble/status\","
    "\"icon\": \"mdi:information-outline\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Time of day at which the meter is interrogated automatically. A text entity
// rather than a number so hour and minute stay one field: "12:30".
// The pattern is enforced by Home Assistant before the value is ever published.
constexpr const char *scheduleTimeConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Reading Time\","
    "\"unique_id\": \"water_meter_time\","
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
    "\"unique_id\": \"water_meter_read_now\","
    "\"command_topic\": \"everblu/cyble/command/read\","
    "\"icon\": \"mdi:refresh\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

// Forget the stored frequency and sweep for it again
constexpr const char *buttonScanConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Full Scan\","
    "\"unique_id\": \"water_meter_full_scan\","
    "\"command_topic\": \"everblu/cyble/command/scan\","
    "\"entity_category\": \"config\","
    "\"icon\": \"mdi:radar\"," EVERBLU_AVAILABILITY_JSON EVERBLU_DEVICE_JSON "}";

#endif // EVERBLU_MQTT_H
