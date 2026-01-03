// 大麥蟲智能養殖監控系統 - ESP32 韌體
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Preferences.h>
#include <time.h>
#include <ArduinoJson.h>

// ===== WiFi & MQTT =====
const char* ssid = "webduino.io";
const char* password = "webduino01";
const char* mqtt_server = "MQTTGO.io";
const uint16_t MQTT_PORT = 1883;
String device_id = "barleybox-001";

WiFiClient espClient;
PubSubClient client(espClient);
Preferences preferences;

// ===== GPIO 定義 =====
#define PIN_HEATER 2          // 加熱器繼電器
#define PIN_MIST 22           // 噴霧器繼電器
#define PIN_STEPPER_IN1 25    // 步進馬達 IN1
#define PIN_STEPPER_IN2 26    // 步進馬達 IN2
#define PIN_STEPPER_IN3 27    // 步進馬達 IN3
#define PIN_STEPPER_IN4 14    // 步進馬達 IN4
#define PIN_NTC A0            // NTC 熱敏電阻 (ADC)

// ===== AHT25 溫濕度感測器 =====
#define AHT25_ADDR 0x38
#define AHT25_CMD_INIT 0xBE
#define AHT25_CMD_TRIGGER 0xAC
#define AHT25_CMD_SOFTRESET 0xBA

// ===== 配置結構 =====
struct Config {
  float T_heat_on = 25.0f;              // 加熱啟動溫度
  float T_heat_off = 33.0f;             // 加熱關閉溫度
  float heater_max_temp = 34.0f;        // 安全停機溫度
  float H_mist_on = 55.0f;              // 噴霧啟動濕度
  float H_mist_off = 80.0f;             // 噴霧關閉濕度
  int mist_max_on_seconds = 60;         // 噴霧最大開啟時間
  int mist_min_off_seconds = 600;       // 噴霧最小關閉時間
  float ntc_low_temp_threshold = 20.0f; // NTC 低溫閾值
  int ntc_heat_on_minutes = 30;         // NTC 加熱時長
  float ntc_adc_vref = 3.3f;            // NTC ADC 參考電壓
  float ntc_temp_offset = 0.0f;         // NTC 溫度偏移
  unsigned long feed_duration_ms = 10000;      // 餵食持續時間（毫秒）
  int feed_min_interval_hours = 10;     // 兩次餵食最小間隔（小時）
  String feed_times_csv = "";           // 餵食時間排程（CSV格式：HH:MM,HH:MM）
  int upload_interval_seconds = 1800;   // 上傳間隔（秒）
  String mode = "AUTO";                 // 運作模式：AUTO/MANUAL
} config;

// ===== 狀態變數 =====
float temp_env = 0.0f;
float hum_env = 0.0f;
float temp_sub = 0.0f;
bool heater_on = false;
bool mist_on = false;
String current_mode = "AUTO";

// ===== 自動控制狀態 =====
unsigned long mist_start_time = 0;
unsigned long mist_stop_time = 0;
unsigned long heater_start_time = 0;
bool ntc_heating = false;
unsigned long ntc_heat_start_time = 0;

// ===== 步進馬達 =====
// 28BYJ-48 半步序列（8步，逆時針）
const int stepperSequence[8][4] = {
  {1, 0, 0, 0},
  {1, 1, 0, 0},
  {0, 1, 0, 0},
  {0, 1, 1, 0},
  {0, 0, 1, 0},
  {0, 0, 1, 1},
  {0, 0, 0, 1},
  {1, 0, 0, 1}
};
int stepperCurrentStep = 0;

// ===== 餵食排程 =====
struct FeedTime {
  int hour;
  int minute;
};
FeedTime feedTimes[2];
int feedTimeCount = 0;
unsigned long lastFeedTime = 0;
unsigned long nextFeedTime = 0;

// ===== 時間同步 =====
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;  // GMT+8
const int daylightOffset_sec = 0;

// ===== 上傳計時 =====
unsigned long lastUploadTime = 0;

// ===== AHT25 初始化 =====
void initAHT25() {
  Wire.beginTransmission(AHT25_ADDR);
  Wire.write(AHT25_CMD_SOFTRESET);
  Wire.endTransmission();
  delay(20);
  
  Wire.beginTransmission(AHT25_ADDR);
  Wire.write(AHT25_CMD_INIT);
  Wire.write(0x08);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(10);
}

// ===== AHT25 讀取溫濕度 =====
bool readAHT25(float* temperature, float* humidity) {
  Wire.beginTransmission(AHT25_ADDR);
  Wire.write(AHT25_CMD_TRIGGER);
  Wire.write(0x33);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(80);
  
  Wire.requestFrom(AHT25_ADDR, 6);
  if (Wire.available() < 6) return false;
  
  uint8_t data[6];
  for (int i = 0; i < 6; i++) {
    data[i] = Wire.read();
  }
  
  if ((data[0] & 0x68) == 0x08) {
    uint32_t h = ((uint32_t)data[1] << 12) | ((uint32_t)data[2] << 4) | ((uint32_t)data[3] >> 4);
    uint32_t t = (((uint32_t)data[3] & 0x0F) << 16) | ((uint32_t)data[4] << 8) | (uint32_t)data[5];
    
    *humidity = (float)h * 100.0f / 1048576.0f;
    *temperature = (float)t * 200.0f / 1048576.0f - 50.0f;
    return true;
  }
  return false;
}

// ===== NTC 溫度讀取 =====
float readNTCTempC() {
  const float ntc_r25 = 10000.0f;  // 25°C時的電阻值（10K）
  const float ntc_beta = 3950.0f;  // B值
  const float ntc_rpull = 10000.0f; // 上拉電阻（固定10K）
  
  int adc_value = analogRead(PIN_NTC);
  float voltage = (float)adc_value / 4095.0f * config.ntc_adc_vref;
  float ntc_r = ntc_rpull * voltage / (config.ntc_adc_vref - voltage);
  
  // Steinhart-Hart 
  float steinhart = log(ntc_r / ntc_r25) / ntc_beta;
  steinhart += 1.0f / (25.0f + 273.15f);
  float temp_k = 1.0f / steinhart;
  float temp_c = temp_k - 273.15f;
  
  // 應用溫度偏移
  return temp_c + config.ntc_temp_offset;
}

// ===== 步進馬達單步 =====
void stepperStep(int step) {
  step = step % 8;
  digitalWrite(PIN_STEPPER_IN1, stepperSequence[step][0]);
  digitalWrite(PIN_STEPPER_IN2, stepperSequence[step][1]);
  digitalWrite(PIN_STEPPER_IN3, stepperSequence[step][2]);
  digitalWrite(PIN_STEPPER_IN4, stepperSequence[step][3]);
}

// ===== 停止步進馬達 =====
void stepperStop() {
  digitalWrite(PIN_STEPPER_IN1, LOW);
  digitalWrite(PIN_STEPPER_IN2, LOW);
  digitalWrite(PIN_STEPPER_IN3, LOW);
  digitalWrite(PIN_STEPPER_IN4, LOW);
}

// ===== 步進馬達旋轉 =====
void stepperRotate(long steps, float stepDelayMs) {
  unsigned long absSteps = abs(steps);
  unsigned int stepDelayUs = (unsigned int)(stepDelayMs * 1000);
  
  Serial.printf("[Stepper] Start: %ld steps, %.1fms/step, CCW\n", absSteps, stepDelayMs);
  
  for (unsigned long i = 0; i < absSteps; i++) {
    stepperStep(stepperCurrentStep);
    stepperCurrentStep++;  // 正向循環（逆時針）
    if (stepperCurrentStep >= 8) stepperCurrentStep = 0;
    delayMicroseconds(stepDelayUs);
  }
  
  stepperStop();
  Serial.printf("[Stepper] Complete\n");
}

// ===== 觸發餵食 =====
void triggerFeed(bool isManual = false) {
  unsigned long now = millis();
  
  if (!isManual) {
    // 自動觸發：檢查間隔
    if (lastFeedTime > 0 && (now - lastFeedTime) < (unsigned long)config.feed_min_interval_hours * 3600000UL) {
      Serial.println("Feed ignored: too soon");
      return;
    }
    
    // 檢查與噴霧的最小間隔（4小時）
    if (mist_stop_time > 0 && (now - mist_stop_time) < 14400000UL) {
      Serial.println("Feed ignored: too soon after mist");
      return;
    }
  } else {
    Serial.println("Manual feed trigger (no restrictions)");
  }
  
  Serial.println("=== Triggering feed ===");
  Serial.printf("feed_duration_ms: %lu\n", config.feed_duration_ms);
  
  // 計算步數（28BYJ-48 約 2048 步/圈，假設每秒約667步，1.5ms/步）
  float stepDelayMs = 1.5f;
  unsigned long totalSteps = (unsigned long)(config.feed_duration_ms / stepDelayMs);
  
  // 如果時間太長，動態調整步進延遲以保持總時間
  if (totalSteps > 100000) {
    stepDelayMs = (float)config.feed_duration_ms / 100000.0f;
    totalSteps = 100000;
  }
  
  Serial.printf("Stepper rotating %lu steps (duration: %lu ms, delay: %.1f ms per step)\n", 
                totalSteps, config.feed_duration_ms, stepDelayMs);
  
  stepperRotate(totalSteps, stepDelayMs);
  
  lastFeedTime = now;
  preferences.putULong("lastFeedTime", lastFeedTime);
  
  Serial.println("=== Feed complete ===");
}

// ===== 解析餵食時間排程 =====
void parseFeedTimes() {
  feedTimeCount = 0;
  if (config.feed_times_csv.length() == 0) return;
  
  String csv = config.feed_times_csv;
  csv.replace("、", ",");
  int startPos = 0;
  
  while (startPos < csv.length() && feedTimeCount < 2) {
    int commaPos = csv.indexOf(",", startPos);
    String timeStr;
    
    if (commaPos > 0) {
      timeStr = csv.substring(startPos, commaPos);
      startPos = commaPos + 1;
    } else {
      timeStr = csv.substring(startPos);
      startPos = csv.length();
    }
    
    timeStr.trim();
    int colonPos = timeStr.indexOf(":");
    if (colonPos > 0) {
      int h = timeStr.substring(0, colonPos).toInt();
      int m = timeStr.substring(colonPos + 1).toInt();
      
      if (h >= 0 && h < 24 && m >= 0 && m < 60) {
        feedTimes[feedTimeCount].hour = h;
        feedTimes[feedTimeCount].minute = m;
        feedTimeCount++;
      }
    }
  }
  
  Serial.printf("[Feed] Parsed %d feed times\n", feedTimeCount);
}

// ===== 計算下次餵食時間 =====
void calculateNextFeedTime() {
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  
  nextFeedTime = 0;
  unsigned long nowSeconds = t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
  
  for (int i = 0; i < feedTimeCount; i++) {
    unsigned long feedSeconds = feedTimes[i].hour * 3600 + feedTimes[i].minute * 60;
    
    if (feedSeconds > nowSeconds) {
      if (nextFeedTime == 0 || feedSeconds < nextFeedTime) {
        nextFeedTime = feedSeconds;
      }
    }
  }
  
  if (nextFeedTime == 0 && feedTimeCount > 0) {
    // 今天沒有了，使用明天的第一個
    nextFeedTime = feedTimes[0].hour * 3600 + feedTimes[0].minute * 60;
  }
}

// ===== 檢查並觸發排程餵食 =====
void checkAndTriggerScheduledFeed() {
  if (feedTimeCount == 0) return;
  if (current_mode != "AUTO") return;
  
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  unsigned long nowSeconds = t->tm_hour * 3600 + t->tm_min * 60;
  
  for (int i = 0; i < feedTimeCount; i++) {
    if (t->tm_hour == feedTimes[i].hour && t->tm_min == feedTimes[i].minute) {
      static int lastTriggeredMinute = -1;
      if (t->tm_min != lastTriggeredMinute) {
        lastTriggeredMinute = t->tm_min;
        Serial.printf("[Feed] Scheduled feed triggered at %02d:%02d\n", t->tm_hour, t->tm_min);
        triggerFeed(false);
      }
      break;
    }
  }
}

// ===== 自動控制邏輯 =====
void doAutoControl() {
  if (current_mode != "AUTO") return;
  
  unsigned long now = millis();
  
  // 加熱器控制
  if (temp_env < config.T_heat_on && !heater_on) {
    heater_on = true;
    heater_start_time = now;
    ntc_heating = false;
    digitalWrite(PIN_HEATER, HIGH);
    Serial.printf("[Auto] Heater ON (temp: %.1f < %.1f)\n", temp_env, config.T_heat_on);
  } else if (heater_on) {
    // 檢查安全停機溫度
    if (temp_env >= config.heater_max_temp) {
      heater_on = false;
      digitalWrite(PIN_HEATER, LOW);
      Serial.printf("[Auto] Heater OFF (safety: %.1f >= %.1f)\n", temp_env, config.heater_max_temp);
    }
    // 檢查關閉溫度
    else if (temp_env >= config.T_heat_off) {
      heater_on = false;
      digitalWrite(PIN_HEATER, LOW);
      Serial.printf("[Auto] Heater OFF (temp: %.1f >= %.1f)\n", temp_env, config.T_heat_off);
    }
  }
  
  // NTC 低溫加熱邏輯
  if (temp_sub < config.ntc_low_temp_threshold && !ntc_heating && !heater_on) {
    ntc_heating = true;
    ntc_heat_start_time = now;
    heater_on = true;
    heater_start_time = now;
    digitalWrite(PIN_HEATER, HIGH);
    Serial.printf("[Auto] NTC Heater ON (sub temp: %.1f < %.1f)\n", temp_sub, config.ntc_low_temp_threshold);
  } else if (ntc_heating && heater_on) {
    // NTC 加熱持續到達到關閉溫度
    if (temp_sub >= config.T_heat_off) {
      ntc_heating = false;
      heater_on = false;
      digitalWrite(PIN_HEATER, LOW);
      Serial.printf("[Auto] NTC Heater OFF (sub temp: %.1f >= %.1f)\n", temp_sub, config.T_heat_off);
    }
    // 安全停機
    else if (temp_sub >= config.heater_max_temp) {
      ntc_heating = false;
      heater_on = false;
      digitalWrite(PIN_HEATER, LOW);
      Serial.printf("[Auto] NTC Heater OFF (safety: %.1f >= %.1f)\n", temp_sub, config.heater_max_temp);
    }
  }
  
  // 噴霧器控制
  if (hum_env < config.H_mist_on && !mist_on && (now - mist_stop_time) >= (unsigned long)config.mist_min_off_seconds * 1000) {
    mist_on = true;
    mist_start_time = now;
    digitalWrite(PIN_MIST, HIGH);
    Serial.printf("[Auto] Mist ON (hum: %.1f < %.1f)\n", hum_env, config.H_mist_on);
  } else if (mist_on) {
    if (hum_env >= config.H_mist_off || (now - mist_start_time) >= (unsigned long)config.mist_max_on_seconds * 1000) {
      mist_on = false;
      mist_stop_time = now;
      digitalWrite(PIN_MIST, LOW);
      Serial.printf("[Auto] Mist OFF (hum: %.1f or timeout)\n", hum_env);
    }
  }
}

// ===== 儲存配置到 NVS =====
void saveConfigToNVS() {
  preferences.begin("config", false);
  
  preferences.putFloat("T_heat_on", config.T_heat_on);
  preferences.putFloat("T_heat_off", config.T_heat_off);
  preferences.putFloat("heater_max_temp", config.heater_max_temp);
  preferences.putFloat("H_mist_on", config.H_mist_on);
  preferences.putFloat("H_mist_off", config.H_mist_off);
  preferences.putInt("mist_max_on", config.mist_max_on_seconds);
  preferences.putInt("mist_min_off", config.mist_min_off_seconds);
  preferences.putFloat("ntc_low_temp", config.ntc_low_temp_threshold);
  preferences.putInt("ntc_heat_min", config.ntc_heat_on_minutes);
  preferences.putFloat("ntc_adc_vref", config.ntc_adc_vref);
  preferences.putFloat("ntc_temp_offset", config.ntc_temp_offset);
  preferences.putULong("feed_duration", config.feed_duration_ms);
  preferences.putInt("feed_min_int", config.feed_min_interval_hours);
  preferences.putString("feed_times", config.feed_times_csv);
  preferences.putInt("upload_int", config.upload_interval_seconds);
  preferences.putString("mode", config.mode);
  
  preferences.end();
  Serial.println("[Config] Saved to NVS");
}

// ===== 從 NVS 載入配置 =====
void loadConfigFromNVS() {
  preferences.begin("config", true);
  
  config.T_heat_on = preferences.getFloat("T_heat_on", 25.0f);
  config.T_heat_off = preferences.getFloat("T_heat_off", 33.0f);
  config.heater_max_temp = preferences.getFloat("heater_max_temp", 34.0f);
  config.H_mist_on = preferences.getFloat("H_mist_on", 55.0f);
  config.H_mist_off = preferences.getFloat("H_mist_off", 80.0f);
  config.mist_max_on_seconds = preferences.getInt("mist_max_on", 60);
  config.mist_min_off_seconds = preferences.getInt("mist_min_off", 600);
  config.ntc_low_temp_threshold = preferences.getFloat("ntc_low_temp", 20.0f);
  config.ntc_heat_on_minutes = preferences.getInt("ntc_heat_min", 30);
  config.ntc_adc_vref = preferences.getFloat("ntc_adc_vref", 3.3f);
  config.ntc_temp_offset = preferences.getFloat("ntc_temp_offset", 0.0f);
  config.feed_duration_ms = preferences.getULong("feed_duration", 10000);
  config.feed_min_interval_hours = preferences.getInt("feed_min_int", 10);
  config.feed_times_csv = preferences.getString("feed_times", "");
  config.upload_interval_seconds = preferences.getInt("upload_int", 1800);
  config.mode = preferences.getString("mode", "AUTO");
  
  lastFeedTime = preferences.getULong("lastFeedTime", 0);
  
  preferences.end();
  current_mode = config.mode;
  parseFeedTimes();
  Serial.println("[Config] Loaded from NVS");
}

// ===== 發布遙測數據 =====
void publishTelemetry() {
  StaticJsonDocument<512> doc;
  doc["device"] = device_id;
  doc["ts_ms"] = millis();
  doc["temp_env"] = temp_env;
  doc["hum_env"] = hum_env;
  doc["temp_sub"] = temp_sub;
  doc["heater_on"] = heater_on;
  doc["mist_on"] = mist_on;
  doc["mode"] = current_mode;
  
  JsonObject thresholds = doc.createNestedObject("thresholds");
  thresholds["T_heat_on"] = config.T_heat_on;
  thresholds["T_heat_off"] = config.T_heat_off;
  thresholds["H_mist_on"] = config.H_mist_on;
  thresholds["H_mist_off"] = config.H_mist_off;
  thresholds["mist_max_on_seconds"] = config.mist_max_on_seconds;
  thresholds["mist_min_off_seconds"] = config.mist_min_off_seconds;
  thresholds["heater_max_temp"] = config.heater_max_temp;
  thresholds["ntc_low_temp_threshold"] = config.ntc_low_temp_threshold;
  thresholds["ntc_heat_on_minutes"] = config.ntc_heat_on_minutes;
  
  String payload;
  serializeJson(doc, payload);
  
  String topic = "farm/telemetry/" + device_id;
  client.publish(topic.c_str(), payload.c_str());
}

// ===== 發布當前配置 =====
void publishCurrentConfig() {
  StaticJsonDocument<512> doc;
  doc["device"] = device_id;
  doc["T_heat_on"] = config.T_heat_on;
  doc["T_heat_off"] = config.T_heat_off;
  doc["heater_max_temp"] = config.heater_max_temp;
  doc["H_mist_on"] = config.H_mist_on;
  doc["H_mist_off"] = config.H_mist_off;
  doc["mist_max_on_seconds"] = config.mist_max_on_seconds;
  doc["mist_min_off_seconds"] = config.mist_min_off_seconds;
  doc["ntc_low_temp_threshold"] = config.ntc_low_temp_threshold;
  doc["ntc_heat_on_minutes"] = config.ntc_heat_on_minutes;
  doc["ntc_adc_vref"] = config.ntc_adc_vref;
  doc["ntc_temp_offset"] = config.ntc_temp_offset;
  doc["feed_duration_ms"] = config.feed_duration_ms;
  doc["feed_min_interval_hours"] = config.feed_min_interval_hours;
  doc["feed_times_csv"] = config.feed_times_csv;
  doc["upload_interval_seconds"] = config.upload_interval_seconds;
  doc["mode"] = config.mode;
  
  String payload;
  serializeJson(doc, payload);
  
  String topic = "farm/config/" + device_id + "/current";
  client.publish(topic.c_str(), payload.c_str());
}

// ===== MQTT 回調 =====
void callback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String msg;
  for (int i = 0; i < length; i++) msg += (char)payload[i];
  
  Serial.printf("[MQTT] Recv topic: %s payload: %s\n", topicStr.c_str(), msg.c_str());
  
  // 配置輸入
  if (topicStr == "farm/config/" + device_id) {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, msg);
    if (error) {
      Serial.printf("[MQTT] JSON parse error: %s\n", error.c_str());
      return;
    }
    
    if (doc.containsKey("T_heat_on")) config.T_heat_on = doc["T_heat_on"];
    if (doc.containsKey("T_heat_off")) config.T_heat_off = doc["T_heat_off"];
    if (doc.containsKey("heater_max_temp")) config.heater_max_temp = doc["heater_max_temp"];
    if (doc.containsKey("H_mist_on")) config.H_mist_on = doc["H_mist_on"];
    if (doc.containsKey("H_mist_off")) config.H_mist_off = doc["H_mist_off"];
    if (doc.containsKey("mist_max_on_seconds")) config.mist_max_on_seconds = doc["mist_max_on_seconds"];
    if (doc.containsKey("mist_min_off_seconds")) config.mist_min_off_seconds = doc["mist_min_off_seconds"];
    if (doc.containsKey("ntc_low_temp_threshold")) config.ntc_low_temp_threshold = doc["ntc_low_temp_threshold"];
    if (doc.containsKey("ntc_heat_on_minutes")) config.ntc_heat_on_minutes = doc["ntc_heat_on_minutes"];
    if (doc.containsKey("ntc_adc_vref")) config.ntc_adc_vref = doc["ntc_adc_vref"];
    if (doc.containsKey("ntc_temp_offset")) config.ntc_temp_offset = doc["ntc_temp_offset"];
    if (doc.containsKey("feed_duration_ms")) config.feed_duration_ms = doc["feed_duration_ms"];
    if (doc.containsKey("feed_min_interval_hours")) {
      config.feed_min_interval_hours = doc["feed_min_interval_hours"];
      parseFeedTimes();
    }
    if (doc.containsKey("feed_times_csv")) {
      config.feed_times_csv = doc["feed_times_csv"].as<String>();
      parseFeedTimes();
    }
    if (doc.containsKey("upload_interval_seconds")) config.upload_interval_seconds = doc["upload_interval_seconds"];
    if (doc.containsKey("mode")) {
      config.mode = doc["mode"].as<String>();
      current_mode = config.mode;
    }
    
    saveConfigToNVS();
    publishCurrentConfig();
    Serial.println("[MQTT] Config updated");
  }
  
  // 設備控制
  else if (topicStr == "farm/control/" + device_id + "/heater") {
    if (msg == "ON") {
      heater_on = true;
      digitalWrite(PIN_HEATER, HIGH);
    } else if (msg == "OFF") {
      heater_on = false;
      digitalWrite(PIN_HEATER, LOW);
    }
  }
  else if (topicStr == "farm/control/" + device_id + "/mist") {
    if (msg == "ON") {
      mist_on = true;
      digitalWrite(PIN_MIST, HIGH);
    } else if (msg == "OFF") {
      mist_on = false;
      digitalWrite(PIN_MIST, LOW);
    }
  }
  else if (topicStr == "farm/control/" + device_id + "/feed") {
    if (msg == "TRIGGER") {
      triggerFeed(true);  // 手動觸發，無限制
    }
  }
  else if (topicStr == "farm/control/" + device_id + "/mode") {
    if (msg == "AUTO" || msg == "MANUAL") {
      config.mode = msg;
      current_mode = msg;
      saveConfigToNVS();
    }
  }
}

// ===== NTP 時間同步 =====
void setupNTP() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("[NTP] Waiting for time sync...");
    delay(1000);
  }
  Serial.println("[NTP] Time synced");
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("==== BarleyBox System Start ====");
  
  // GPIO 初始化
  pinMode(PIN_HEATER, OUTPUT);
  pinMode(PIN_MIST, OUTPUT);
  pinMode(PIN_STEPPER_IN1, OUTPUT);
  pinMode(PIN_STEPPER_IN2, OUTPUT);
  pinMode(PIN_STEPPER_IN3, OUTPUT);
  pinMode(PIN_STEPPER_IN4, OUTPUT);
  pinMode(PIN_NTC, INPUT);
  
  digitalWrite(PIN_HEATER, LOW);
  digitalWrite(PIN_MIST, LOW);
  stepperStop();
  
  // I2C 初始化（AHT25）
  Wire.begin();
  delay(100);
  initAHT25();
  
  // 載入配置
  loadConfigFromNVS();
  
  // WiFi 連接
  Serial.printf("[WiFi] Connecting to %s...\n", ssid);
  WiFi.begin(ssid, password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    setupNTP();
    
    // MQTT 連接
    client.setServer(mqtt_server, MQTT_PORT);
    client.setCallback(callback);
    
    String clientId = "ESP32-" + device_id + "-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str())) {
      Serial.println("[MQTT] Connected");
      
      String subTopics[] = {
        "farm/config/" + device_id,
        "farm/control/" + device_id + "/heater",
        "farm/control/" + device_id + "/mist",
        "farm/control/" + device_id + "/feed",
        "farm/control/" + device_id + "/mode"
      };
      
      for (int i = 0; i < 5; i++) {
        client.subscribe(subTopics[i].c_str());
      }
      
      publishCurrentConfig();
    }
  } else {
    Serial.println("\n[WiFi] Connection failed!");
  }
  
  Serial.println("==== System Ready ====");
}

// ===== Loop =====
void loop() {
  static unsigned long lastSensorRead = 0;
  static unsigned long lastTelemetryPublish = 0;
  
  // MQTT 處理
  if (WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      String clientId = "ESP32-" + device_id + "-" + String(random(0xffff), HEX);
      if (client.connect(clientId.c_str())) {
        String subTopics[] = {
          "farm/config/" + device_id,
          "farm/control/" + device_id + "/heater",
          "farm/control/" + device_id + "/mist",
          "farm/control/" + device_id + "/feed",
          "farm/control/" + device_id + "/mode"
        };
        for (int i = 0; i < 5; i++) {
          client.subscribe(subTopics[i].c_str());
        }
      }
    } else {
      client.loop();
    }
  }
  
  // 讀取感測器（每秒）
  unsigned long now = millis();
  if (now - lastSensorRead >= 1000) {
    lastSensorRead = now;
    
    if (readAHT25(&temp_env, &hum_env)) {
      temp_sub = readNTCTempC();
      doAutoControl();
    }
  }
  
  // 發布遙測數據（每秒）
  if (now - lastTelemetryPublish >= 1000) {
    lastTelemetryPublish = now;
    if (client.connected()) {
      publishTelemetry();
    }
  }
  
  // 檢查排程餵食
  checkAndTriggerScheduledFeed();
  
  delay(10);
}
