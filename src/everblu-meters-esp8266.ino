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
#include <coredecls.h>
#include "everblu_mqtt.h"
#include "everblu_cyble.h"

// Optional - only for DEBUG
#define DEBUG_MQTT

const char *NtpServer = "myNtpServer";
const char *TZstr = "UTC+0,M3.5.0,M10.5.0/3"; // https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html

// User defined
EspMQTTClient client(
    "WifiSSID",
    "WifiPassword",
    "myMqttServer", // MQTT Broker server
    "MQTTUsername", // Can be omitted if not needed
    "MQTTPassword", // Can be omitted if not needed
    "TestClient"    // Client name that uniquely identify your device
);

EverbluCyble cyble(
    0,      // GDO0 pin
    yy,     // Year
    0123456 // Serial without leading zero
);

// Internal variables
bool cbtime_set = false;

void onUpdateData()
{
  time_t tnow = time(nullptr);
  struct tm *ptm = gmtime(&tnow);
  Serial.printf("Current date (UTC) : %04d/%02d/%02d %02d:%02d:%02d - %s\n", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec, String(tnow, DEC).c_str());

  char iso8601[128];
  strftime(iso8601, sizeof iso8601, "%FT%TZ", gmtime(&tnow));
  // cyble.getDataFromMeter();

  if (cyble.num_of_readings == 0 || cyble.current_index == 0)
  {
    Serial.println("Unable to retrieve data from meter. Retry later...");

    // Call back this function in 10 sec (in miliseconds)
    mqtt.executeDelayed(1000 * 10, onUpdateData);

    return;
  }

  digitalWrite(LED_BUILTIN, LOW); // turned on

  // Push to MQTT
}

void onConnectionEstablished()
{
  Serial.println("Connected to MQTT Broker");

  // Update time with NTP server
  request_ntp_time();

  /* Use this to trigger update of firmware + manual update
    mqtt.subscribe("everblu/cyble/trigger", [](const String& message) {
    });
  */

  Serial.println("> Send MQTT config for HA.");
  // Publish the discovery configuration messages
  mqtt.publish(indexConfigTopic, currentIndexConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(batteryConfigTopic, batteryConfigPayload, true);
  delay(50); // Do not remove
  mqtt.publish(counterConfigTopic, numReadingsConfigPayload, true);
  delay(50); // Do not remove

  onUpdateData();

  cyble.lookForMeter();
}

void setup()
{
  // Initialize serial link to give feedback
  Serial.begin(115200);
  Serial.println("Everblu Cyble - Reader");

  // Change the packet size from 128 bytes to 1024
  mqtt.setMaxPacketSize(1024);

#ifdef DEBUG_MQTT
  mqtt.enableDebuggingMessages(true);
#endif
  cyble.init();
}

void loop()
{
  mqtt.loop();
}

/**
 *
 */
void next_wake_up()
{
  time_t tnow = time(nullptr);
  struct tm *ptm = gmtime(&tnow);
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