# everblu-meters - Water usage data for Home Assistant

Fetch water/gas usage data from Cyble EverBlu meters using RADIAN protocol on 433Mhz. Integrated with Home Assistant via MQTT.
This fork is based on:

- <http://www.lamaisonsimon.fr/wiki/doku.php?id=maison2:compteur_d_eau:compteur_d_eau>
- <https://github.com/psykokwak-com/everblu-meters-esp8266>

Meters supported:

- Itron EverBlu Cyble Enhanced

## Features

- Completely rewritten code of cc1101 to improve reliability and understanding
- Auto-discovery of water meter frequency and record it in EEPROM
- HASS auto-discovery
- Deep-sleep mode
- CC1101 frequency autocalibration & compensation
- Debugging much more easier (with serial link)
- Time synchronization using *configTime* to avoid buggy Arduino library

## Build & configuration

### Dependencies

- EspMQTTClient library <https://github.com/plapointe6/EspMQTTClient> (by Patrick Lapointe) version **1.13.3**

### Configuration

In the file *everblu-meters-esp8266.ino*, adapt those to your needs:

```c
const char *NtpServer = "myNtpServer";
const char *TZstr = "UTC+0,M3.5.0,M10.5.0/3"; // https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html

// User defined
EspMQTTClient client(
  "WifiSSID",
  "WifiPassword",
  "myMqttServer",   // MQTT Broker server
  "MQTTUsername",   // Can be omitted if not needed
  "MQTTPassword",   // Can be omitted if not needed
  "TestClient"      // Client name that uniquely identify your device
);

EverbluCyble cyble(
  0,            // GDO0 pin
  yy,           // Year
  0123456       // Serial without leading zero
  );
```
