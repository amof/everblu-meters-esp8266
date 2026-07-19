/**
 * Project source :
 * http://www.lamaisonsimon.fr/wiki/doku.php?id=maison2:compteur_d_eau:compteur_d_eau
 *
 * Based on modified version from Psykokwak:
 * https://github.com/psykokwak-com/everblu-meters-esp8266
 *
 * Dependencies:
 *  - EspMQTTClient library https://github.com/plapointe6/EspMQTTClient (by Patrick Lapointe) version 1.13.3
 *
 * Use:
 *  - TODO
 */

#include "EspMQTTClient.h"
#include <ArduinoOTA.h>
#include <coredecls.h>
#include "everblu_mqtt.h"
#include "everblu_cyble.h"
#include "everblu_log.h"
// Deployment configuration and credentials. Gitignored: copy
// include/secrets.h.example to include/secrets.h and fill it in.
#include "secrets.h"

// Optional - only for DEBUG
#define DEBUG_MQTT

const char *NtpServer = NTP_SERVER;
const char *TZstr = TZ_STRING;

EspMQTTClient mqtt(
    WIFI_SSID,
    WIFI_PASSWORD,
    MQTT_SERVER,
    MQTT_USERNAME,
    MQTT_PASSWORD,
    MQTT_CLIENT_NAME // Also becomes the mDNS hostname used for OTA uploads
);

EverbluCyble cyble(
    GDO0_PIN,
    METER_YEAR,
    METER_SERIAL);

// How often to check whether the scheduled reading time has arrived. The check
// itself is date-based, so this only bounds how late a reading can start.
//
// Five minutes rather than one, because the tick is also the retry interval: a
// day that is due but unread is retried on every tick until it succeeds, and a
// failing read costs a full frequency sweep of transmission each time. Starting
// a daily reading up to five minutes late is worth far more than four extra
// sweeps an hour into a meter that is not answering.
#define SCHEDULE_TICK_MS (1000UL * 60 * 5)

// Internal variables
bool cbtime_set = false;
bool schedule_started = false;

/**
 * @brief Parse "HH:MM" into hour and minute.
 *
 * Home Assistant enforces the pattern on the text entity, but the topic is
 * writable by anything on the broker, so the value is validated here too.
 */
bool parseHhMm(const String &value, uint8_t *hour, uint8_t *minute)
{
  int separator = value.indexOf(':');
  if (separator < 1)
    return false;

  long h = value.substring(0, separator).toInt();
  long m = value.substring(separator + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59)
    return false;

  *hour = (uint8_t)h;
  *minute = (uint8_t)m;
  return true;
}

/**
 * @brief Publish the schedule currently in force as "HH:MM".
 */
void publishScheduledTime()
{
  // Oversized: the values are always 2 digits, but the compiler cannot prove it.
  char formatted[8];
  snprintf(formatted, sizeof(formatted), "%02u:%02u", cyble.scheduledHour(), cyble.scheduledMinute());
  mqtt.publish(scheduleTimeStateTopic, formatted, true);
}

/**
 * @brief Publish the wiring verdict and the chip identity behind it.
 *
 * The check itself runs at boot, long before MQTT exists, so the verdict is
 * held on the reader and published from here — on every connect, and again
 * whenever the check is re-run by hand.
 */
void publishWiring()
{
  const char *verdict = "ok";
  switch (cyble.wiringResult())
  {
  case WIRING_OK:
    verdict = "ok";
    break;
  case WIRING_SPI_FAILED:
    verdict = "spi_failed";
    break;
  case WIRING_GDO0_FAILED:
    verdict = "gdo0_failed";
    break;
  }
  mqtt.publish(wiringStateTopic, verdict, true);

  ChipIdentity id = cyble.chipIdentity();
  char attributes[64];
  snprintf(attributes, sizeof(attributes),
           "{\"partnum\": \"0x%02X\", \"version\": \"0x%02X\"}",
           id.partNumber, id.version);
  mqtt.publish(wiringAttributesTopic, attributes, true);
}

/**
 * @brief Publish the outcome of a read attempt.
 */
void publishResult(MeterReadResult result)
{
  switch (result)
  {
  case METER_READ_OK:
    digitalWrite(LED_BUILTIN, LOW); // turned on
    mqtt.publish(litersStateTopic, String(cyble.current_index), true);
    mqtt.publish(batteryStateTopic, String(cyble.battery_lifetime), true);
    mqtt.publish(counterStateTopic, String(cyble.num_of_readings), true);
    mqtt.publish(statusStateTopic, "ok", true);
    break;

  case METER_ASLEEP:
    mqtt.publish(statusStateTopic, "asleep", true);
    break;

  case METER_NO_RESPONSE:
    Serial.println("Unable to retrieve data from meter.");
    mqtt.publish(statusStateTopic, cyble.isProvisioned() ? "no_response" : "not_provisioned", true);
    break;

  case METER_BUSY:
    // The caller published "reading" or "sweeping" before getting here, so
    // leaving the status alone would strand the entity on a transient value:
    // the run that is actually in flight publishes its own outcome, not this
    // one's. Say what happened to *this* request instead.
    mqtt.publish(statusStateTopic, "busy", true);
    break;
  }
}

/**
 * @brief Read the meter now, whatever the schedule says.
 */
void readNow()
{
  if (!cbtime_set)
  {
    Serial.println("Clock not synchronized yet, refusing to transmit");
    mqtt.publish(statusStateTopic, "no_clock", true);
    return;
  }

  mqtt.publish(statusStateTopic, "reading", true);
  publishResult(cyble.readMeter(time(nullptr)));
}

/**
 * @brief Forget the stored frequency and sweep the whole band for it.
 */
void scanNow()
{
  if (!cbtime_set)
  {
    Serial.println("Clock not synchronized yet, refusing to transmit");
    mqtt.publish(statusStateTopic, "no_clock", true);
    return;
  }

  mqtt.publish(statusStateTopic, "sweeping", true);
  cyble.forgetMeter();
  publishResult(cyble.scanForMeter(time(nullptr)));
}

/**
 * @brief Periodic tick that performs the scheduled daily reading.
 */
void onScheduleTick()
{
  mqtt.executeDelayed(SCHEDULE_TICK_MS, onScheduleTick);

  // Do not transmit before the clock is trustworthy: the wakeup-window check
  // is the only thing preventing a pointless multi-minute sweep at night.
  if (!cbtime_set)
  {
    Serial.println("Clock not synchronized yet, waiting...");
    return;
  }

  bool performed = false;
  MeterReadResult result = cyble.readIfDue(time(nullptr), &performed);

  // Stay quiet on the ticks where nothing was due, so the status topic reflects
  // real attempts rather than flapping every minute.
  if (performed)
    publishResult(result);
}

void onConnectionEstablished()
{
  Serial.println("Connected to MQTT Broker");

  // Only now is publishing possible, so this is where the log starts mirroring.
  // Anything logged before this point stays Serial-only.
  logSetSink([](const char *line) { mqtt.publish(logTopic, line); });
  // Retained, unlike the live topic: this is what makes recent history visible
  // to a client that connects long after the lines were produced.
  logSetSnapshotSink([](const char *blob) { mqtt.publish(logRecentTopic, blob, true); });
  // Makes the topic exist as soon as we connect, and marks the boundary in a
  // captured log where Serial-only history ends and MQTT history begins.
  // Naming the firmware on the boundary line rather than its own means every
  // captured log says which image produced it, without costing a line of the
  // retained snapshot.
  LOG("[Everblu] Log mirroring started, firmware %s\n", FIRMWARE_VERSION);

  // Cancels the retained "offline" left by the last will, or by the OTA start
  // callback. Published on every reconnect, not just the first, because the
  // will fires on every unclean disconnection.
  mqtt.publish(availabilityTopic, availabilityOnline, true);

  // Update time with NTP server
  request_ntp_time();

  Serial.println("> Send MQTT config for HA.");
  // Publish the discovery configuration messages
  mqtt.publish(indexConfigTopic, currentIndexConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(batteryConfigTopic, batteryConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(counterConfigTopic, numReadingsConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(statusConfigTopic, statusConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(scheduleTimeConfigTopic, scheduleTimeConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(buttonReadConfigTopic, buttonReadConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(buttonScanConfigTopic, buttonScanConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(wiringConfigTopic, wiringConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(buttonWiringConfigTopic, buttonWiringConfigPayload, true);
  delay(50); // Do not remove

  // Reflect the stored schedule so the Home Assistant entity shows the value
  // actually in force rather than an empty state.
  publishScheduledTime();
  // The verdict from boot. Published on every reconnect rather than once, so a
  // broker restart does not leave the entity blank until the next power cycle.
  publishWiring();

  mqtt.subscribe(scheduleTimeSetTopic, [](const String &message) {
    uint8_t hour = 0;
    uint8_t minute = 0;
    if (!parseHhMm(message, &hour, &minute))
    {
      LOG("Ignoring unparsable reading time '%s'\n", message.c_str());
      return;
    }
    if (cyble.setScheduledTime(hour, minute))
      publishScheduledTime();
  });

  mqtt.subscribe(commandReadTopic, [](const String &message) { readNow(); });
  mqtt.subscribe(commandScanTopic, [](const String &message) { scanNow(); });

  // Unlike read and scan, this transmits nothing and takes milliseconds, so it
  // needs neither a clock nor a wakeup-window check.
  mqtt.subscribe(commandWiringTopic, [](const String &message) {
    cyble.checkWiring();
    publishWiring();
  });

  // Start the schedule once and only once. onConnectionEstablished() fires on
  // every MQTT reconnection, and a read can cost minutes of transmission, so
  // starting it here unguarded would let a flapping broker drive the radio.
  if (!schedule_started)
  {
    schedule_started = true;
    onScheduleTick();
  }
}

void setup()
{
  // Initialize serial link to give feedback
  Serial.begin(115200);
  // First line out of the boot, before anything can fail: if the device is in a
  // reboot loop this is still enough to tell which image is looping.
  Serial.printf("Everblu Cyble - Reader %s\n", FIRMWARE_VERSION);

  // Change the packet size from 128 bytes to 1024
  mqtt.setMaxPacketSize(1024);

#ifdef DEBUG_MQTT
  mqtt.enableDebuggingMessages(true);
#endif

  // Retained, so a client connecting long after the device died still learns it
  // is dead. Without a will, every entity keeps its last retained value and a
  // device that never came back from an update goes on reporting "ok".
  mqtt.enableLastWillMessage(availabilityTopic, availabilityOffline, true);

  // Over-the-air updates. The password is passed explicitly because
  // enableOTA(NULL) silently reuses the MQTT password instead, which would make
  // the broker credential a flashing credential without saying so anywhere.
  mqtt.enableOTA(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    // The CC1101 has no reset line, so the ESP rebooting does not stop it. If
    // an update lands mid-transmission the chip radiates a carrier until the
    // new image reaches init() — flash write, reboot and eboot copy, tens of
    // seconds. Park it while we still can.
    cyble.radioIdle();

    // Say we are going away rather than waiting for the broker to work it out:
    // the will only fires once the keepalive expires, leaving the entities
    // looking healthy for seconds after the device has stopped existing.
    mqtt.publish(availabilityTopic, availabilityOffline, true);
    LOG("[Everblu] OTA update starting, rebooting shortly\n");
    logFlush();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    // A failed update does not reboot, so the device is still here and the
    // "offline" published above is now a lie. Take it back.
    LOG("[Everblu] OTA update failed (error %u)\n", (unsigned)error);
    mqtt.publish(availabilityTopic, availabilityOnline, true);
  });

  // A sweep blocks for minutes; keep the MQTT connection alive across it and
  // drain the log so progress is visible while it runs rather than only after.
  cyble.setBetweenAttemptsCallback([]() {
    logFlush();
    mqtt.loop();
  });

  cyble.init();
}

void loop()
{
  mqtt.loop();
  // Drain here rather than at the point of logging: publishing is network I/O
  // and much of what we log happens inside timing-sensitive radio sequences.
  logFlush();
}

/**
 * @brief Get the ntp time object
 *
 */
void request_ntp_time()
{
  Serial.println("Retrieving time from NTP server...");

  // Sync time using NTP
  configTime(0, 0, NtpServer);

  // Configure local time
  setenv("TZ", TZstr, 1);
  tzset(); // save the TZ variable

  // Create callback to know when the NTP server has responded
  settimeofday_cb(time_is_set);
}

/**
 * @brief Callback called when NTP time has been received
 *
 */
void time_is_set(void)
{
  cbtime_set = true;

  Serial.println("Clock is synchronized !");
  time_t tnow = time(nullptr);
  printf("         ctime: %s", ctime(&tnow));              // print formated local time
  printf(" local asctime: %s", asctime(localtime(&tnow))); // print formated local time
  printf("gmtime asctime: %s", asctime(gmtime(&tnow)));    // print formated gm time
  Serial.println();
}