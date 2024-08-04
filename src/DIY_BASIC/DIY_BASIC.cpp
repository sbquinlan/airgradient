/*
This is the code for the AirGradient DIY BASIC Air Quality Sensor with an ESP8266 Microcontroller.

It is a high quality sensor showing PM2.5, CO2, Temperature and Humidity on a small display and can send data over Wifi.

Build Instructions: https://www.airgradient.com/open-airgradient/instructions/diy/

Kits (including a pre-soldered version) are available: https://www.airgradient.com/open-airgradient/kits/

The codes needs the following libraries installed:
“WifiManager by tzapu, tablatronix” tested with version 2.0.11-beta
“U8g2” by oliver tested with version 2.32.15
"Arduino-SHT" by Johannes Winkelmann Version 1.2.2

Configuration:
Please set in the code below the configuration parameters.

If you have any questions please visit our forum at https://forum.airgradient.com/

If you are a school or university contact us for a free trial on the AirGradient platform.
https://www.airgradient.com/

MIT License

*/

#include <Sht/Sht.h>
#include <S8/S8.h>
#include <PMS/PMS.h>
#include <PMS/PMS5003.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <SoftwareSerial.h>
#include <WiFiClient.h>
#include <WiFiManager.h>

#include <U8g2lib.h>

Sht sht(BoardType::DIY_BASIC);
S8 co2(BoardType::DIY_BASIC);
PMS5003 pms(BoardType::DIY_BASIC);

// Display bottom right
U8G2_SSD1306_64X48_ER_1_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

// CONFIGURATION START
// for persistent saving and loading
const uint8_t settings_addr = 4;
const uint8_t hostname_addr = 8;
const uint8_t hostname_len = 24;

//set to the endpoint you would like to use
boolean useAGPlatform = false;
String APIROOT = "http://hw.airgradient.com/";

// set to true to switch from Celcius to Fahrenheit
boolean useFahrenheit = true;

// PM2.5 in US AQI (default ug/m3)
boolean useUSAQI = true;

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
    void update(uint16_t measurement) {
      last = measurement;
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
    }

    AirVariable(
      const char* _label,
      const char* _units,
      UnitConversionFunction converter = identity
    )
      : label(_label),
        units(_units),
        conversion(converter)
    {}  
};

const char* cubic_microgram_unit = "\xB5g/m\xB3";
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

int lastState = HIGH;
int buttonState = HIGH;
unsigned long debounceStart = 0;
const unsigned long debounceDelay = 50;

// Wifi Manager
const String ag_platform_yes = "yes";
const String temp_units_fahrenheit = "fahrenheit";
const String pm_units_usaqi = "USAQI";

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
CustomParameter wifi_temp_units(
  "Celsius",
  10,
  "<label for=\"param_2\">Temperature Units</label>"
  "<select id=\"param_2\" name=\"param_2\">"
    "<option value=\"fahrenheit\" selected>°F</option>"
    "<option value=\"celsius\">°C</option>"
  "</select>"
);
CustomParameter wifi_pm_units(
  "USAQI",
  10,
  "<label for=\"param_3\">PM 2.5 Units</label>"
  "<select id=\"param_3\" name=\"param_3\">"
    "<option value=\"USAQI\" selected>AQI</option>"
    "<option value=\"cubic_mg\">µg/m³</option>"
  "</select>"
);
CustomParameter wifi_spark_interval(
  "1",
  4,
  "<label for=\"param_4\">Chart Time Window</label>"
  "<select id=\"param_4\" name=\"param_4\">"
    "<option value=\"1\" selected>5 min</option>"
    "<option value=\"2\">10 min</option>"
    "<option value=\"6\">30 min</option>"
    "<option value=\"12\">1 hour</option>"
    "<option value=\"72\">6 hour</option>"
    "<option value=\"144\">12 hour</option>"
    "<option value=\"288\">1 day</option>"
  "</select>"
);

void readSettings() {
  uint8_t settings = EEPROM.read(settings_addr);
  useAGPlatform = (settings & 1) == 1;
  useFahrenheit = ((settings >> 1) & 1) == 1;
  useUSAQI = ((settings >> 2) & 1) == 1;

  for (unsigned long i = 0; i < hostname_len; i++) {
    hostname[i] = EEPROM.read(hostname_addr + i);
  }

  temp.setConversion(useFahrenheit ? K_TO_F : K_TO_C);
  temp.setUnits(useFahrenheit ? "\xB0" "F" : "\xB0" "C");
  pm25.setConversion(useUSAQI ? PM_TO_AQI_US : identity);
  pm25.setUnits(useUSAQI ? "AQI" : "\xB5g/m\xB3");
  wifiManager.setHostname(hostname);
}

void writeSettings() {
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

  EEPROM.commit();

  temp.setConversion(useFahrenheit ? K_TO_F : K_TO_C);
  temp.setUnits(useFahrenheit ? "\xB0" "F" : "\xB0" "C");
  pm25.setConversion(useUSAQI ? PM_TO_AQI_US : identity);
  pm25.setUnits(useUSAQI ? "AQI" : "\xB5g/m\xB3");
  wifiManager.setHostname(hostname);
}

void sendToServer() {
  if (!useAGPlatform) { 
    return;
  }

  String payload = "{\"wifi\":\"" + String(WiFi.RSSI())
    + "\", \"rco2\":\"" + String(CO2.getLast())
    + "\", \"pm01\":\"" + String(pm01.getLast())
    + "\", \"pm02\":\"" + String(pm25.getLast())
    + "\", \"pm10\":\"" + String(pm10.getLast())
    + "\", \"pm003_count\":\"" + String(pm03.getLast())
    + "\", \"atmp\":\"" + String(K_TO_C(temp.getLast()))
    + "\", \"rhum\":\"" + String(hum.getLast())
  + "\"\n}";

  if (WiFi.status() == WL_CONNECTED) {
    String POSTURL = APIROOT + "sensors/airgradient:" + String(ESP.getChipId(), HEX) + "/measures";
    WiFiClient client;
    HTTPClient http;
    http.begin(client, POSTURL);
    http.addHeader("content-type", "application/json");
    int httpCode = http.POST(payload);
    String response = http.getString();
    http.end();
  } else {
    Serial.println("WiFi Disconnected");
  }
}

void wifi_handleMetrics() {
  // Use json-exporter if you want to ingest this to prometheus. Not worth being 
  // prometheus-specific at this point.
  String metrics = "{\n"
      "\"id\":\"" + String(ESP.getChipId(), HEX)
    + "\", \"mac\":\"" + WiFi.macAddress()
    + "\", \"hostname\":\"" + String(hostname)
    + "\", \"rco2\":\"" + String(CO2.getLast())
    + "\", \"pm01\":\"" + String(pm01.getLast())
    + "\", \"pm02\":\"" + String(pm25.getLast())
    + "\", \"pm10\":\"" + String(pm10.getLast())
    + "\", \"pm003_count\":\"" + String(pm03.getLast())
    + "\", \"atmp\":\"" + String(K_TO_C(temp.getLast()))
    + "\", \"rhum\":\"" + String(hum.getLast())
  + "\"\n}";
  wifiManager.server->send(200, "application/json", metrics);
}

void wifi_addRoutes() {
  Serial.println("*wm:Adding metrics route");
  wifiManager.server->on("/metrics", wifi_handleMetrics);
}

void wifi_saveParameters() {
  strncpy(hostname, wifi_hostname.getValue(), 24);

  Serial.println("hostname param: " + String(wifi_hostname.getValue()));
  Serial.println("platform param: " + String(wifi_ag_platform.getValue()));
  Serial.println("temp param: " + String(wifi_temp_units.getValue()));
  Serial.println("pm units param: " + String(wifi_pm_units.getValue()));
  Serial.println("spark interval param: " + String(wifi_spark_interval.getValue()));

  useAGPlatform = ag_platform_yes.equals(wifi_ag_platform.getValue());
  useFahrenheit = temp_units_fahrenheit.equals(wifi_temp_units.getValue());
  useUSAQI = pm_units_usaqi.equals(wifi_pm_units.getValue());
  writeSettings();
}

void setupWifi() {
  wifiManager.setTimeout(90);
  wifiManager.setConfigPortalBlocking(false);

  wifiManager.setSaveParamsCallback(wifi_saveParameters);
  wifiManager.setWebServerCallback(wifi_addRoutes);

  wifiManager.addParameter(&wifi_hostname);
  wifiManager.addParameter(&wifi_ag_platform);
  wifiManager.addParameter(&wifi_temp_units);
  wifiManager.addParameter(&wifi_pm_units);
  wifiManager.addParameter(&wifi_spark_interval);
  uint param_num = wifiManager.getParametersCount();
  Serial.println("Params: " + String(param_num));

  String HOTSPOT = "AG-" + String(ESP.getChipId(), HEX);
  if (String(hostname).isEmpty()) {
    strncpy(hostname, HOTSPOT.c_str(), 24);
  }
  wifi_hostname.setValue(hostname, 24);
  wifiManager.autoConnect((const char*)hostname);
}

void updateCo2() {
  int value = co2.getCo2();
  if (value < 0) {
    Serial.println("CO2 read failed");
  } else {
    CO2.update(value);
  }
}

void updatePm() {
  if (pms.isFailed() == false) {
    pm01.update(pms.getPm01Ae());
    pm25.update(pms.getPm25Ae());
    pm10.update(pms.getPm10Ae());
    pm03.update(pms.getPm03ParticleCount());

    Serial.printf(
      "PM1 %d\r\nPM2.5 %d\r\nPM10 %d\r\nPM0.3 %d\r\n", 
      pm01.getLast(), 
      pm25.getLast(), 
      pm10.getLast(), 
      pm03.getLast()
    );
  } else {
    Serial.printf("PMS read failed\r\n");
  }
}

void updateTempHum() {
  if (sht.measure()) {
    // temp is hundreths of a degree to avoid using floats
    uint16_t kelvin = static_cast<uint16_t>(std::round(
      (sht.getTemperature() + 273.15) * 100
    ));
    temp.update(kelvin);
    Serial.printf("Temp %d\r\nHum %d\r\n", kelvin / 100, sht.getRelativeHumidity());
    hum.update(
      static_cast<uint16_t>(sht.getRelativeHumidity())
    );
  } else {
    Serial.printf("Error in updateTempHum()\r\n");
  }
}

void renderWifi() {
  u8g2.setFont(u8g2_font_siji_t_6x10);
  if (WiFi.status() != WL_CONNECTED && wifiManager.getConfigPortalActive()) {
    u8g2.drawGlyph(0, 48, 0xe21a);

    u8g2.setFont(u8g2_font_t0_11_tf);
    if (displaySSID) {
      u8g2.drawStr(12, 48, wifiManager.getWiFiSSID().substring(0, 19).c_str());
    } else {
      u8g2.drawStr(12, 48, "HOTSPOT");
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    u8g2.drawGlyph(0, 48, 0xe217);

    u8g2.setFont(u8g2_font_t0_11_tf);
    u8g2.drawStr(12, 48, "OFFLINE");
  } else {
    u8g2.drawGlyph(0, 48, 0xe21a);

    u8g2.setFont(u8g2_font_t0_11_tf);
    if (displaySSID) {
      u8g2.drawStr(12, 48, wifiManager.getWiFiSSID().substring(0, 19).c_str());
    } else {
      char sliced[9] = { "\0" };
      strncpy(sliced, hostname, 8);
      u8g2.drawStr(12, 48, sliced);
    }
  }
}

void renderVariable() {
  const AirVariable* variable = allVariables[displayVariable];
  u8g2.firstPage();
  do {
    variable->draw();
    renderWifi();
  } while (u8g2.nextPage());
}

void renderText(String ln1, String ln2, String ln3) {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_t0_16_tf);
    u8g2.drawStr(1, 10, String(ln1).c_str());
    u8g2.drawStr(1, 28, String(ln2).c_str());
    u8g2.drawStr(1, 48, String(ln3).c_str());
  } while (u8g2.nextPage());
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("Hello");

  EEPROM.begin(512);

  pinMode(D7, INPUT_PULLUP);
  Wire.begin(SDA, SCL);
  Wire.setClock(100000);
  delay(1000);

  Serial.println("Setting up display");
  u8g2.setBusClock(100000);
  u8g2.begin();
  delay(1000);

  readSettings();
  setupWifi();

  // 0x40 is the default I2C address for the SHT4x
  Serial.println("Setting up SHT");
  sht.begin(Wire, Serial);

  co2.begin(&Serial);
  pms.begin(&Serial);
}

void loop() {
  static esp8266::polledTimeout::oneShot warmUp(10000);
  static esp8266::polledTimeout::periodicMs fivSecond(5000);
  static esp8266::polledTimeout::periodicMs tenSecond(10000);
  
  if (fivSecond && warmUp) {
    updateTempHum();
    updateCo2();
    updatePm();

    displayVariable = (displayVariable + 1) % (sizeof(allVariables) / sizeof(allVariables[0]));
  }
  if (tenSecond) {
    sendToServer();

    displaySSID = !displaySSID;
  }

  wifiManager.process();
  // if the wifi is connected and the web portal is not active, then start it.
  if (
    WiFi.status() == WL_CONNECTED &&
    !wifiManager.getWebPortalActive() && 
    !wifiManager.getConfigPortalActive()
  ) {
    wifiManager.startWebPortal();
  }
  
  int reading = digitalRead(D7);
  if (reading != lastState) {
    debounceStart = millis();
  }
  if ((millis() - debounceStart) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) {
        // reset
        wifiManager.resetSettings();
        useAGPlatform = false;
        useFahrenheit = true;
        useUSAQI = true;
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
