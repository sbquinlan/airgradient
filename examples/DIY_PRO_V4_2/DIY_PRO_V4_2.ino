/*
Important: This code is only for the DIY PRO PCB Version 3.7 that has a push button mounted.

This is the code for the AirGradient DIY PRO Air Quality Sensor with an ESP8266 Microcontroller with the SGP40 TVOC module from AirGradient.

It is a high quality sensor showing PM2.5, CO2, Temperature and Humidity on a small display and can send data over Wifi.

Build Instructions: https://www.airgradient.com/open-airgradient/instructions/diy-pro-v37/

Kits (including a pre-soldered version) are available: https://www.airgradient.com/open-airgradient/kits/

The codes needs the following libraries installed:
“WifiManager by tzapu, tablatronix” tested with version 2.0.11-beta
“U8g2” by oliver tested with version 2.32.15
"Sensirion I2C SGP41" by Sensation Version 0.1.0
"Sensirion Gas Index Algorithm" by Sensation Version 3.2.1
"Arduino-SHT" by Johannes Winkelmann Version 1.2.2

Configuration:
Please set in the code below the configuration parameters.

If you have any questions please visit our forum at https://forum.airgradient.com/

If you are a school or university contact us for a free trial on the AirGradient platform.
https://www.airgradient.com/

CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License

*/


#include <AirGradient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

#include <EEPROM.h>
#include "SHTSensor.h"

//#include "SGP30.h"
#include <SensirionI2CSgp41.h>
#include <NOxGasIndexAlgorithm.h>
#include <VOCGasIndexAlgorithm.h>

#include <SparkLine.h>
#include <U8g2lib.h>

AirGradient ag = AirGradient();
SensirionI2CSgp41 sgp41;
VOCGasIndexAlgorithm voc_algorithm;
NOxGasIndexAlgorithm nox_algorithm;
SHTSensor sht;

// Display bottom right
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// Replace above if you have display on top left
//U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2, /* reset=*/ U8X8_PIN_NONE);

// CONFIGURATION START
// for persistent saving and loading
const uint8_t settings_addr = 4;
const uint8_t hostname_addr = 8;
const uint8_t hostname_len = 24;
const uint8_t sparkInterval_addr = 32;

//set to the endpoint you would like to use
boolean useAGPlatform = false;
String APIROOT = "http://hw.airgradient.com/";

// set to true to switch from Celcius to Fahrenheit
boolean useFahrenheit = true;

// PM2.5 in US AQI (default ug/m3)
boolean useUSAQI = true;

// interval to record measurements to spark
uint8_t sparkInterval = 1;

// set to true if you want to connect to wifi. You have 60 seconds to connect. Then it will go into an offline mode.
char hostname[24];

// CONFIGURATION END

const std::function<float(const uint16_t x)> identity = [](const uint16_t val) { return static_cast<float>(val); };
const std::function<float(const uint16_t x)> K_TO_C = [](uint16_t kelvin_hundredths) {
  return (kelvin_hundredths / 100.0) - 273.15;
};
const std::function<float(const uint16_t x)> K_TO_F = [](uint16_t kelvin_hundredths) {
  return (K_TO_C(kelvin_hundredths) * 9. / 5. + 32.);
};

// Calculate PM2.5 US AQI
const std::function<float(const uint16_t x)> PM_TO_AQI_US = [](const uint16_t pm02) {
  if (pm02 <= 12.0) return ((50 - 0) / (12.0 - .0) * (pm02 - .0) + 0);
  else if (pm02 <= 35.4) return ((100 - 50) / (35.4 - 12.0) * (pm02 - 12.0) + 50);
  else if (pm02 <= 55.4) return ((150 - 100) / (55.4 - 35.4) * (pm02 - 35.4) + 100);
  else if (pm02 <= 150.4) return ((200 - 150) / (150.4 - 55.4) * (pm02 - 55.4) + 150);
  else if (pm02 <= 250.4) return ((300 - 200) / (250.4 - 150.4) * (pm02 - 150.4) + 200);
  else if (pm02 <= 350.4) return ((400 - 300) / (350.4 - 250.4) * (pm02 - 250.4) + 300);
  else if (pm02 <= 500.4) return ((500 - 400) / (500.4 - 350.4) * (pm02 - 350.4) + 400);
  else return 500.;
};

class AirVariable 
{
  using UnitConversionFunction = std::function<float(const uint16_t x)>;
  SparkLine<uint16_t> spark;
  uint16_t last = 0;
  String label;
  String units;
  UnitConversionFunction conversion;

  private:
    void formatNumber(char* s, size_t n, float x) const {
      uint16_t asint = static_cast<uint16_t>(x);
      if (asint == x) {
        snprintf(s, n, "%d", asint);
      } else {
        snprintf(s, n, "%.1f", x);
      }
    }
  
  public:
    void update(uint16_t measurement, boolean recordToSpark) {
      last = measurement;
      if (recordToSpark) {
        spark.add(measurement);
      }
    }

    String getLabel() const {
      return label;
    }

    uint16_t getLast() const {
      return last;
    }

    void setConversion(UnitConversionFunction newVal) {
      conversion = newVal;
    }

    void setUnits(String newVal) {
      units = newVal;
    }

    void draw() const {
      char number_buffer[6];
      u8g2.setFont(u8g2_font_t0_18b_tf);
      
      formatNumber(number_buffer, 6, conversion(last));
      u8g2_uint_t width = u8g2.drawStr(0, 31, number_buffer);

      u8g2.setFont(u8g2_font_t0_11_tf);
      u8g2.drawStr(0, 11, label.c_str());
      u8g2.drawStr(width, 31, units.c_str());

      formatNumber(number_buffer, 6, conversion(spark.findMax()));
      u8g2.drawStr(98, 24, number_buffer);

      formatNumber(number_buffer, 6, conversion(spark.findMin()));
      u8g2.drawStr(98, 36, number_buffer);

      u8g2.setFont(u8g2_font_siji_t_6x10);
      u8g2.drawGlyph(86, 24, 0xe12b);
      u8g2.drawGlyph(86, 36, 0xe12c);

      spark.draw(0, 50, 76, 16);
    }

    AirVariable(
      const char* _label,
      const char* _units,
      UnitConversionFunction converter = identity
    )
      : spark(60, [&](const uint16_t x0, const uint16_t y0, const uint16_t x1, const uint16_t y1) { 
          u8g2.drawLine(x0, y0, x1, y1);
        }),
        label(_label),
        units(_units),
        conversion(converter)
    {}  
};

const char* cubic_microgram_unit = "\xB5g/m\xB3";
AirVariable TVOC("TVOC", "");
AirVariable NOX("NOX", "");
AirVariable CO2("CO\xB2", "ppm");
AirVariable pm10("PM 10", cubic_microgram_unit);
AirVariable pm25(
  "PM 2.5", 
  useUSAQI ? "AQI" : cubic_microgram_unit, 
  useUSAQI ? PM_TO_AQI_US : identity
);
AirVariable pm01("PM 1", cubic_microgram_unit);
AirVariable pm03("PM 0.03", "");
AirVariable temp(
  "TEMPERATURE", 
  useFahrenheit ? "\xB0" "F" : "\xB0" "C", 
  useFahrenheit ? K_TO_F : K_TO_C
);
AirVariable hum("HUMIDITY", "%");

const AirVariable* const allVariables[] = {
  &TVOC, 
  &NOX, 
  &CO2,
  &pm10,
  &pm25, 
  &pm01,
  &pm03,
  &temp,
  &hum 
};

// STATE

// index in allVariables of displayed variable
uint8_t displayVariable = 0;

// wifi display state toggle
boolean displaySSID = true;

// current spark interval
uint8_t currentInterval = 0;

// time in seconds needed for NOx conditioning
uint16_t conditioning_runs = 2;

int lastState = HIGH;
int buttonState = HIGH;
unsigned long debounceStart = 0;
const unsigned long debounceDelay = 50;

// Wifi Manager
const String ag_platform_yes = "yes";
const String temp_units_fahrenheit = "fahrenheit";
const String pm_units_usaqi = "USAQI";

#define HTML_LABEL(id, label) "<label for=\"" id "\">" label "</label>"
#define HTML_SELECT_START(id, name) "<select id=\"" id "\" name=\"" name "\">"
#define HTML_DEFAULT_OPTION(value, text) "<option value=\"" + value + "\" selected>" + text + "</option>"
#define HTML_OPTION(value, text) "<option value=\"" + value + "\">" + text + "</option>"
#define HTML_SELECT_END "</select>"

WiFiManager wifiManager;
WiFiManagerParameter wifi_hostname("hostname", "Hostname", "hostname", 23);
WiFiManagerParameter wifi_ag_platform(
  (HTML_LABEL("ag_platform", "AirGradient Platform")
  HTML_SELECT_START("ag_platform", "ag_platform")
    HTML_DEFAULT_OPTION(ag_platform_yes, "Yes")
    HTML_OPTION("no", "No")
  HTML_SELECT_END).c_str()
);
WiFiManagerParameter wifi_temp_units(
  (HTML_LABEL("temp_units", "Temperature Units")
  HTML_SELECT_START("temp_units", "temp_units")
    HTML_DEFAULT_OPTION(temp_units_fahrenheit, "Fahrenheit")
    HTML_OPTION("celsius", "Celsius")
  HTML_SELECT_END).c_str()
);
WiFiManagerParameter wifi_pm_units(
  (HTML_LABEL("pm_units", "PM 2.5 Units")
  HTML_SELECT_START("pm_units", "pm_units")
    HTML_DEFAULT_OPTION(pm_units_usaqi, "AQI")
    HTML_OPTION("cubic_microgram", cubic_microgram_unit)
  HTML_SELECT_END).c_str()
);
WiFiManagerParameter wifi_spark_interval(
  (HTML_LABEL("spark_interval", "Chart Time Window")
  HTML_SELECT_START("spark_interval", "spark_interval")
    HTML_DEFAULT_OPTION(String("1"), String("5 min"))
    HTML_OPTION("2", "10 min")
    HTML_OPTION("6", "30 min")
    HTML_OPTION("12", "1 hour")
    HTML_OPTION("72", "6 hour")
    HTML_OPTION("144", "12 hour")
    HTML_OPTION("288", "1 day")
  HTML_SELECT_END).c_str()
);

void setup() {
  Serial.begin(115200);
  Serial.println("Hello");
  u8g2.begin();
  sht.init();
  sht.setAccuracy(SHTSensor::SHT_ACCURACY_MEDIUM);
  //u8g2.setDisplayRotation(U8G2_R0);

  EEPROM.begin(512);
  delay(500);

  readSettings();

  pinMode(D7, INPUT_PULLUP);

  setupWifi();

  sgp41.begin(Wire);
  ag.CO2_Init();
  ag.PMS_Init();
  ag.TMP_RH_Init(0x44);
}

void setupWifi() {
  wifiManager.setTimeout(90);
  wifiManager.setConfigPortalBlocking(false);

  wifiManager.setSaveParamsCallback(wifi_saveParameters);
  wifiManager.setSaveConfigCallback(wifi_startWebPortal);
  wifiManager.setWebServerCallback(wifi_addRoutes);

  wifiManager.addParameter(&wifi_ag_platform);
  wifiManager.addParameter(&wifi_temp_units);
  wifiManager.addParameter(&wifi_pm_units);
  wifiManager.addParameter(&wifi_spark_interval);

  String HOTSPOT = "AG-" + String(ESP.getChipId(), HEX);
  
  if (String(hostname).isEmpty()) {
    strncpy(hostname, HOTSPOT.c_str(), 24);
  }

  if (wifiManager.autoConnect((const char*)HOTSPOT.c_str())) {
    // if autoConnect succeeds the first time, it doesn't call the callback
    wifi_startWebPortal();
  }
}

void wifi_startWebPortal() {
  wifiManager.startWebPortal();
}

void wifi_addRoutes() {
  wifiManager.server->on("/metrics", wifi_handleMetrics);
}

#define JSON_FIELD(name, value) "\"" + name + "\": \"" + value + "\""
void wifi_handleMetrics() {
  // Use json-exporter if you want to ingest this to prometheus. Not worth being 
  // prometheus-specific at this point.
  String metrics = "{\n"
    JSON_FIELD(String("id"), String(ESP.getChipId(), HEX))
    JSON_FIELD(String("mac"), WiFi.macAddress())
    JSON_FIELD(String("hostname"), String(hostname));

  const uint8_t count = sizeof(allVariables) / sizeof(allVariables[0]);
  for (uint i = 0; i < count; i++) {
    if (i != 0) {
      metrics += ",\n";
    }
    metrics += JSON_FIELD(allVariables[i]->getLabel(), String(allVariables[i]->getLast()));
  }
  metrics += "\n}";
  wifiManager.server->send(200, "text/plain", metrics);
}

void wifi_saveParameters() {
  strncpy(hostname, wifi_hostname.getValue(), 24);
  useAGPlatform = ag_platform_yes.equals(wifi_ag_platform.getValue());
  useFahrenheit = temp_units_fahrenheit.equals(wifi_temp_units.getValue());
  useUSAQI = pm_units_usaqi.equals(wifi_pm_units.getValue());
  sparkInterval = String(wifi_spark_interval.getValue()).toInt();
  validateSparkInterval();

  writeSettings();
}


void loop() {
  static esp8266::polledTimeout::periodicMs fivSecond(5000);
  static esp8266::polledTimeout::periodicMs tenSecond(10000);

  if (fivSecond) {
    updateTVOC();
    updateTempHum();
    updateCo2();
    updatePm();

    currentInterval = (currentInterval + 1) % (sparkInterval + 1);
    displayVariable = (displayVariable + 1) % (sizeof(allVariables) / sizeof(allVariables[0]));
  }
  if (tenSecond) {
    sendToServer();

    displaySSID = !displaySSID;
  }

  wifiManager.process();
  
  int reading = digitalRead(D7);
  if (reading != lastState) {
    debounceStart = millis();
  }
  if ((millis() - debounceStart) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) {
        // reset
        WiFi.disconnect();
        useAGPlatform = false;
        useFahrenheit = true;
        useUSAQI = true;
        sparkInterval = 1;
        strcpy(hostname, "");
        writeSettings();
        renderText("Resetting", "", "");
        delay(1000);

        ESP.reset();
      }
    }
  }
  lastState = reading;
  
  renderVariable();
}

void readSettings() {
  uint8_t settings = EEPROM.read(settings_addr);
  useAGPlatform = (settings & 1) == 1;
  useFahrenheit = (settings & (1 << 1)) == 1;
  useUSAQI = (settings & (1 << 2)) == 1;

  for (unsigned long i = 0; i < hostname_len; i++) {
    hostname[i] = EEPROM.read(hostname_addr + i);
  }

  temp.setConversion(useFahrenheit ? K_TO_F : K_TO_C);
  temp.setUnits(useFahrenheit ? "\xB0" "F" : "\xB0" "C");
  pm25.setConversion(useUSAQI ? PM_TO_AQI_US : identity);
  pm25.setUnits(useUSAQI ? "AQI" : "\xB5g/m\xB3");
  WiFi.setHostname(hostname);

  sparkInterval = EEPROM.read(sparkInterval_addr);

  validateSparkInterval();
}

void writeSettings() {
  validateSparkInterval();

  uint8_t settings = 0;
  if (useAGPlatform) {
    settings |= 1;
  }
  if (useFahrenheit) {
    settings |= (1 << 1);
  }
  if (useUSAQI) {
    settings |= (1 << 2);
  }
  EEPROM.write(settings_addr, settings);

  for (unsigned long i = 0; i < hostname_len; i++) {
    EEPROM.write(hostname_addr + i, hostname[i]);
  }

  EEPROM.write(sparkInterval_addr, sparkInterval);

  EEPROM.commit();

  temp.setConversion(useFahrenheit ? K_TO_F : K_TO_C);
  temp.setUnits(useFahrenheit ? "\xB0" "F" : "\xB0" "C");
  pm25.setConversion(useUSAQI ? PM_TO_AQI_US : identity);
  pm25.setUnits(useUSAQI ? "AQI" : "\xB5g/m\xB3");
  WiFi.setHostname(hostname);
}

void updateTVOC() {
  uint16_t error;
  uint16_t srawVoc = 0;
  uint16_t srawNox = 0;

  uint16_t temp_celsius = static_cast<uint16_t>(std::round(K_TO_C(temp.getLast())));
  uint16_t compensationT = static_cast<uint16_t>((temp_celsius + 45) * 65535. / 175.);
  uint16_t compensationRh = static_cast<uint16_t>(hum.getLast() * 65535. / 100.);

  if (conditioning_runs > 0) {
    error = sgp41.executeConditioning(
      compensationRh, 
      compensationT, 
      srawVoc
    );
    conditioning_runs--;
  } else {
    error = sgp41.measureRawSignals(
      compensationRh, 
      compensationT, 
      srawVoc,
      srawNox
    );
  }

  TVOC.update(voc_algorithm.process(srawVoc), currentInterval % sparkInterval == 0);
  NOX.update(nox_algorithm.process(srawNox), currentInterval % sparkInterval == 0);
  Serial.println("TVOC: " + String(TVOC.getLast()));
}

void updateCo2() {
  CO2.update(ag.getCO2_Raw(), currentInterval % sparkInterval == 0);
  Serial.println("CO2: " + String(CO2.getLast()));
}

void updatePm() {
  pm01.update(ag.getPM1_Raw(), currentInterval % sparkInterval == 0);
  pm25.update(ag.getPM2_Raw(), currentInterval % sparkInterval == 0);
  pm10.update(ag.getPM10_Raw(), currentInterval % sparkInterval == 0);
  pm03.update(ag.getPM0_3Count(), currentInterval % sparkInterval == 0);
  Serial.println("PM25: " + String(pm25.getLast()));
}

void updateTempHum() {
  if (sht.readSample()) {
    // temp is hundreths of a degree to avoid using floats
    uint16_t kelvin = static_cast<uint16_t>(std::round(
      (sht.getTemperature() + 273.15) * 100
    ));
    temp.update(kelvin, currentInterval % sparkInterval == 0);
    Serial.println("TEMP: " + String(kelvin / 100));
    hum.update(
      static_cast<uint16_t>(sht.getHumidity()),
      currentInterval % sparkInterval == 0
    );
  } else {
    Serial.println("Error in readSample()");
  }
}

void renderVariable() {
  const AirVariable* variable = allVariables[displayVariable];
  u8g2.firstPage();
  do {
    variable->draw();
    renderWifi();
    renderSparkCaption();
  } while (u8g2.nextPage());
}

void validateSparkInterval() {
  switch (sparkInterval) {
    case 1:
    case 2:
    case 6:
    case 12:
    case 72:
    case 144:
    case 288:
      return;
    default:
      sparkInterval = 1;
  }
}

void renderSparkCaption() {
  String sparkCaption;
  switch (sparkInterval) {
    case 1:
      sparkCaption = "last 5m";
    case 2: 
      sparkCaption = "last 10m";
    case 6:
      sparkCaption = "last 30m";
    case 12:
      sparkCaption = "last 1h";
    case 72:
      sparkCaption = "last 6h";
    case 144:
      sparkCaption = "last 12h";
    case 288:
      sparkCaption = "last 1d";
    default:
      sparkInterval = 1;
      sparkCaption = "last 5m";
  }
  u8g2.setFont(u8g2_font_t0_11_tf);
  u8g2.drawStr(79, 50, sparkCaption.c_str());
}

void renderWifi() {
  u8g2.setFont(u8g2_font_siji_t_6x10);
  if (WiFi.status() != WL_CONNECTED) {
    u8g2.drawGlyph(0, 64, 0xe217);

    u8g2.setFont(u8g2_font_t0_11_tf);
    u8g2.drawStr(12, 64, "DISCONNECTED");
  } else {
    u8g2.drawGlyph(0, 64, 0xe21a);

    u8g2.setFont(u8g2_font_t0_11_tf);
    if (displaySSID) {
      u8g2.drawStr(12, 64, WiFi.SSID().substring(0, 19).c_str());
    } else {
      char sliced[20];
      strncpy(sliced, hostname, 19);
      u8g2.drawStr(12, 64, sliced);
    }
  }
}

void renderText(String ln1, String ln2, String ln3) {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_t0_16_tf);
    u8g2.drawStr(1, 10, String(ln1).c_str());
    u8g2.drawStr(1, 30, String(ln2).c_str());
    u8g2.drawStr(1, 50, String(ln3).c_str());
  } while (u8g2.nextPage());
}

void sendToServer() {
  if (!useAGPlatform) { 
    return;
  }

  String payload = "{\"wifi\":" + String(WiFi.RSSI())
                    + ", \"rco2\":" + String(CO2.getLast())
                    + ", \"pm01\":" + String(pm01.getLast())
                    + ", \"pm02\":" + String(pm25.getLast())
                    + ", \"pm10\":" + String(pm10.getLast())
                    + ", \"pm003_count\":" + String(pm03.getLast())
                    + ", \"tvoc_index\":" + String(TVOC.getLast())
                    + ", \"nox_index\":" + String(NOX.getLast())
                    + ", \"atmp\":" + String(K_TO_C(temp.getLast()))
                    + ", \"rhum\":" + String(hum.getLast())
                    + "}";

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(payload);
    String POSTURL = APIROOT + "sensors/airgradient:" + String(ESP.getChipId(), HEX) + "/measures";
    Serial.println(POSTURL);
    WiFiClient client;
    HTTPClient http;
    http.begin(client, POSTURL);
    http.addHeader("content-type", "application/json");
    int httpCode = http.POST(payload);
    String response = http.getString();
    Serial.println(httpCode);
    Serial.println(response);
    http.end();
  } else {
    Serial.println("WiFi Disconnected");
  }
}
