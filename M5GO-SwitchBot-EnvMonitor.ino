#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>

#include "config.h"

// ============================================================================
// Application
// ============================================================================

static constexpr char APP_NAME[] = "M5GO-SwitchBot-EnvMonitor";
static constexpr char APP_VERSION[] = "0.2.0";

// ============================================================================
// Display
// ============================================================================

static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;

// Omarchy-inspired palette (RGB565)
static constexpr uint16_t COLOR_BG = 0x1082;
static constexpr uint16_t COLOR_PANEL = 0x18E3;
static constexpr uint16_t COLOR_TEXT = 0xE71C;
static constexpr uint16_t COLOR_MUTED = 0x8410;
static constexpr uint16_t COLOR_CYAN = 0x05DB;
static constexpr uint16_t COLOR_ORANGE = 0xFC60;
static constexpr uint16_t COLOR_PURPLE = 0xB29F;
static constexpr uint16_t COLOR_GREEN = 0x5E8B;
static constexpr uint16_t COLOR_YELLOW = 0xDDA0;
static constexpr uint16_t COLOR_RED = 0xE986;
static constexpr uint16_t COLOR_DIVIDER = 0x3186;

// Full-screen off-screen buffer.
//
// Everything is rendered here first and transferred to the LCD in one pass.
// This eliminates most of the visible flicker caused by repeatedly clearing
// and drawing directly to the physical LCD.
M5Canvas canvas(&M5.Display);

// ============================================================================
// Timing
// ============================================================================

static constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
static constexpr uint32_t MQTT_RETRY_INTERVAL_MS = 5000;

// One frame per second is sufficient because the only continuously changing
// information is the clock/data age.
static constexpr uint32_t DISPLAY_INTERVAL_MS = 1000;

// Provisional freshness thresholds.
//
// These describe how long it has been since this M5GO received the MQTT
// message. Later we can use a timestamp supplied by Home Assistant instead.
static constexpr uint32_t DATA_STALE_SEC = 180;
static constexpr uint32_t DATA_OFFLINE_SEC = 600;

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

uint32_t lastWifiAttemptMs = 0;
uint32_t lastMqttAttemptMs = 0;
uint32_t lastDisplayMs = 0;

// ============================================================================
// Forward declarations
// ============================================================================

void connectWiFi();
void maintainWiFi();

void setupTime();
void updateTimeState();
bool isTimeValid();

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

// ============================================================================
// Setup
// ============================================================================

void setup() {
  // --------------------------------------------------------------------------
  // M5Unified
  // --------------------------------------------------------------------------

  auto cfg = M5.config();

  M5.begin(cfg);

  // Landscape 320 x 240.
  M5.Display.setRotation(1);
  M5.Display.setBrightness(160);

  // --------------------------------------------------------------------------
  // Serial
  // --------------------------------------------------------------------------

  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("========================================");
  Serial.printf(" %s\n", APP_NAME);
  Serial.printf(" version %s\n", APP_VERSION);
  Serial.println("========================================");

  Serial.printf(
    "Display: %d x %d\n",
    M5.Display.width(),
    M5.Display.height());

  // --------------------------------------------------------------------------
  // Canvas
  // --------------------------------------------------------------------------

  canvas.setColorDepth(16);

  if (canvas.createSprite(SCREEN_W, SCREEN_H) == nullptr) {
    Serial.println("ERROR: failed to create display canvas");

    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_RED);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 10);
    M5.Display.println("Canvas allocation");
    M5.Display.println("failed.");

    while (true) {
      delay(1000);
    }
  }

  canvas.setTextWrap(false);

  drawScreen();

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

  const uint32_t nowMs = millis();

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

  const uint32_t startedAt = millis();

  while (
    WiFi.status() != WL_CONNECTED && millis() - startedAt < 15000) {
    delay(250);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi: connected");

    Serial.print("WiFi: IP = ");
    Serial.println(WiFi.localIP());

    Serial.printf(
      "WiFi: RSSI = %d dBm\n",
      WiFi.RSSI());

    lastWifiAttemptMs = millis();

    drawScreen();
    return;
  }

  Serial.println("WiFi: initial connection failed");

  lastWifiAttemptMs = millis();

  drawScreen();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t nowMs = millis();

  if (
    nowMs - lastWifiAttemptMs < WIFI_RETRY_INTERVAL_MS) {
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

  struct tm timeInfo;

  // Give SNTP a short opportunity to synchronize at startup,
  // but never block forever.
  for (int i = 0; i < 20; ++i) {
    if (getLocalTime(&timeInfo, 250)) {
      ntpReady = true;

      Serial.printf(
        "NTP: synchronized %04d-%02d-%02d "
        "%02d:%02d:%02d\n",
        timeInfo.tm_year + 1900,
        timeInfo.tm_mon + 1,
        timeInfo.tm_mday,
        timeInfo.tm_hour,
        timeInfo.tm_min,
        timeInfo.tm_sec);

      drawScreen();
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

    drawScreen();
  }
}

bool isTimeValid() {
  const time_t now = time(nullptr);

  // 2024-01-01 00:00:00 UTC
  return now > 1704067200;
}

// ============================================================================
// MQTT
// ============================================================================

void setupMQTT() {
  mqttClient.setServer(
    MQTT_HOST,
    MQTT_PORT);

  mqttClient.setCallback(mqttCallback);

  // More than enough for the small environment JSON.
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

  const uint32_t nowMs = millis();

  if (
    nowMs - lastMqttAttemptMs < MQTT_RETRY_INTERVAL_MS) {
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
    connected = mqttClient.connect(
      MQTT_CLIENT_ID);
  }

  if (!connected) {
    Serial.printf(
      "MQTT: connection failed, state=%d\n",
      mqttClient.state());

    drawScreen();
    return;
  }

  Serial.println("MQTT: connected");

  if (
    mqttClient.subscribe(
      MQTT_TOPIC_ENV,
      0)) {
    Serial.printf(
      "MQTT: subscribed %s\n",
      MQTT_TOPIC_ENV);
  } else {
    Serial.println("MQTT: subscribe failed");
  }

  drawScreen();
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

  if (
    strcmp(
      topic,
      MQTT_TOPIC_ENV)
    != 0) {
    return;
  }

  JsonDocument doc;

  const DeserializationError error =
    deserializeJson(
      doc,
      payload,
      length);

  if (error) {
    Serial.print("JSON: parse failed: ");
    Serial.println(error.c_str());
    return;
  }

  if (
    !doc["temperature"].is<float>() && !doc["temperature"].is<int>()) {
    Serial.println(
      "JSON: temperature missing");
    return;
  }

  if (
    !doc["humidity"].is<float>() && !doc["humidity"].is<int>()) {
    Serial.println(
      "JSON: humidity missing");
    return;
  }

  const float newTemperature =
    doc["temperature"].as<float>();

  const float newHumidity =
    doc["humidity"].as<float>();

  // --------------------------------------------------------------------------
  // Sanity checks
  // --------------------------------------------------------------------------

  if (
    newTemperature < -50.0f || newTemperature > 80.0f) {
    Serial.println(
      "JSON: temperature out of range");
    return;
  }

  if (
    newHumidity < 0.0f || newHumidity > 100.0f) {
    Serial.println(
      "JSON: humidity out of range");
    return;
  }

  // --------------------------------------------------------------------------
  // Update state
  // --------------------------------------------------------------------------

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

  // Immediate visual update.
  drawScreen();
}

// ============================================================================
// Display
// ============================================================================

void drawScreen() {
  // IMPORTANT:
  //
  // We clear the OFF-SCREEN canvas, not the physical LCD.
  // The completed frame is pushed to the LCD only once at the end.

  canvas.fillSprite(COLOR_BG);

  drawHeader();
  drawEnvironment();
  drawFooter();

  // One physical display transfer per frame.
  canvas.pushSprite(0, 0);
}

// ============================================================================
// Header
// ============================================================================

void drawHeader() {
  // --------------------------------------------------------------------------
  // Application / sensor name
  // --------------------------------------------------------------------------

  canvas.setTextDatum(middle_left);
  canvas.setTextColor(COLOR_CYAN);

  canvas.setFont(
    &fonts::FreeMonoBold9pt7b);

  canvas.drawString(
    "ENV / B3D8",
    14,
    21);

  // --------------------------------------------------------------------------
  // Date and time
  // --------------------------------------------------------------------------

  canvas.setTextDatum(middle_right);
  canvas.setTextColor(COLOR_TEXT);

  canvas.setFont(
    &fonts::FreeMono9pt7b);

  String clockText;

  if (isTimeValid()) {
    clockText =
      getDateString() + " " + getTimeString();
  } else {
    clockText =
      "--/-- --:--";
  }

  canvas.drawString(
    clockText,
    SCREEN_W - 14,
    21);

  // --------------------------------------------------------------------------
  // Divider
  // --------------------------------------------------------------------------

  canvas.drawFastHLine(
    14,
    39,
    SCREEN_W - 28,
    COLOR_DIVIDER);
}

// ============================================================================
// Main environment area
// ============================================================================

void drawEnvironment() {
  // Layout:
  //
  //     TEMPERATURE       HUMIDITY
  //
  //        24.9              62
  //         °C                %
  //
  // The main numeric area uses almost the full width of the M5GO.

  static constexpr int LEFT_CENTER_X = 82;
  static constexpr int RIGHT_CENTER_X = 238;

  static constexpr int LABEL_Y = 61;
  static constexpr int VALUE_Y = 119;
  static constexpr int UNIT_Y = 174;

  // --------------------------------------------------------------------------
  // Vertical separator
  // --------------------------------------------------------------------------

  canvas.drawFastVLine(
    SCREEN_W / 2,
    51,
    137,
    COLOR_DIVIDER);

  // --------------------------------------------------------------------------
  // Labels
  // --------------------------------------------------------------------------

  canvas.setTextDatum(middle_center);

  canvas.setFont(
    &fonts::FreeMonoBold9pt7b);

  canvas.setTextColor(
    COLOR_MUTED);

  canvas.drawString(
    "TEMPERATURE",
    LEFT_CENTER_X,
    LABEL_Y);

  canvas.drawString(
    "HUMIDITY",
    RIGHT_CENTER_X,
    LABEL_Y);

  // --------------------------------------------------------------------------
  // Temperature value
  // --------------------------------------------------------------------------

  canvas.setTextColor(
    COLOR_ORANGE);

  canvas.setFont(
    &fonts::FreeMonoBold24pt7b);

  if (
    hasSensorData && !isnan(temperature)) {
    char buffer[16];

    snprintf(
      buffer,
      sizeof(buffer),
      "%.1f",
      temperature);

    canvas.drawString(
      buffer,
      LEFT_CENTER_X,
      VALUE_Y);
  } else {
    canvas.drawString(
      "--.-",
      LEFT_CENTER_X,
      VALUE_Y);
  }

  // --------------------------------------------------------------------------
  // Humidity value
  // --------------------------------------------------------------------------

  canvas.setTextColor(
    COLOR_CYAN);

  canvas.setFont(
    &fonts::FreeMonoBold24pt7b);

  if (
    hasSensorData && !isnan(humidity)) {
    char buffer[16];

    snprintf(
      buffer,
      sizeof(buffer),
      "%.0f",
      humidity);

    canvas.drawString(
      buffer,
      RIGHT_CENTER_X,
      VALUE_Y);
  } else {
    canvas.drawString(
      "--",
      RIGHT_CENTER_X,
      VALUE_Y);
  }

  // --------------------------------------------------------------------------
  // Units
  //
  // "C" is used for now instead of the degree symbol because the final
  // JetBrains Mono font subset has not yet been embedded.
  // --------------------------------------------------------------------------

  canvas.setFont(
    &fonts::FreeMonoBold12pt7b);

  canvas.setTextColor(
    COLOR_ORANGE);

  canvas.drawString(
    "C",
    LEFT_CENTER_X,
    UNIT_Y);

  canvas.setTextColor(
    COLOR_CYAN);

  canvas.drawString(
    "%",
    RIGHT_CENTER_X,
    UNIT_Y);
}

// ============================================================================
// Footer
// ============================================================================

void drawFooter() {
  static constexpr int DIVIDER_Y = 195;
  static constexpr int FOOTER_Y = 218;

  // --------------------------------------------------------------------------
  // Divider
  // --------------------------------------------------------------------------

  canvas.drawFastHLine(
    14,
    DIVIDER_Y,
    SCREEN_W - 28,
    COLOR_DIVIDER);

  // --------------------------------------------------------------------------
  // MQTT indicator
  // --------------------------------------------------------------------------

  const uint16_t mqttColor =
    mqttClient.connected()
      ? COLOR_GREEN
      : COLOR_RED;

  canvas.fillCircle(
    18,
    FOOTER_Y,
    4,
    mqttColor);

  canvas.setFont(
    &fonts::FreeMono9pt7b);

  canvas.setTextDatum(
    middle_left);

  canvas.setTextColor(
    COLOR_MUTED);

  canvas.drawString(
    "MQTT",
    28,
    FOOTER_Y);

  // --------------------------------------------------------------------------
  // Data freshness state
  // --------------------------------------------------------------------------

  canvas.setFont(
    &fonts::FreeMonoBold9pt7b);

  canvas.setTextColor(
    getDataStateColor());

  canvas.drawString(
    getDataStateString(),
    83,
    FOOTER_Y);

  // --------------------------------------------------------------------------
  // Data age
  // --------------------------------------------------------------------------

  canvas.setFont(
    &fonts::FreeMono9pt7b);

  canvas.setTextDatum(
    middle_right);

  canvas.setTextColor(
    COLOR_MUTED);

  canvas.drawString(
    getDataAgeString(),
    SCREEN_W - 14,
    FOOTER_Y);
}

// ============================================================================
// Date / time formatting
// ============================================================================

String getTimeString() {
  struct tm timeInfo;

  if (
    !getLocalTime(
      &timeInfo,
      10)) {
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

  if (
    !getLocalTime(
      &timeInfo,
      10)) {
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

  if (
    !isTimeValid() || lastDataReceivedAt == 0) {
    return "LIVE";
  }

  const time_t now =
    time(nullptr);

  if (now < lastDataReceivedAt) {
    return "LIVE";
  }

  const uint32_t age =
    static_cast<uint32_t>(
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
  const String state =
    getDataStateString();

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

  if (
    !isTimeValid() || lastDataReceivedAt == 0) {
    return "received";
  }

  const time_t now =
    time(nullptr);

  if (now < lastDataReceivedAt) {
    return "received";
  }

  const uint32_t age =
    static_cast<uint32_t>(
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