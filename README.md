# everblu-meters - Water usage data for Home Assistant

Fetch water/gas usage data from Cyble EverBlu meters using RADIAN protocol on 433Mhz. Integrated with Home Assistant via MQTT.
This fork is based on:

- <http://www.lamaisonsimon.fr/wiki/doku.php?id=maison2:compteur_d_eau:compteur_d_eau>
- <https://github.com/psykokwak-com/everblu-meters-esp8266>

Meters supported:

- Itron EverBlu Cyble Enhanced

## Current State

This fork has the everblu library completely rewritten from scratch.
As a consequence, there are a few things missing and today a blocking bug where the ESP is not able to communicate with the Everblu. Investigation is on-going and if you wish to help, don't hesitate to do it.

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

// User defined - read these off the meter label (see meter_label.png)
#define METER_YEAR 20       // Two-digit year
#define METER_SERIAL 123456 // Serial WITHOUT any leading zero
#define GDO0_PIN 5          // GPIO connected to the CC1101 GDO0 pin

EspMQTTClient mqtt(
  "WifiSSID",
  "WifiPassword",
  "myMqttServer",   // MQTT Broker server
  "MQTTUsername",   // Can be omitted if not needed
  "MQTTPassword",   // Can be omitted if not needed
  "TestClient"      // Client name that uniquely identify your device
);
```

> **Serial number:** do not keep a leading zero. `0123456` is an *octal* literal in
> C, so it silently becomes 42798, and a serial containing an 8 or a 9 will not
> compile at all. Write `123456`.

> **GDO0 pin:** the default is GPIO5 (D1). Set it to whichever pin your CC1101's
> GDO0 is wired to. Avoid GPIO0 — it is a boot-mode strapping pin on the ESP8266.

## Operation

The reader interrogates the meter **once a day at 12:00 local time** by default,
and publishes the result. On first use it does not yet know which frequency
reaches your meter, so press **Full Scan** once; the frequency it finds is stored
in EEPROM and reused from then on.

All controls appear in Home Assistant via autodiscovery, under one *Everblu
Cyble* device:

| Entity | Topic | Purpose |
| --- | --- | --- |
| Reading Time | `everblu/cyble/schedule/time/set` | Time of the daily reading as `HH:MM`, e.g. `12:30`. Persisted. |
| Read Now | `everblu/cyble/command/read` | Read immediately using the known frequency. |
| Full Scan | `everblu/cyble/command/scan` | Forget the stored frequency and sweep for it. |
| Status | `everblu/cyble/status` | `ok`, `reading`, `sweeping`, `asleep`, `no_response`, `not_provisioned`, `no_clock` |

Readings are published retained on `everblu/cyble/liters`, `.../battery` and
`.../num_readings`.

### Debugging without a serial cable

Everything written to the serial console is mirrored to `everblu/cyble/log`, one
line per message:

```sh
mosquitto_sub -h myMqttServer -t 'everblu/cyble/log' -v
```

This topic is deliberately *not* exposed as a Home Assistant entity — it is far
too chatty for the recorder, and entity states are capped at 255 characters.

Lines are queued and published from the main loop rather than at the point of
logging, because much of what is logged happens inside timing-sensitive radio
sequences where a network stall would break the exchange. The queue holds 24
lines; if it overflows, the oldest are dropped and a `[log] N lines dropped`
notice is emitted so gaps are never silent. Anything logged before the broker
connects stays on serial only.

### When the meter will not answer

The meter is deaf outside **Monday–Saturday, 06:00–18:00**, and the reader will
not transmit then — a manual read or scan at 22:00 returns `asleep` without
putting anything on the air. It also refuses to transmit before NTP has
synchronised the clock (`no_clock`), since the window check depends on it.

A scheduled reading is tracked by date, not by a countdown: if the device is
offline or the meter is unreachable at 12:00, it retries on subsequent ticks the
same day, and stops once that day has been read successfully.
