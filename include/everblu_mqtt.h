#ifndef EVERBLU_MQTT_H
#define EVERBLU_MQTT_H

// MQTT topics for discovery and data
const char *indexConfigTopic = "homeassistant/sensor/everblucyble01a/index/config";
const char *batteryConfigTopic = "homeassistant/sensor/everblucyble01a/battery/config";
const char *counterConfigTopic = "homeassistant/sensor/everblucyble01a/readings/config";
const char *litersStateTopic = "everblu/cyble/liters";
const char *batteryStateTopic = "everblu/cyble/battery";
const char *counterStateTopic = "everblu/cyble/num_readings";

// JSON payload for water meter index sensor
const char *currentIndexConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Current Index\","
    "\"state_topic\": \"everblu/cyble/liters\","
    "\"device_class\": \"water\","
    "\"unique_id\": \"everblu_cyble_index\","
    "\"unit_of_measurement\": \"L\","
    "\"icon\": \"mdi:water\","
    "\"device\" : {"
    "\"identifiers\" : ["
    "\"everblucyble01a\" ],"
    "\"name\": \"Everblu Cyble\","
    "\"model\": \"Everblu Cyble ESP8266/ESP32\","
    "\"manufacturer\": \"ITRON\""
    "}"
    "}";

// JSON payload for water meter battery sensor
const char *batteryConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Battery\","
    "\"unique_id\": \"water_meter_battery\","
    "\"state_topic\": \"everblu/cyble/battery\","
    "\"unit_of_measurement\": \"%\","
    "\"device_class\": \"battery\","
    "\"icon\": \"mdi:battery\","
    "\"device\" : {"
    "\"identifiers\" : ["
    "\"everblucyble01a\" ],"
    "\"name\": \"Everblu Cyble\","
    "\"model\": \"Everblu Cyble ESP8266/ESP32\","
    "\"manufacturer\": \"ITRON\""
    "}"
    "}";

// JSON payload for water meter counter sensor
const char *numReadingsConfigPayload =
    "{"
    "\"name\": \"Everblu Cyble Num Readings\","
    "\"unique_id\": \"water_meter_readings\","
    "\"state_topic\": \"everblu/cyble/num_readings\","
    "\"icon\": \"mdi:counter\","
    "\"device\" : {"
    "\"identifiers\" : ["
    "\"everblucyble01a\" ],"
    "\"name\": \"Everblu Cyble\","
    "\"model\": \"Everblu Cyble ESP8266/ESP32\","
    "\"manufacturer\": \"ITRON\""
    "}"
    "}";

#endif // EVERBLU_MQTT_H