/*
Important: This code is only for the DIY OUTDOOR OPEN AIR Presoldered Kit with the ESP-C3.

It is a high quality outdoor air quality sensor with dual PM2.5 modules and can send data over Wifi.

Kits are available: https://www.airgradient.com/open-airgradient/kits/

The codes needs the following libraries installed:
“WifiManager by tzapu, tablatronix” tested with version 2.0.11-beta

For built instructions and how to patch the PMS library: https://www.airgradient.com/open-airgradient/instructions/diy-open-air-presoldered-v11/

Note that below code only works with both PM sensor modules connected.

If you have any questions please visit our forum at https://forum.airgradient.com/

CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License

*/

#include <Arduino.h>
#include <AirGradient.h>
#include <EEPROM.h>
#include <HardwareSerial.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <WiFiManager.h>

#define DEBUG true

HTTPClient client;

PMS pms1 = PMS();
PMS pms2 = PMS();

float pm1Mean = 0;
float pm25Mean = 0;
float pm10Mean = 0;
float pm03Mean = 0;
float pmTempMean = 0;
float pmHumMean = 0;

int count = 1;
int targetCount = 40;
unsigned long loopCount = 0;
unsigned long lastTime = 0;
unsigned long startTime = 0;

// CONFIGURATION START
// for persistent saving and loading
const uint8_t settings_addr = 4;
const uint8_t hostname_addr = 8;
const uint8_t hostname_len = 24;
const uint8_t sparkInterval_addr = 32;

//set to the endpoint you would like to use
boolean useAGPlatform = false;
String APIROOT = "http://hw.airgradient.com/";

char hostname[24];

// Wifi Manager
const String ag_platform_yes = "yes";

/** 
 * WiFiManagerParameter sucks if you want something other than a text input 
 * The only way to get it to use the entire customHTML is to null out getID
 * and if you do that, then value can never be set.
 * 
 * init() always nulls out _value.
 * init() calls setValue().
 * setValue() is the only way to set _value.
 * setValue() only sets _value if _id is not null.
 * init() is the only way to set _id
 */
class CustomParameter : public WiFiManagerParameter
{
  public:
    CustomParameter(
      const char* value, 
      int length, 
      const char* customHtml
    ) : WiFiManagerParameter(customHtml) {
      _id = "";
      setValue(value, length);
      _id = nullptr;
    }
};

WiFiManager wifiManager;
WiFiManagerParameter wifi_hostname("hostname", "Hostname", "hostname", 23);

// Note that each param's name is important. param_# is a format that 
// WiFiManager insists on if you're going to implement completely custom params.
CustomParameter wifi_ag_platform(
  "yes",
  4,
  "<label for=\"param_1\">AirGradient Platform</label>"
  "<select id=\"param_1\" name=\"param_1\">"
    "<option value=\"yes\" selected>Yes</option>"
    "<option value=\"no\">No</option>"
  "</select>"
);


void readSettings() {
  uint8_t settings = EEPROM.read(settings_addr);
  useAGPlatform = (settings & 1) == 1;

  for (unsigned long i = 0; i < hostname_len; i++) {
    hostname[i] = EEPROM.read(hostname_addr + i);
  }
  wifiManager.setHostname(hostname);

}

void writeSettings() {
  uint8_t settings = 0;
  if (useAGPlatform) {
    settings |= 1;
  }
  EEPROM.write(settings_addr, settings);

  for (unsigned long i = 0; i < hostname_len; i++) {
    EEPROM.write(hostname_addr + i, hostname[i]);
  }

  EEPROM.commit();
  wifiManager.setHostname(hostname);
}

void debugln(String msg)
{
  if (DEBUG)
    Serial.println(msg);
}

void IRAM_ATTR isr()
{
  wifiManager.resetSettings();
  useAGPlatform = false;
  strcpy(hostname, "");
  writeSettings();
  debugln("resetting");
  delay(1000);

  ESP.restart();
}

void switchLED(boolean ledON)
{
  if (ledON)
  {
    digitalWrite(10, HIGH);
  }
  else
  {
    digitalWrite(10, LOW);
  }
}

void resetWatchdog()
{
  digitalWrite(2, HIGH);
  delay(20);
  digitalWrite(2, LOW);
}

String getNormalizedMac()
{
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return mac;
}

void sendPayload(String payload)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    debugln("post skipped, not network connection");
  }

  switchLED(true);
  String url = APIROOT + "sensors/airgradient:" + getNormalizedMac() + "/measures";
  debugln(url);
  debugln(payload);
  client.setConnectTimeout(5 * 1000);
  client.begin(url);
  client.addHeader("content-type", "application/json");
  int httpCode = client.POST(payload);
  debugln(String(httpCode));
  client.end();
  resetWatchdog();
  switchLED(false);
}

void sendPing()
{
  if (!useAGPlatform) {
    return;
  }
  String payload = "{\"wifi\":" + String(WiFi.RSSI()) + ", \"boot\":" + loopCount + "}";
  sendPayload(payload);
}

void postToServer()
{
  if (!useAGPlatform) {
    return;
  }
  String payload = "{\"wifi\":" + String(WiFi.RSSI()) + \
    ", \"pm01\":" + String(pm1Mean) + \
    ", \"pm02\":" + String(pm25Mean) + \
    ", \"pm10\":" + String(pm10Mean) + \
    ", \"pm003_count\":" + String(pm03Mean) + \
    ", \"atmp\":" + String(pmTempMean / 100) + \
    ", \"rhum\":" + String(pmHumMean / 100) + \
    ", \"boot\":" + loopCount + ", \"channels\": {} }";
  loopCount++;
  sendPayload(payload);
}

#define JSON_FIELD(name, value) "\"" + name + "\": \"" + value + "\",\n"
void wifi_handleMetrics() {
  // Use json-exporter if you want to ingest this to prometheus. Not worth being 
  // prometheus-specific at this point.
  String metrics = "{\n"
    JSON_FIELD(String("mac"), WiFi.macAddress())
    JSON_FIELD(String("hostname"), String(hostname))
    JSON_FIELD("pm01", String(pm1Mean))
    JSON_FIELD("pm02", String(pm25Mean))
    JSON_FIELD("pm10", String(pm10Mean))
    JSON_FIELD("pm003_count", String(pm03Mean))
    JSON_FIELD("atmp", String(pmTempMean / 100))
    "\"rhum\": \"" + String(pmHumMean / 100) + "\"\n"
  "}";
  wifiManager.server->send(200, "application/json", metrics);
}

void wifi_addRoutes() {
  Serial.println("Adding metrics route");
  wifiManager.server->on("/metrics", wifi_handleMetrics);
}

void wifi_saveParameters() {
  strncpy(hostname, wifi_hostname.getValue(), 24);

  Serial.println("hostname param: " + String(wifi_hostname.getValue()));
  Serial.println("platform param: " + String(wifi_ag_platform.getValue()));

  useAGPlatform = ag_platform_yes.equals(wifi_ag_platform.getValue());

  writeSettings();
}

void setupWifi() {
  wifiManager.setTimeout(90);
  wifiManager.setConfigPortalBlocking(false);

  wifiManager.setSaveParamsCallback(wifi_saveParameters);
  wifiManager.setWebServerCallback(wifi_addRoutes);

  wifiManager.addParameter(&wifi_hostname);
  wifiManager.addParameter(&wifi_ag_platform);
  uint param_num = wifiManager.getParametersCount();
  Serial.println("Params: " + String(param_num));

  String HOTSPOT = "AG-" + String(getNormalizedMac());
  if (String(hostname).isEmpty()) {
    strncpy(hostname, HOTSPOT.c_str(), 24);
  }
  wifi_hostname.setValue(hostname, 24);
  wifiManager.autoConnect((const char*)hostname);
}

void setup()
{
  if (DEBUG)
  {
    Serial.begin(115200);
    // see https://github.com/espressif/arduino-esp32/issues/6983
    Serial.setTxTimeoutMs(0); // <<<====== solves the delay issue
  }

  debugln("Serial Number: " + getNormalizedMac());

  // default hardware serial, PMS connector on the right side of the C3 mini on the Open Air
  Serial0.begin(9600);

  // second hardware serial, PMS connector on the left side of the C3 mini on the Open Air
  Serial1.begin(9600, SERIAL_8N1, 0, 1);

  // led
  pinMode(10, OUTPUT);

  // push button
  pinMode(9, INPUT_PULLUP);
  attachInterrupt(9, isr, FALLING);

  pinMode(2, OUTPUT);
  digitalWrite(2, LOW);

  pms1.init(Serial0);
  pms1.passiveMode();
  pms2.init(Serial1);
  pms2.passiveMode();

  setupWifi();
  sendPing();
  switchLED(false);

  startTime = millis();
}

uint16_t addToMean(uint16_t avg, uint16_t count, uint16_t x) {
  return avg + (x - avg) / count;
}

void updateMeansWithData(const PMS::Data& data) {
  pm1Mean = addToMean(pm1Mean, count, data.PM_AE_UG_1_0);
  pm25Mean = addToMean(pm25Mean, count, data.PM_AE_UG_2_5);
  pm10Mean = addToMean(pm10Mean, count, data.PM_AE_UG_10_0);
  pm03Mean = addToMean(pm03Mean, count, data.PM_RAW_0_3);
  pmTempMean = addToMean(pmTempMean, count, data.PM_TMP);
  pmHumMean = addToMean(pmHumMean, count, data.PM_HUM);
  ++count;
}

void loop()
{
  wifiManager.process();

  // if the wifi is connected and the web portal is not active, then start it.
  if (
    WiFi.status() == WL_CONNECTED &&
    !wifiManager.getWebPortalActive() && 
    !wifiManager.getConfigPortalActive()
  ) {
    wifiManager.startWebPortal();
  }

  unsigned long now = millis();
  // allow sensors to warm up
  if (now - startTime < 10000) {
    return;
  }

  // only take samples every 2 seconds
  if (now - lastTime < 2000) {
    return;
  }
  lastTime = now;

  pms1.requestRead();
  if (pms1.readUntil(2000))
  {
    updateMeansWithData(pms1.getData());
  }
  pms2.requestRead();
  if (pms2.readUntil(2000))
  {
    updateMeansWithData(pms2.getData());
  }

  if (count >= targetCount)
  {
    postToServer();

    count = 1;
    pm1Mean = 0;
    pm25Mean = 0;
    pm10Mean = 0;
    pm03Mean = 0;
    pmTempMean = 0;
    pmHumMean = 0;
  }
}
