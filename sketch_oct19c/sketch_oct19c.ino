/* BarleyBox_MQTT_OLED_AHT25.ino
   Integrated:
    - AHT25 (Adafruit_AHTX0) on TwoWire(1) (SDA=32,SCL=33)
    - OLED SSD1306 via U8g2 on Wire (SDA=4,SCL=15)
    - NTC 10K on ADC (GPIO34)
    - 2x Relay (mist, heater)
    - Servo feeder (Servo)
    - MQTT telemetry & control (config update via MQTT)
    - Google Sheets webhook upload (POST JSON)
    - Config persisted in Preferences (NVS)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>

// 必須在 PubSubClient.h 之前定義，增加 MQTT 訊息緩衝區大小
#define MQTT_MAX_PACKET_SIZE 512

#include <PubSubClient.h>
#include <Adafruit_AHTX0.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <math.h>
#include <time.h>
#include <Thermistor.h>
#include <NTC_Thermistor.h>

// ================ USER CONFIG (edit) =================
const char* WIFI_SSID = "webduino.io";
const char* WIFI_PASS = "webduino01";

const char* MQTT_SERVER = "broker.MQTTGO.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_USER = ""; // optional
const char* MQTT_PASS = ""; // optional

const char* GOOGLE_WEBHOOK = "https://script.google.com/macros/s/AKfycbw8MBbQkwL8JyguYL8eidSgi-GIkV6GDaT8zFZfHKBDwAktwgdHSP8CIAG_FN488bQ2TA/exec";
const char* GOOGLE_TOKEN   = "AKfycbw8MBbQkwL8JyguYL8eidSgi-GIkV6GDaT8zFZfHKBDwAktwgdHSP8CIAG_FN488bQ2TA";

const char* DEVICE_ID = "barleybox-001";
// =====================================================

// ================= hardware pins =================
#define PIN_RELAY_MIST  22
#define PIN_RELAY_HEAT  21
#define PIN_SERVO       25
#define PIN_NTC_ADC     34   // ADC1 recommended for WiFi stability
#define PIN_SDA_OLED    4
#define PIN_SCL_OLED    15
#define AHT25_SDA       32
#define AHT25_SCL       33

// NTC Thermistor params (10K NTC 3950)
#define NTC_SENSOR_PIN              34
#define NTC_REFERENCE_RESISTANCE    10000
#define NTC_NOMINAL_RESISTANCE      10000
#define NTC_NOMINAL_TEMPERATURE     25
#define NTC_B_VALUE                 3950
#define ESP32_ANALOG_RESOLUTION     4095
#define ESP32_ADC_VREF_MV           3300

// Instances
WiFiClient espClient;
PubSubClient mqtt(espClient);
HTTPClient http;
Preferences prefs;
Adafruit_AHTX0 aht;
Servo feeder;

// NTC Thermistor
Thermistor* thermistor = nullptr;

// OLED (Wire)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE, PIN_SCL_OLED, PIN_SDA_OLED);

// second I2C for AHT25
TwoWire I2C_AHT = TwoWire(1);

// timing
unsigned long lastTelemetryMs = 0;
unsigned long lastGoogleMs = 0;
unsigned long lastMistStopMs = 0;
unsigned long mistStartMs = 0;
unsigned long lastHeaterToggleMs = 0;
unsigned long lastFeedMs = 0;
unsigned long lastMQTTReconnectTry = 0;
unsigned long lcdUpdateMs = 0;
unsigned long ntcLowTempStart = 0;
bool ntcLowTempHeating = false;

#define TELEMETRY_INTERVAL_MS 1000UL

// state
bool heater_on = false;
bool mist_on = false;
bool autoMode = true;
bool ahtReady = false;

// MQTT topics (populated on connect)
String topicTelemetry;
String topicControlHeater;
String topicControlMist;
String topicControlFeed;
String topicConfigIn;
String topicConfigOut;
String topicStatus;
String topicCommand;

// ----------------- config (persisted) -----------------
struct Config {
  float T_heat_on;
  float T_heat_off;
  float H_mist_on;
  float H_mist_off;
  unsigned long mist_max_on_seconds;
  unsigned long mist_min_off_seconds;
  unsigned long feed_interval_seconds;
  int feed_duration_ms;
  unsigned long upload_interval_seconds;
  float heater_max_temp;
  String feed_times_csv;
  float ntc_low_temp_threshold;
  unsigned long ntc_heat_on_minutes;

  Config() {
    T_heat_on = 25.5f;
    T_heat_off = 28.5f;
    H_mist_on = 60.0f;
    H_mist_off = 68.0f;
    mist_max_on_seconds = 60;
    mist_min_off_seconds = 600;
    feed_interval_seconds = 6 * 3600;
    feed_duration_ms = 3000;
    upload_interval_seconds = 1800;
    heater_max_temp = 32.0f;
    feed_times_csv = "09:00,17:00";
    ntc_low_temp_threshold = 20.0f;
    ntc_heat_on_minutes = 30;
  }
};
Config config;

const char* PREF_NAMESPACE = "barleycfg";

// forward declarations
float readNTCTempC();
void publishTelemetry(float t_env, float h_env, float t_sub);
void doAutoControl(float t_env, float h_env, float t_sub);
void setHeater(bool on);
void setMist(bool on);
void triggerFeed();
void saveConfigToPrefs();
void loadConfigFromPrefs();
void publishConfig();
void handleConfigJson(const char* payload);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void connectWiFi();
void connectMQTT();
void uploadToGoogle(float envTemp, float envHumi, float mediaTemp);
void updateOLED(float t_env, float h_env, float t_sub);
void scanI2C(TwoWire &bus, const char *busName);

// ================= setup =================
void setup() {
  Serial.begin(115200);
  delay(200);

  // pins
  pinMode(PIN_RELAY_HEAT, OUTPUT);
  pinMode(PIN_RELAY_MIST, OUTPUT);
  digitalWrite(PIN_RELAY_HEAT, LOW);
  digitalWrite(PIN_RELAY_MIST, LOW);

  feeder.attach(PIN_SERVO, 500, 2400);
  feeder.write(0);

  prefs.begin(PREF_NAMESPACE, false);
  loadConfigFromPrefs();

  // init I2C buses
  Wire.begin(PIN_SDA_OLED, PIN_SCL_OLED);
  Wire.setClock(100000);
  delay(50);
  scanI2C(Wire, "OLED Bus (Wire)");

  I2C_AHT.begin(AHT25_SDA, AHT25_SCL);
  I2C_AHT.setClock(100000);
  delay(50);
  scanI2C(I2C_AHT, "AHT Bus (I2C_AHT)");

  // init OLED
  oled.begin();
  oled.clearBuffer();
  oled.setFont(u8g2_font_ncenB08_tr);
  oled.drawStr(0, 12, "BarleyBox Init...");
  oled.sendBuffer();

  // init AHT using second I2C port
  ahtReady = aht.begin(&I2C_AHT);
  if (!ahtReady) {
    Serial.println("AHT init failed!");
    oled.clearBuffer();
    oled.drawStr(0, 12, "AHT init failed!");
    oled.sendBuffer();
  } else {
    Serial.println("AHT initialized");
  }

  // init NTC Thermistor (ESP32 optimized version)
  thermistor = new NTC_Thermistor_ESP32(
    NTC_SENSOR_PIN,
    NTC_REFERENCE_RESISTANCE,
    NTC_NOMINAL_RESISTANCE,
    NTC_NOMINAL_TEMPERATURE,
    NTC_B_VALUE,
    ESP32_ADC_VREF_MV,
    ESP32_ANALOG_RESOLUTION
  );
  Serial.println("NTC Thermistor (ESP32) initialized");

  // WiFi & MQTT
  connectWiFi();
  
  // 設置 MQTT 緩衝區大小（必須在連接前設置）
  mqtt.setBufferSize(1024);
  Serial.println("MQTT buffer size set to 1024 bytes");
  
  // NTP time sync (GMT+8 for Taiwan)
  Serial.println("Configuring NTP time sync...");
  configTime(8 * 3600, 0, "time.stdtime.gov.tw", "pool.ntp.org");
  Serial.print("Waiting for NTP time sync");
  time_t now = time(nullptr);
  int retry = 0;
  while (now < 8 * 3600 * 2 && retry < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }
  Serial.println();
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    Serial.println(&timeinfo, "✅ Time synced: %Y-%m-%d %H:%M:%S");
    oled.clearBuffer();
    oled.drawStr(0, 12, "Time synced");
    oled.sendBuffer();
    delay(1000);
  } else {
    Serial.println("⚠️ Time sync failed, timestamps may be incorrect");
  }
  
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  connectMQTT();

  lastTelemetryMs = millis();
  lastGoogleMs = millis();
  lastMistStopMs = millis() - config.mist_min_off_seconds * 1000UL;
  lcdUpdateMs = millis();
}

// ================= loop =================
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (!mqtt.connected()) {
    if (now - lastMQTTReconnectTry > 5000) {
      Serial.println("[MQTT] Attempting reconnection...");
      connectMQTT();
      lastMQTTReconnectTry = now;
    }
  } else {
    mqtt.loop();
  }

  // Read sensors
  float t_env = NAN, h_env = NAN;
  sensors_event_t humidity, temp_event;
  if (ahtReady) {
    aht.getEvent(&humidity, &temp_event);
    t_env = temp_event.temperature;
    h_env = humidity.relative_humidity;
  }
  float t_sub = readNTCTempC();

  // Telemetry every second
  if (now - lastTelemetryMs >= TELEMETRY_INTERVAL_MS) {
    publishTelemetry(t_env, h_env, t_sub);
    lastTelemetryMs = now;
  }

  // Auto control
  if (autoMode) doAutoControl(t_env, h_env, t_sub);

  // OLED update every 1s
  if (now - lcdUpdateMs >= 1000) {
    updateOLED(t_env, h_env, t_sub);
    lcdUpdateMs = now;
  }

  // Google upload
  if (now - lastGoogleMs >= config.upload_interval_seconds * 1000UL) {
    uploadToGoogle(t_env, h_env, t_sub);
    lastGoogleMs = now;
  }

  // NTC low-temp auto-heat (configurable)
  if (!isnan(t_sub)) {
    if (!ntcLowTempHeating && t_sub < config.ntc_low_temp_threshold) {
      Serial.println("[NTC] below threshold -> start timed heating");
      setHeater(true);
      ntcLowTempStart = now;
      ntcLowTempHeating = true;
    }
    if (ntcLowTempHeating && (now - ntcLowTempStart >= config.ntc_heat_on_minutes * 60UL * 1000UL)) {
      Serial.println("[NTC] timed heating done -> stop");
      setHeater(false);
      ntcLowTempHeating = false;
    }
  }

  delay(10);
}

// ================= functions =================

void scanI2C(TwoWire &bus, const char *busName) {
  Serial.printf("\n🔍 Scanning %s ...\n", busName);
  byte found = 0;
  for (byte addr = 1; addr < 127; addr++) {
    bus.beginTransmission(addr);
    byte err = bus.endTransmission();
    if (err == 0) {
      Serial.printf("  ✅ Found device at 0x%02X\n", addr);
      found++;
    }
  }
  if (!found) Serial.printf("  ⚠️ No I2C devices found on %s\n", busName);
}

float readNTCTempC() {
  if (thermistor == nullptr) {
    return NAN;
  }
  
  const double celsius = thermistor->readCelsius();
  
  // 檢查是否為有效讀數
  if (isnan(celsius) || celsius < -50 || celsius > 150) {
    return NAN;
  }
  
  return (float)celsius;
}

void publishTelemetry(float t_env, float h_env, float t_sub) {
  StaticJsonDocument<384> doc;
  doc["device"] = DEVICE_ID;
  doc["ts_ms"] = millis();
  // Round to 1 decimal place
  if (!isnan(t_env)) doc["temp_env"] = round(t_env * 10.0f) / 10.0f;
  if (!isnan(h_env)) doc["hum_env"] = round(h_env * 10.0f) / 10.0f;
  if (!isnan(t_sub)) doc["temp_sub"] = round(t_sub * 10.0f) / 10.0f;
  else doc["temp_sub"] = t_sub;
  doc["heater_on"] = heater_on;
  doc["mist_on"] = mist_on;
  doc["mode"] = (autoMode ? "AUTO" : "MANUAL");
  // include thresholds so web UI can read them via telemetry topic
  JsonObject thr = doc.createNestedObject("thresholds");
  thr["T_heat_on"] = config.T_heat_on;
  thr["T_heat_off"] = config.T_heat_off;
  thr["H_mist_on"] = config.H_mist_on;
  thr["H_mist_off"] = config.H_mist_off;
  thr["mist_max_on_seconds"] = config.mist_max_on_seconds;
  thr["mist_min_off_seconds"] = config.mist_min_off_seconds;
  thr["heater_max_temp"] = config.heater_max_temp;
  thr["ntc_low_temp_threshold"] = config.ntc_low_temp_threshold;
  thr["ntc_heat_on_minutes"] = config.ntc_heat_on_minutes;

  String out; serializeJson(doc, out);
  if (mqtt.connected()) {
    bool published = mqtt.publish(topicTelemetry.c_str(), out.c_str());
    Serial.print(published ? "[MQTT-OK] " : "[MQTT-FAIL] ");
    Serial.print("Topic: ");
    Serial.print(topicTelemetry);
    Serial.print(" | ");
  } else {
    Serial.print("[MQTT-DISCONNECTED] ");
  }
  Serial.println(out);
}

void setHeater(bool on) {
  if (on == heater_on) return;
  digitalWrite(PIN_RELAY_HEAT, on ? HIGH : LOW);
  heater_on = on;
  lastHeaterToggleMs = millis();
  StaticJsonDocument<192> s;
  s["device"] = DEVICE_ID;
  s["heater_on"] = heater_on;
  String out; serializeJson(s, out);
  if (mqtt.connected()) mqtt.publish(topicStatus.c_str(), out.c_str());
  Serial.printf("Heater -> %s\n", heater_on ? "ON" : "OFF");
}

void setMist(bool on) {
  if (on == mist_on) return;
  digitalWrite(PIN_RELAY_MIST, on ? HIGH : LOW);
  mist_on = on;
  if (!on) lastMistStopMs = millis();
  else mistStartMs = millis();
  StaticJsonDocument<192> s;
  s["device"] = DEVICE_ID;
  s["mist_on"] = mist_on;
  String out; serializeJson(s, out);
  if (mqtt.connected()) mqtt.publish(topicStatus.c_str(), out.c_str());
  Serial.printf("Mist -> %s\n", mist_on ? "ON" : "OFF");
}

void triggerFeed() {
  unsigned long now = millis();
  if (now - lastFeedMs < config.feed_interval_seconds * 1000UL) {
    Serial.println("Feed ignored: too soon");
    return;
  }
  Serial.println("Triggering feed...");
  feeder.write(90); // adjust per your mechanical setup
  delay(config.feed_duration_ms);
  feeder.write(0);
  lastFeedMs = now;
  StaticJsonDocument<192> ev;
  ev["device"] = DEVICE_ID;
  ev["event"] = "feed";
  ev["ts_ms"] = now;
  String out; serializeJson(ev, out);
  if (mqtt.connected()) mqtt.publish(topicStatus.c_str(), out.c_str());
}

void doAutoControl(float t_env, float h_env, float t_sub) {
  unsigned long now = millis();
  // Heater based on substrate temp (NTC)
  if (!isnan(t_sub)) {
    if (!heater_on && t_sub <= config.T_heat_on) setHeater(true);
    if (heater_on && t_sub >= config.T_heat_off) setHeater(false);
    if (heater_on && t_sub >= config.heater_max_temp) {
      setHeater(false);
      StaticJsonDocument<256> warn;
      warn["device"] = DEVICE_ID;
      warn["warning"] = "heater_force_off_over_temp";
      warn["temp_sub"] = t_sub;
      String out; serializeJson(warn, out);
      if (mqtt.connected()) mqtt.publish(topicStatus.c_str(), out.c_str());
    }
  }
  // Mist based on environment humidity
  if (!isnan(h_env)) {
    if (!mist_on) {
      if (h_env <= config.H_mist_on && (now - lastMistStopMs >= config.mist_min_off_seconds * 1000UL)) {
        setMist(true);
      }
    } else {
      if (h_env >= config.H_mist_off) setMist(false);
      else if ((now - mistStartMs) >= config.mist_max_on_seconds * 1000UL) setMist(false);
    }
    if (h_env >= 85.0 && mist_on) {
      setMist(false);
      StaticJsonDocument<256> warn;
      warn["device"] = DEVICE_ID;
      warn["warning"] = "hum_too_high_mist_forced_off";
      warn["hum"] = h_env;
      String out; serializeJson(warn, out);
      if (mqtt.connected()) mqtt.publish(topicStatus.c_str(), out.c_str());
    }
  }
}

void saveConfigToPrefs() {
  prefs.putFloat("T_heat_on", config.T_heat_on);
  prefs.putFloat("T_heat_off", config.T_heat_off);
  prefs.putFloat("H_mist_on", config.H_mist_on);
  prefs.putFloat("H_mist_off", config.H_mist_off);
  prefs.putULong("mist_max_on_seconds", config.mist_max_on_seconds);
  prefs.putULong("mist_min_off_seconds", config.mist_min_off_seconds);
  prefs.putULong("feed_interval_seconds", config.feed_interval_seconds);
  prefs.putInt("feed_duration_ms", config.feed_duration_ms);
  prefs.putULong("upload_interval_seconds", config.upload_interval_seconds);
  prefs.putFloat("heater_max_temp", config.heater_max_temp);
  prefs.putString("feed_times_csv", config.feed_times_csv);
  prefs.putFloat("ntc_low_temp_threshold", config.ntc_low_temp_threshold);
  prefs.putULong("ntc_heat_on_minutes", config.ntc_heat_on_minutes);
  Serial.println("Config saved to NVS");
}

void loadConfigFromPrefs() {
  if (prefs.isKey("T_heat_on")) config.T_heat_on = prefs.getFloat("T_heat_on", config.T_heat_on);
  if (prefs.isKey("T_heat_off")) config.T_heat_off = prefs.getFloat("T_heat_off", config.T_heat_off);
  if (prefs.isKey("H_mist_on")) config.H_mist_on = prefs.getFloat("H_mist_on", config.H_mist_on);
  if (prefs.isKey("H_mist_off")) config.H_mist_off = prefs.getFloat("H_mist_off", config.H_mist_off);
  if (prefs.isKey("mist_max_on_seconds")) config.mist_max_on_seconds = prefs.getULong("mist_max_on_seconds", config.mist_max_on_seconds);
  if (prefs.isKey("mist_min_off_seconds")) config.mist_min_off_seconds = prefs.getULong("mist_min_off_seconds", config.mist_min_off_seconds);
  if (prefs.isKey("feed_interval_seconds")) config.feed_interval_seconds = prefs.getULong("feed_interval_seconds", config.feed_interval_seconds);
  if (prefs.isKey("feed_duration_ms")) config.feed_duration_ms = prefs.getInt("feed_duration_ms", config.feed_duration_ms);
  if (prefs.isKey("upload_interval_seconds")) config.upload_interval_seconds = prefs.getULong("upload_interval_seconds", config.upload_interval_seconds);
  if (prefs.isKey("heater_max_temp")) config.heater_max_temp = prefs.getFloat("heater_max_temp", config.heater_max_temp);
  if (prefs.isKey("feed_times_csv")) config.feed_times_csv = prefs.getString("feed_times_csv", config.feed_times_csv);
  if (prefs.isKey("ntc_low_temp_threshold")) config.ntc_low_temp_threshold = prefs.getFloat("ntc_low_temp_threshold", config.ntc_low_temp_threshold);
  if (prefs.isKey("ntc_heat_on_minutes")) config.ntc_heat_on_minutes = prefs.getULong("ntc_heat_on_minutes", config.ntc_heat_on_minutes);
  Serial.println("Config loaded from NVS");
}

void publishConfig() {
  StaticJsonDocument<600> doc;
  doc["device"] = DEVICE_ID;
  doc["T_heat_on"] = config.T_heat_on;
  doc["T_heat_off"] = config.T_heat_off;
  doc["H_mist_on"] = config.H_mist_on;
  doc["H_mist_off"] = config.H_mist_off;
  doc["mist_max_on_seconds"] = config.mist_max_on_seconds;
  doc["mist_min_off_seconds"] = config.mist_min_off_seconds;
  doc["feed_interval_seconds"] = config.feed_interval_seconds;
  doc["feed_duration_ms"] = config.feed_duration_ms;
  doc["upload_interval_seconds"] = config.upload_interval_seconds;
  doc["heater_max_temp"] = config.heater_max_temp;
  doc["feed_times_csv"] = config.feed_times_csv;
  doc["ntc_low_temp_threshold"] = config.ntc_low_temp_threshold;
  doc["ntc_heat_on_minutes"] = config.ntc_heat_on_minutes;
  doc["mode"] = autoMode ? "AUTO" : "MANUAL";
  String out; serializeJson(doc, out);
  if (mqtt.connected()) mqtt.publish(topicConfigOut.c_str(), out.c_str());
}

void handleConfigJson(const char* payload) {
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Invalid JSON in config");
    return;
  }
  bool changed = false;
  if (doc.containsKey("ntc_low_temp_threshold")) { config.ntc_low_temp_threshold = doc["ntc_low_temp_threshold"].as<float>(); changed = true; }
  if (doc.containsKey("ntc_heat_on_minutes")) { config.ntc_heat_on_minutes = doc["ntc_heat_on_minutes"].as<unsigned long>(); changed = true; }

  if (doc.containsKey("T_heat_on")) { config.T_heat_on = doc["T_heat_on"].as<float>(); changed = true; }
  if (doc.containsKey("T_heat_off")) { config.T_heat_off = doc["T_heat_off"].as<float>(); changed = true; }
  if (doc.containsKey("H_mist_on")) { config.H_mist_on = doc["H_mist_on"].as<float>(); changed = true; }
  if (doc.containsKey("H_mist_off")) { config.H_mist_off = doc["H_mist_off"].as<float>(); changed = true; }
  if (doc.containsKey("mist_max_on_seconds")) { config.mist_max_on_seconds = doc["mist_max_on_seconds"].as<unsigned long>(); changed = true; }
  if (doc.containsKey("mist_min_off_seconds")) { config.mist_min_off_seconds = doc["mist_min_off_seconds"].as<unsigned long>(); changed = true; }
  if (doc.containsKey("feed_interval_seconds")) { config.feed_interval_seconds = doc["feed_interval_seconds"].as<unsigned long>(); changed = true; }
  if (doc.containsKey("feed_duration_ms")) { config.feed_duration_ms = doc["feed_duration_ms"].as<int>(); changed = true; }
  if (doc.containsKey("upload_interval_seconds")) { config.upload_interval_seconds = doc["upload_interval_seconds"].as<unsigned long>(); changed = true; }
  if (doc.containsKey("heater_max_temp")) { config.heater_max_temp = doc["heater_max_temp"].as<float>(); changed = true; }
  if (doc.containsKey("feed_times_csv")) { config.feed_times_csv = String((const char*)doc["feed_times_csv"]); changed = true; }
  if (doc.containsKey("mode")) {
    String m = String((const char*)doc["mode"]);
    autoMode = (m.equalsIgnoreCase("AUTO"));
    changed = true;
  }
  if (changed) {
    saveConfigToPrefs();
    publishConfig();
    Serial.println("Config updated via MQTT and saved");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg;
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  Serial.printf("MQTT recv topic:%s payload:%s\n", t.c_str(), msg.c_str());

  if (t == topicControlHeater) {
    if (msg == "ON") { setHeater(true); autoMode = false; publishConfig(); }
    else if (msg == "OFF") { setHeater(false); autoMode = false; publishConfig(); }
    else if (msg == "AUTO") { autoMode = true; publishConfig(); }
  } else if (t == topicControlMist) {
    if (msg == "ON") { setMist(true); autoMode = false; publishConfig(); }
    else if (msg == "OFF") { setMist(false); autoMode = false; publishConfig(); }
    else if (msg == "AUTO") { autoMode = true; publishConfig(); }
  } else if (t == topicControlFeed) {
    if (msg == "TRIGGER" || msg == "RUN") triggerFeed();
  } else if (t == topicConfigIn) {
    handleConfigJson(msg.c_str());
  } else if (t == topicCommand) {
    if (msg == "publish_config") publishConfig();
    if (msg == "reboot") { ESP.restart(); }
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Connecting WiFi %s ...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    oled.clearBuffer();
    oled.drawStr(0, 12, "WiFi connected");
    oled.sendBuffer();
  } else {
    Serial.println("WiFi connect failed or timed out");
    oled.clearBuffer();
    oled.drawStr(0, 12, "WiFi failed");
    oled.sendBuffer();
  }
}

void connectMQTT() {
  if (mqtt.connected()) return;
  Serial.print("Connecting MQTT...");
  String clientId = String(DEVICE_ID) + "-" + String(random(0xffff), HEX);
  bool ok;
  if (String(MQTT_USER).length() > 0) ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);
  else ok = mqtt.connect(clientId.c_str());
  if (ok) {
    Serial.println("MQTT connected");
    // topics
    topicTelemetry = String("farm/telemetry/") + DEVICE_ID;
    topicControlHeater = String("farm/control/") + DEVICE_ID + "/heater";
    topicControlMist = String("farm/control/") + DEVICE_ID + "/mist";
    topicControlFeed = String("farm/control/") + DEVICE_ID + "/feed";
    topicConfigIn = String("farm/config/") + DEVICE_ID;
    topicConfigOut = String("farm/config/") + DEVICE_ID + "/current";
    topicStatus = String("farm/status/") + DEVICE_ID;
    topicCommand = String("farm/command/") + DEVICE_ID;

    // subscribe
    mqtt.subscribe(topicControlHeater.c_str());
    mqtt.subscribe(topicControlMist.c_str());
    mqtt.subscribe(topicControlFeed.c_str());
    mqtt.subscribe(topicConfigIn.c_str());
    mqtt.subscribe(topicCommand.c_str());

    // publish initial
    publishConfig();
    StaticJsonDocument<256> st;
    st["device"] = DEVICE_ID;
    st["status"] = "online";
    String out; serializeJson(st, out);
    mqtt.publish(topicStatus.c_str(), out.c_str());
  } else {
    Serial.println("MQTT connect failed");
  }
}

void uploadToGoogle(float envTemp, float envHumi, float mediaTemp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Google] WiFi not connected; skip upload");
    return;
  }
  HTTPClient http;
  http.begin(GOOGLE_WEBHOOK);
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<256> doc;
  time_t nowt = time(NULL);
  char ts[32];
  strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&nowt));
  doc["token"] = String(GOOGLE_TOKEN);
  doc["device"] = DEVICE_ID;
  doc["timestamp"] = String(ts);
  // Round to 1 decimal place
  if (!isnan(envTemp)) doc["env_temp"] = round(envTemp * 10.0f) / 10.0f;
  if (!isnan(envHumi)) doc["env_humi"] = round(envHumi * 10.0f) / 10.0f;
  if (!isnan(mediaTemp)) doc["media_temp"] = round(mediaTemp * 10.0f) / 10.0f;
  else doc["media_temp"] = mediaTemp;
  doc["heater_on"] = heater_on;
  doc["mist_on"] = mist_on;
  doc["mode"] = autoMode ? "AUTO" : "MANUAL";
  String payload; serializeJson(doc, payload);
  int code = http.POST(payload);
  if (code > 0) {
    Serial.printf("[Google] upload code=%d\n", code);
  } else {
    Serial.printf("[Google] upload failed: %s\n", http.errorToString(code).c_str());
  }
  http.end();
}

void updateOLED(float t_env, float h_env, float t_sub) {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tf);

  char line1[32];
  char line2[32];
  char line3[32];

  if (!isnan(t_env)) snprintf(line1, sizeof(line1), "Env: %.1fC  %.0f%%", t_env, h_env);
  else strncpy(line1, "Env: --.-C  --%", sizeof(line1));

  if (!isnan(t_sub)) snprintf(line2, sizeof(line2), "Sub: %.1fC", t_sub);
  else strncpy(line2, "Sub: --.-C", sizeof(line2));

  snprintf(line3, sizeof(line3), "Mode:%s H:%s M:%s",
           autoMode ? "AUTO" : "MAN",
           heater_on ? "ON" : "OFF",
           mist_on ? "ON" : "OFF");

  oled.drawStr(0, 12, line1);
  oled.drawStr(0, 28, line2);
  oled.drawStr(0, 44, line3);

  // show one line of thresholds summary
  char thr[64];
  snprintf(thr, sizeof(thr), "T_on:%.1f T_off:%.1f H_on:%.0f H_off:%.0f",
           config.T_heat_on, config.T_heat_off, config.H_mist_on, config.H_mist_off);
  oled.drawStr(0, 56, thr);

  oled.sendBuffer();
}
