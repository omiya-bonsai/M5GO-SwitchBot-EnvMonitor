#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

#include "config.h"

// ============================================================================
// Display
// ============================================================================

constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;

// Omarchy-inspired RGB565 palette
constexpr uint16_t COLOR_BG = 0x1082;
constexpr uint16_t COLOR_PANEL = 0x18E3;
constexpr uint16_t COLOR_TEXT = 0xE71C;
constexpr uint16_t COLOR_MUTED = 0x8410;
constexpr uint16_t COLOR_CYAN = 0x05DB;
constexpr uint16_t COLOR_ORANGE = 0xFC60;
constexpr uint16_t COLOR_PURPLE = 0xB29F;
constexpr uint16_t COLOR_GREEN = 0x5E8B;
constexpr uint16_t COLOR_YELLOW = 0xDDA0;
constexpr uint16_t COLOR_RED = 0xE986;
constexpr uint16_t COLOR_DIVIDER = 0x3186;

// ============================================================================
// Timing
// ============================================================================

constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;
constexpr unsigned long MQTT_RETRY_INTERVAL_MS = 5000;
constexpr unsigned long DISPLAY_INTERVAL_MS = 1000;

// These are provisional.
// Adjust after observing the real SwitchBot update interval.
constexpr unsigned long DATA_STALE_SEC = 180;
constexpr unsigned long DATA_OFFLINE_SEC = 600;

// ============================================================================
// Runtime state
// ============================================================================

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

float temperature = NAN;
float humidity = NAN;

bool hasSensorData = false;
bool ntpReady = false;

time_t lastDataReceivedAt = 0;

unsigned long lastWifiAttemptMs = 0;
unsigned long lastMqttAttemptMs = 0;
unsigned long lastDisplayMs = 0;

// ============================================================================
// Forward declarations
// ============================================================================

void connectWiFi();
void maintainWiFi();

void setupTime();
void updateTimeState();

void setupMQTT();
void maintainMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);

void drawScreen();
void drawHeader();
void drawEnvironment();
void drawFooter();

String getTimeString();
String getDateString();
String getDataAgeString();
String getDataStateString();

uint16_t getDataStateColor();

bool isTimeValid();

// ============================================================================
// Setup
// ============================================================================

void setup() {
  M5.begin();

  Serial.begin(115200);
  delay(100);

  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(COLOR_BG);
  M5.Lcd.setTextWrap(false);

  Serial.println();
  Serial.println("================================");
  Serial.println(" M5GO SwitchBot Monitor");
  Serial.println("================================");

  // --------------------------------------------------------------------------
  // Initial display
  // --------------------------------------------------------------------------

  M5.Lcd.setTextColor(COLOR_CYAN, COLOR_BG);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setCursor(16, 20);
  M5.Lcd.println("ENV / B3D8");

  M5.Lcd.setTextColor(COLOR_MUTED, COLOR_BG);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setCursor(16, 52);
  M5.Lcd.println("starting...");

  // --------------------------------------------------------------------------
  // Wi-Fi
  // --------------------------------------------------------------------------

  connectWiFi();

  // --------------------------------------------------------------------------
  // NTP
  // --------------------------------------------------------------------------

  setupTime();

  // --------------------------------------------------------------------------
  // MQTT
  // --------------------------------------------------------------------------

  setupMQTT();

  drawScreen();
}

// ============================================================================
// Main loop
// ============================================================================

void loop() {
  M5.update();

  maintainWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    updateTimeState();
    maintainMQTT();

    if (mqttClient.connected()) {
      mqttClient.loop();
    }
  }

  const unsigned long nowMs = millis();

  if (nowMs - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    lastDisplayMs = nowMs;
    drawScreen();
  }

  delay(5);
}

// ============================================================================
// Wi-Fi
// ============================================================================

void connectWiFi() {
  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long startedAt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: connected");
    Serial.print("WiFi: IP = ");
    Serial.println(WiFi.localIP());

    lastWifiAttemptMs = millis();
    return;
  }

  Serial.println("WiFi: initial connection failed");
  lastWifiAttemptMs = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const unsigned long nowMs = millis();

  if (nowMs - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiAttemptMs = nowMs;

  Serial.println("WiFi: reconnecting...");

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ============================================================================
// NTP
// ============================================================================

void setupTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("NTP: WiFi unavailable, deferred");
    return;
  }

  Serial.println("NTP: configuring");

  configTzTime(
    TZ_INFO,
    NTP_SERVER_1,
    NTP_SERVER_2,
    NTP_SERVER_3);

  // Do not block indefinitely.
  // Give SNTP a short opportunity to synchronize.
  struct tm timeInfo;

  for (int i = 0; i < 20; ++i) {
    if (getLocalTime(&timeInfo, 250)) {
      ntpReady = true;

      Serial.printf(
        "NTP: synchronized %04d-%02d-%02d %02d:%02d:%02d\n",
        timeInfo.tm_year + 1900,
        timeInfo.tm_mon + 1,
        timeInfo.tm_mday,
        timeInfo.tm_hour,
        timeInfo.tm_min,
        timeInfo.tm_sec);

      return;
    }

    delay(250);
  }

  Serial.println("NTP: synchronization pending");
}

void updateTimeState() {
  if (ntpReady) {
    return;
  }

  if (isTimeValid()) {
    ntpReady = true;
    Serial.println("NTP: time became valid");
  }
}

bool isTimeValid() {
  const time_t now = time(nullptr);

  // Anything after 2024-01-01 is sufficient for this device.
  return now > 1704067200;
}

// ============================================================================
// MQTT
// ============================================================================

void setupMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  maintainMQTT();
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (mqttClient.connected()) {
    return;
  }

  const unsigned long nowMs = millis();

  if (nowMs - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) {
    return;
  }

  lastMqttAttemptMs = nowMs;

  Serial.printf(
    "MQTT: connecting to %s:%u\n",
    MQTT_HOST,
    MQTT_PORT);

  bool connected = false;

  if (strlen(MQTT_USERNAME) > 0) {
    connected = mqttClient.connect(
      MQTT_CLIENT_ID,
      MQTT_USERNAME,
      MQTT_PASSWORD);
  } else {
    connected = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (!connected) {
    Serial.printf(
      "MQTT: connection failed, state=%d\n",
      mqttClient.state());
    return;
  }

  Serial.println("MQTT: connected");

  if (mqttClient.subscribe(MQTT_TOPIC_ENV, 0)) {
    Serial.printf(
      "MQTT: subscribed %s\n",
      MQTT_TOPIC_ENV);
  } else {
    Serial.println("MQTT: subscribe failed");
  }
}

// ============================================================================
// MQTT callback
// ============================================================================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length) {
  Serial.printf(
    "MQTT: received topic=%s length=%u\n",
    topic,
    length);

  if (strcmp(topic, MQTT_TOPIC_ENV) != 0) {
    return;
  }

  JsonDocument doc;

  DeserializationError error =
    deserializeJson(doc, payload, length);

  if (error) {
    Serial.print("JSON: parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  if (!doc["temperature"].is<float>() && !doc["temperature"].is<int>()) {
    Serial.println("JSON: temperature missing");
    return;
  }

  if (!doc["humidity"].is<float>() && !doc["humidity"].is<int>()) {
    Serial.println("JSON: humidity missing");
    return;
  }

  const float newTemperature =
    doc["temperature"].as<float>();

  const float newHumidity =
    doc["humidity"].as<float>();

  // Basic sanity checking.
  if (newTemperature < -50.0f || newTemperature > 80.0f) {
    Serial.println("JSON: temperature out of range");
    return;
  }

  if (newHumidity < 0.0f || newHumidity > 100.0f) {
    Serial.println("JSON: humidity out of range");
    return;
  }

  temperature = newTemperature;
  humidity = newHumidity;

  hasSensorData = true;

  if (isTimeValid()) {
    lastDataReceivedAt = time(nullptr);
  } else {
    lastDataReceivedAt = 0;
  }

  Serial.printf(
    "ENV: temperature=%.1f humidity=%.1f\n",
    temperature,
    humidity);

  // Refresh immediately rather than waiting up to one second.
  drawScreen();
}

// ============================================================================
// Main display
// ============================================================================

void drawScreen() {
  M5.Lcd.fillScreen(COLOR_BG);

  drawHeader();
  drawEnvironment();
  drawFooter();
}

// ============================================================================
// Header
// ============================================================================

void drawHeader() {
  // Device title
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(COLOR_CYAN, COLOR_BG);

  M5.Lcd.drawString(
    "ENV / B3D8",
    14,
    12,
    2);

  // Date + time
  M5.Lcd.setTextDatum(TR_DATUM);
  M5.Lcd.setTextColor(COLOR_TEXT, COLOR_BG);

  String clockText;

  if (isTimeValid()) {
    clockText =
      getDateString() + "  " + getTimeString();
  } else {
    clockText = "--/--  --:--";
  }

  M5.Lcd.drawString(
    clockText,
    SCREEN_W - 14,
    12,
    2);

  // Divider
  M5.Lcd.drawFastHLine(
    14,
    37,
    SCREEN_W - 28,
    COLOR_DIVIDER);
}

// ============================================================================
// Environment
// ============================================================================

void drawEnvironment() {
  // --------------------------------------------------------------------------
  // Temperature label
  // --------------------------------------------------------------------------

  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextColor(COLOR_MUTED, COLOR_BG);

  M5.Lcd.drawString(
    "TEMPERATURE",
    18,
    53,
    2);

  // --------------------------------------------------------------------------
  // Temperature value
  // --------------------------------------------------------------------------

  M5.Lcd.setTextColor(COLOR_ORANGE, COLOR_BG);

  if (hasSensorData && !isnan(temperature)) {
    char buffer[24];

    snprintf(
      buffer,
      sizeof(buffer),
      "%.1f C",
      temperature);

    M5.Lcd.drawString(
      buffer,
      18,
      72,
      6);
  } else {
    M5.Lcd.drawString(
      "--.- C",
      18,
      72,
      6);
  }

  // --------------------------------------------------------------------------
  // Humidity label
  // --------------------------------------------------------------------------

  M5.Lcd.setTextColor(COLOR_MUTED, COLOR_BG);

  M5.Lcd.drawString(
    "HUMIDITY",
    18,
    132,
    2);

  // --------------------------------------------------------------------------
  // Humidity value
  // --------------------------------------------------------------------------

  M5.Lcd.setTextColor(COLOR_CYAN, COLOR_BG);

  if (hasSensorData && !isnan(humidity)) {
    char buffer[24];

    snprintf(
      buffer,
      sizeof(buffer),
      "%.0f %%",
      humidity);

    M5.Lcd.drawString(
      buffer,
      18,
      151,
      6);
  } else {
    M5.Lcd.drawString(
      "-- %",
      18,
      151,
      6);
  }
}

// ============================================================================
// Footer
// ============================================================================

void drawFooter() {
  const int dividerY = 207;

  M5.Lcd.drawFastHLine(
    14,
    dividerY,
    SCREEN_W - 28,
    COLOR_DIVIDER);

  // --------------------------------------------------------------------------
  // MQTT indicator
  // --------------------------------------------------------------------------

  M5.Lcd.setTextDatum(TL_DATUM);

  uint16_t mqttColor =
    mqttClient.connected()
      ? COLOR_GREEN
      : COLOR_RED;

  M5.Lcd.fillCircle(
    18,
    224,
    4,
    mqttColor);

  M5.Lcd.setTextColor(COLOR_MUTED, COLOR_BG);

  M5.Lcd.drawString(
    "MQTT",
    28,
    216,
    2);

  // --------------------------------------------------------------------------
  // Data state
  // --------------------------------------------------------------------------

  M5.Lcd.setTextColor(
    getDataStateColor(),
    COLOR_BG);

  M5.Lcd.drawString(
    getDataStateString(),
    82,
    216,
    2);

  // --------------------------------------------------------------------------
  // Age
  // --------------------------------------------------------------------------

  M5.Lcd.setTextDatum(TR_DATUM);
  M5.Lcd.setTextColor(COLOR_MUTED, COLOR_BG);

  M5.Lcd.drawString(
    getDataAgeString(),
    SCREEN_W - 14,
    216,
    2);
}

// ============================================================================
// Time formatting
// ============================================================================

String getTimeString() {
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 10)) {
    return "--:--";
  }

  char buffer[8];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02d:%02d",
    timeInfo.tm_hour,
    timeInfo.tm_min);

  return String(buffer);
}

String getDateString() {
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 10)) {
    return "--/--";
  }

  char buffer[8];

  snprintf(
    buffer,
    sizeof(buffer),
    "%02d/%02d",
    timeInfo.tm_mon + 1,
    timeInfo.tm_mday);

  return String(buffer);
}

// ============================================================================
// Data freshness
// ============================================================================

String getDataStateString() {
  if (!hasSensorData) {
    return "WAIT";
  }

  if (!isTimeValid() || lastDataReceivedAt == 0) {
    return "LIVE";
  }

  const time_t now = time(nullptr);

  if (now < lastDataReceivedAt) {
    return "LIVE";
  }

  const unsigned long age =
    static_cast<unsigned long>(
      now - lastDataReceivedAt);

  if (age < DATA_STALE_SEC) {
    return "LIVE";
  }

  if (age < DATA_OFFLINE_SEC) {
    return "STALE";
  }

  return "OFFLINE";
}

uint16_t getDataStateColor() {
  const String state = getDataStateString();

  if (state == "LIVE") {
    return COLOR_GREEN;
  }

  if (state == "STALE") {
    return COLOR_YELLOW;
  }

  if (state == "OFFLINE") {
    return COLOR_RED;
  }

  return COLOR_PURPLE;
}

String getDataAgeString() {
  if (!hasSensorData) {
    return "waiting";
  }

  if (!isTimeValid() || lastDataReceivedAt == 0) {
    return "received";
  }

  const time_t now = time(nullptr);

  if (now < lastDataReceivedAt) {
    return "received";
  }

  const unsigned long age =
    static_cast<unsigned long>(
      now - lastDataReceivedAt);

  if (age < 60) {
    return String(age) + "s ago";
  }

  if (age < 3600) {
    return String(age / 60) + "m ago";
  }

  if (age < 86400) {
    return String(age / 3600) + "h ago";
  }

  return String(age / 86400) + "d ago";
}