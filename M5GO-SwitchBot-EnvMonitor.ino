#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "config.h"

#if __has_include("JetBrainsMono9pt7b.h") && __has_include("JetBrainsMono12pt7b.h") && __has_include("JetBrainsMono24pt7b.h")
#include "JetBrainsMono9pt7b.h"
#include "JetBrainsMono12pt7b.h"
#include "JetBrainsMono24pt7b.h"
#define HAVE_JETBRAINS_MONO 1
#else
#define HAVE_JETBRAINS_MONO 0
#endif

// =============================================================================
// Application
// =============================================================================

namespace App {
constexpr char NAME[] = "M5GO-SwitchBot-EnvMonitor";
constexpr char VERSION[] = "0.4.0";

constexpr int SCREEN_WIDTH = 320;
constexpr int SCREEN_HEIGHT = 240;

constexpr uint32_t DISPLAY_SLEEP_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t DISPLAY_REFRESH_MS = 1000UL;
constexpr uint32_t LONG_PRESS_MS = 800UL;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MIN_MS = 5UL * 1000UL;
constexpr uint32_t WIFI_RETRY_MAX_MS = 60UL * 1000UL;

constexpr uint32_t MQTT_RETRY_MIN_MS = 2UL * 1000UL;
constexpr uint32_t MQTT_RETRY_MAX_MS = 60UL * 1000UL;
constexpr uint16_t MQTT_KEEPALIVE_SEC = 30;
constexpr uint16_t MQTT_SOCKET_TIMEOUT_SEC = 5;

constexpr uint32_t DATA_STALE_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t DATA_OFFLINE_MS = 10UL * 60UL * 1000UL;

constexpr uint32_t HEALTH_LOG_INTERVAL_MS = 5UL * 60UL * 1000UL;

constexpr uint8_t BRIGHTNESS_LEVELS[] = { 40, 80, 120, 160, 220 };
constexpr size_t BRIGHTNESS_LEVEL_COUNT =
  sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]);
constexpr uint8_t DEFAULT_BRIGHTNESS_INDEX = 3;
}  // namespace App

// =============================================================================
// Colors
// =============================================================================

namespace Color {
constexpr uint16_t BG = 0x1082;
constexpr uint16_t TEXT = 0xE71C;
constexpr uint16_t MUTED = 0x8410;
constexpr uint16_t CYAN = 0x05DB;
constexpr uint16_t ORANGE = 0xFC60;
constexpr uint16_t PURPLE = 0xB29F;
constexpr uint16_t GREEN = 0x5E8B;
constexpr uint16_t YELLOW = 0xDDA0;
constexpr uint16_t RED = 0xE986;
constexpr uint16_t DIVIDER = 0x3186;
}  // namespace Color

// =============================================================================
// Types
// =============================================================================

enum class Page {
  Main,
  Status,
};

enum class DataState {
  Waiting,
  Live,
  Stale,
  Offline,
};

struct ButtonState {
  bool wasPressed = false;
  bool longPressHandled = false;
  uint32_t pressedAtMs = 0;
};

struct SensorData {
  float temperature = NAN;
  float humidity = NAN;
  bool valid = false;
  uint32_t lastReceivedMs = 0;
};

struct RetryState {
  uint32_t nextAttemptMs = 0;
  uint32_t delayMs = 0;
};

// =============================================================================
// Global state
// =============================================================================

M5Canvas canvas(&M5.Display);
Preferences preferences;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

Page currentPage = Page::Main;
SensorData sensorData;

ButtonState buttonA;
ButtonState buttonB;
ButtonState buttonC;

RetryState wifiRetry{ 0, App::WIFI_RETRY_MIN_MS };
RetryState mqttRetry{ 0, App::MQTT_RETRY_MIN_MS };

bool displaySleeping = false;
bool timeConfigured = false;
bool timeReadyLogged = false;

uint8_t brightnessIndex = App::DEFAULT_BRIGHTNESS_INDEX;

uint32_t lastUserActivityMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t lastHealthLogMs = 0;

// =============================================================================
// Time helpers
// =============================================================================

bool intervalElapsed(uint32_t now, uint32_t since, uint32_t interval) {
  return static_cast<uint32_t>(now - since) >= interval;
}

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool isClockValid() {
  // 2024-01-01 00:00:00 UTC
  constexpr time_t MIN_VALID_TIME = 1704067200;
  return time(nullptr) >= MIN_VALID_TIME;
}

bool getLocalTimeSafe(struct tm& localTime) {
  if (!isClockValid()) {
    return false;
  }
  return getLocalTime(&localTime, 10);
}

void formatClock(char* buffer, size_t size) {
  struct tm localTime;
  if (!getLocalTimeSafe(localTime)) {
    snprintf(buffer, size, "--:--");
    return;
  }

  snprintf(buffer, size, "%02d:%02d", localTime.tm_hour, localTime.tm_min);
}

void formatDate(char* buffer, size_t size) {
  struct tm localTime;
  if (!getLocalTimeSafe(localTime)) {
    snprintf(buffer, size, "--/--");
    return;
  }

  snprintf(
    buffer,
    size,
    "%02d/%02d",
    localTime.tm_mon + 1,
    localTime.tm_mday);
}

void formatUptime(char* buffer, size_t size) {
  const uint32_t totalSeconds = millis() / 1000UL;
  const uint32_t days = totalSeconds / 86400UL;
  const uint32_t hours = (totalSeconds % 86400UL) / 3600UL;
  const uint32_t minutes = (totalSeconds % 3600UL) / 60UL;

  if (days > 0) {
    snprintf(
      buffer,
      size,
      "%lud %02luh",
      static_cast<unsigned long>(days),
      static_cast<unsigned long>(hours));
  } else {
    snprintf(
      buffer,
      size,
      "%luh %02lum",
      static_cast<unsigned long>(hours),
      static_cast<unsigned long>(minutes));
  }
}

// =============================================================================
// Data freshness
// =============================================================================

uint32_t sensorAgeMs() {
  if (!sensorData.valid) {
    return 0;
  }

  return static_cast<uint32_t>(millis() - sensorData.lastReceivedMs);
}

DataState getDataState() {
  if (!sensorData.valid) {
    return DataState::Waiting;
  }

  const uint32_t ageMs = sensorAgeMs();

  if (ageMs < App::DATA_STALE_MS) {
    return DataState::Live;
  }

  if (ageMs < App::DATA_OFFLINE_MS) {
    return DataState::Stale;
  }

  return DataState::Offline;
}

const char* dataStateText(DataState state) {
  switch (state) {
    case DataState::Live:
      return "LIVE";
    case DataState::Stale:
      return "STALE";
    case DataState::Offline:
      return "OFFLINE";
    case DataState::Waiting:
    default:
      return "WAIT";
  }
}

uint16_t dataStateColor(DataState state) {
  switch (state) {
    case DataState::Live:
      return Color::GREEN;
    case DataState::Stale:
      return Color::YELLOW;
    case DataState::Offline:
      return Color::RED;
    case DataState::Waiting:
    default:
      return Color::PURPLE;
  }
}

void formatDataAge(char* buffer, size_t size) {
  if (!sensorData.valid) {
    snprintf(buffer, size, "waiting");
    return;
  }

  const uint32_t ageSeconds = sensorAgeMs() / 1000UL;

  if (ageSeconds < 60UL) {
    snprintf(buffer, size, "%lus ago", static_cast<unsigned long>(ageSeconds));
  } else if (ageSeconds < 3600UL) {
    snprintf(
      buffer,
      size,
      "%lum ago",
      static_cast<unsigned long>(ageSeconds / 60UL));
  } else if (ageSeconds < 86400UL) {
    snprintf(
      buffer,
      size,
      "%luh ago",
      static_cast<unsigned long>(ageSeconds / 3600UL));
  } else {
    snprintf(
      buffer,
      size,
      "%lud ago",
      static_cast<unsigned long>(ageSeconds / 86400UL));
  }
}

// =============================================================================
// Font helpers
// =============================================================================

void setSmallFont() {
#if HAVE_JETBRAINS_MONO
  canvas.setFont(&JetBrainsMono9pt7b);
#else
  canvas.setFont(&fonts::FreeMono9pt7b);
#endif
}

void setMediumFont() {
#if HAVE_JETBRAINS_MONO
  canvas.setFont(&JetBrainsMono12pt7b);
#else
  canvas.setFont(&fonts::FreeMonoBold12pt7b);
#endif
}

void setLargeFont() {
#if HAVE_JETBRAINS_MONO
  canvas.setFont(&JetBrainsMono24pt7b);
#else
  canvas.setFont(&fonts::FreeMonoBold24pt7b);
#endif
}

void drawSmall(
  const char* text,
  int x,
  int y,
  textdatum_t datum,
  uint16_t color) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  setSmallFont();
  canvas.drawString(text, x, y);
}

void drawMedium(
  const char* text,
  int x,
  int y,
  textdatum_t datum,
  uint16_t color) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  setMediumFont();
  canvas.drawString(text, x, y);
}

void drawLarge(
  const char* text,
  int x,
  int y,
  textdatum_t datum,
  uint16_t color) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  setLargeFont();
  canvas.drawString(text, x, y);
}

// =============================================================================
// Display
// =============================================================================

void drawHeader() {
  char date[8];
  char clock[8];
  char dateTime[20];

  formatDate(date, sizeof(date));
  formatClock(clock, sizeof(clock));
  snprintf(dateTime, sizeof(dateTime), "%s %s", date, clock);

  const char* title =
    currentPage == Page::Main ? "ENV / B3D8" : "SYSTEM STATUS";

  drawSmall(title, 14, 21, middle_left, Color::CYAN);
  drawSmall(dateTime, 306, 21, middle_right, Color::TEXT);

  canvas.drawFastHLine(14, 39, 292, Color::DIVIDER);
}

void drawMainPage() {
  constexpr int LEFT_CENTER_X = 82;
  constexpr int RIGHT_CENTER_X = 238;

  canvas.drawFastVLine(160, 52, 135, Color::DIVIDER);

  drawSmall(
    "TEMPERATURE",
    LEFT_CENTER_X,
    61,
    middle_center,
    Color::MUTED);

  drawSmall(
    "HUMIDITY",
    RIGHT_CENTER_X,
    61,
    middle_center,
    Color::MUTED);

  char value[16];

  if (sensorData.valid && !isnan(sensorData.temperature)) {
    snprintf(value, sizeof(value), "%.1f", sensorData.temperature);
  } else {
    snprintf(value, sizeof(value), "--.-");
  }

  drawLarge(
    value,
    LEFT_CENTER_X,
    119,
    middle_center,
    Color::ORANGE);

  if (sensorData.valid && !isnan(sensorData.humidity)) {
    snprintf(value, sizeof(value), "%.0f", sensorData.humidity);
  } else {
    snprintf(value, sizeof(value), "--");
  }

  drawLarge(
    value,
    RIGHT_CENTER_X,
    119,
    middle_center,
    Color::CYAN);

  // Keep the embedded font ASCII-only: draw the degree symbol geometrically.
  canvas.drawCircle(LEFT_CENTER_X - 11, 158, 3, Color::ORANGE);

  drawMedium(
    "C",
    LEFT_CENTER_X + 4,
    171,
    middle_center,
    Color::ORANGE);

  drawMedium(
    "%",
    RIGHT_CENTER_X,
    171,
    middle_center,
    Color::CYAN);
}

void drawStatusPage() {
  constexpr int LABEL_X = 18;
  constexpr int VALUE_X = 302;
  constexpr int ROW_Y[] = { 60, 84, 108, 132, 156, 180 };

  char value[32];

  drawSmall("Wi-Fi", LABEL_X, ROW_Y[0], middle_left, Color::MUTED);

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(value, sizeof(value), "%d dBm", WiFi.RSSI());
    drawSmall(value, VALUE_X, ROW_Y[0], middle_right, Color::GREEN);
  } else {
    drawSmall("OFFLINE", VALUE_X, ROW_Y[0], middle_right, Color::RED);
  }

  drawSmall("MQTT", LABEL_X, ROW_Y[1], middle_left, Color::MUTED);
  drawSmall(
    mqttClient.connected() ? "CONNECTED" : "OFFLINE",
    VALUE_X,
    ROW_Y[1],
    middle_right,
    mqttClient.connected() ? Color::GREEN : Color::RED);

  drawSmall("Sensor", LABEL_X, ROW_Y[2], middle_left, Color::MUTED);
  drawSmall("B3D8", VALUE_X, ROW_Y[2], middle_right, Color::TEXT);

  drawSmall("Data age", LABEL_X, ROW_Y[3], middle_left, Color::MUTED);
  formatDataAge(value, sizeof(value));
  drawSmall(
    value,
    VALUE_X,
    ROW_Y[3],
    middle_right,
    dataStateColor(getDataState()));

  drawSmall("IP", LABEL_X, ROW_Y[4], middle_left, Color::MUTED);

  if (WiFi.status() == WL_CONNECTED) {
    const IPAddress ip = WiFi.localIP();
    snprintf(
      value,
      sizeof(value),
      "%u.%u.%u.%u",
      ip[0],
      ip[1],
      ip[2],
      ip[3]);
  } else {
    snprintf(value, sizeof(value), "-");
  }

  drawSmall(value, VALUE_X, ROW_Y[4], middle_right, Color::TEXT);

  drawSmall("Uptime", LABEL_X, ROW_Y[5], middle_left, Color::MUTED);
  formatUptime(value, sizeof(value));
  drawSmall(value, VALUE_X, ROW_Y[5], middle_right, Color::TEXT);
}

void drawFooter() {
  canvas.drawFastHLine(14, 195, 292, Color::DIVIDER);

  canvas.fillCircle(
    18,
    218,
    4,
    mqttClient.connected() ? Color::GREEN : Color::RED);

  drawSmall("MQTT", 28, 218, middle_left, Color::MUTED);

  if (currentPage == Page::Main) {
    const DataState state = getDataState();

    drawSmall(
      dataStateText(state),
      83,
      218,
      middle_left,
      dataStateColor(state));

    char age[24];
    formatDataAge(age, sizeof(age));
    drawSmall(age, 306, 218, middle_right, Color::MUTED);
    return;
  }

  char version[16];
  snprintf(version, sizeof(version), "v%s", App::VERSION);
  drawSmall(version, 306, 218, middle_right, Color::MUTED);
}

void drawScreen() {
  if (displaySleeping) {
    return;
  }

  canvas.fillSprite(Color::BG);
  drawHeader();

  if (currentPage == Page::Main) {
    drawMainPage();
  } else {
    drawStatusPage();
  }

  drawFooter();
  canvas.pushSprite(0, 0);
}

void refreshDisplayIfDue(uint32_t now) {
  if (displaySleeping) {
    return;
  }

  if (!intervalElapsed(now, lastDisplayRefreshMs, App::DISPLAY_REFRESH_MS)) {
    return;
  }

  lastDisplayRefreshMs = now;
  drawScreen();
}

// =============================================================================
// Display power / brightness
// =============================================================================

void registerUserActivity() {
  lastUserActivityMs = millis();
}

void sleepDisplay() {
  if (displaySleeping) {
    return;
  }

  displaySleeping = true;
  M5.Display.setBrightness(0);
  Serial.println("Display: sleep");
}

void wakeDisplay() {
  displaySleeping = false;
  registerUserActivity();

  M5.Display.setBrightness(App::BRIGHTNESS_LEVELS[brightnessIndex]);
  drawScreen();

  Serial.println("Display: wake");
}

void loadBrightness() {
  brightnessIndex =
    preferences.getUChar(
      "brightness",
      App::DEFAULT_BRIGHTNESS_INDEX);

  if (brightnessIndex >= App::BRIGHTNESS_LEVEL_COUNT) {
    brightnessIndex = App::DEFAULT_BRIGHTNESS_INDEX;
  }
}

void cycleBrightness() {
  brightnessIndex =
    (brightnessIndex + 1) % App::BRIGHTNESS_LEVEL_COUNT;

  M5.Display.setBrightness(App::BRIGHTNESS_LEVELS[brightnessIndex]);

  // NVS is written only on explicit user action, not in the main loop.
  preferences.putUChar("brightness", brightnessIndex);

  Serial.printf(
    "Display: brightness=%u\n",
    App::BRIGHTNESS_LEVELS[brightnessIndex]);
}

void maintainDisplaySleep(uint32_t now) {
  if (displaySleeping) {
    return;
  }

  if (intervalElapsed(now, lastUserActivityMs, App::DISPLAY_SLEEP_MS)) {
    sleepDisplay();
  }
}

// =============================================================================
// Retry helpers
// =============================================================================

void resetRetry(
  RetryState& retry,
  uint32_t minimumDelayMs) {
  retry.delayMs = minimumDelayMs;
  retry.nextAttemptMs = 0;
}

void scheduleRetry(
  RetryState& retry,
  uint32_t now,
  uint32_t maximumDelayMs) {
  retry.nextAttemptMs = now + retry.delayMs;

  if (retry.delayMs < maximumDelayMs) {
    retry.delayMs =
      min(retry.delayMs * 2UL, maximumDelayMs);
  }
}

// =============================================================================
// Wi-Fi
// =============================================================================

void startWiFiConnection() {
  Serial.printf("WiFi: connecting to %s\n", WIFI_SSID);

  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void configureWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // This device is mains-powered. Disabling Wi-Fi power saving generally
  // improves MQTT latency and long-running connection stability.
  WiFi.setSleep(false);

  startWiFiConnection();

  wifiRetry.nextAttemptMs = millis() + App::WIFI_CONNECT_TIMEOUT_MS;
}

void maintainWiFi(uint32_t now) {
  static wl_status_t previousStatus = WL_NO_SHIELD;

  const wl_status_t status = WiFi.status();

  if (status != previousStatus) {
    previousStatus = status;

    if (status == WL_CONNECTED) {
      const IPAddress ip = WiFi.localIP();

      Serial.printf(
        "WiFi: connected IP=%u.%u.%u.%u RSSI=%d dBm\n",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        WiFi.RSSI());

      resetRetry(wifiRetry, App::WIFI_RETRY_MIN_MS);
      return;
    }

    Serial.printf("WiFi: disconnected status=%d\n", status);
  }

  if (status == WL_CONNECTED) {
    return;
  }

  if (!deadlineReached(now, wifiRetry.nextAttemptMs)) {
    return;
  }

  Serial.printf(
    "WiFi: reconnecting, retry delay=%lu ms\n",
    static_cast<unsigned long>(wifiRetry.delayMs));

  startWiFiConnection();
  scheduleRetry(wifiRetry, now, App::WIFI_RETRY_MAX_MS);
}

// =============================================================================
// NTP
// =============================================================================

void configureTimeIfNeeded() {
  if (timeConfigured || WiFi.status() != WL_CONNECTED) {
    return;
  }

  configTzTime(
    TZ_INFO,
    NTP_SERVER_1,
    NTP_SERVER_2,
    NTP_SERVER_3);

  timeConfigured = true;
  Serial.println("NTP: configured");
}

void maintainTime() {
  if (!timeConfigured) {
    configureTimeIfNeeded();
  }

  if (!timeReadyLogged && isClockValid()) {
    timeReadyLogged = true;

    char date[8];
    char clock[8];

    formatDate(date, sizeof(date));
    formatClock(clock, sizeof(clock));

    Serial.printf("NTP: synchronized %s %s\n", date, clock);
  }
}

// =============================================================================
// MQTT
// =============================================================================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_ENV) != 0) {
    return;
  }

  JsonDocument document;
  const DeserializationError error =
    deserializeJson(document, payload, length);

  if (error) {
    Serial.printf("MQTT: JSON error=%s\n", error.c_str());
    return;
  }

  if (!document["temperature"].is<float>() && !document["temperature"].is<int>()) {
    Serial.println("MQTT: temperature missing/invalid");
    return;
  }

  if (!document["humidity"].is<float>() && !document["humidity"].is<int>()) {
    Serial.println("MQTT: humidity missing/invalid");
    return;
  }

  const float temperature = document["temperature"].as<float>();
  const float humidity = document["humidity"].as<float>();

  if (!isfinite(temperature) || !isfinite(humidity) || temperature < -50.0f || temperature > 80.0f || humidity < 0.0f || humidity > 100.0f) {
    Serial.println("MQTT: environmental value out of range");
    return;
  }

  sensorData.temperature = temperature;
  sensorData.humidity = humidity;
  sensorData.valid = true;
  sensorData.lastReceivedMs = millis();

  Serial.printf(
    "ENV: %.1f C %.1f %%\n",
    sensorData.temperature,
    sensorData.humidity);

  if (!displaySleeping) {
    drawScreen();
    lastDisplayRefreshMs = millis();
  }
}

void configureMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(App::MQTT_KEEPALIVE_SEC);
  mqttClient.setSocketTimeout(App::MQTT_SOCKET_TIMEOUT_SEC);
}

bool connectMQTT() {
  Serial.printf(
    "MQTT: connecting to %s:%u\n",
    MQTT_HOST,
    MQTT_PORT);

  bool connected = false;

  if (strlen(MQTT_USERNAME) > 0) {
    connected =
      mqttClient.connect(
        MQTT_CLIENT_ID,
        MQTT_USERNAME,
        MQTT_PASSWORD);
  } else {
    connected = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (!connected) {
    Serial.printf(
      "MQTT: connect failed state=%d\n",
      mqttClient.state());
    return false;
  }

  if (!mqttClient.subscribe(MQTT_TOPIC_ENV, 0)) {
    Serial.println("MQTT: subscribe failed");
    mqttClient.disconnect();
    return false;
  }

  Serial.printf(
    "MQTT: connected and subscribed %s\n",
    MQTT_TOPIC_ENV);

  return true;
}

void maintainMQTT(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (mqttClient.connected()) {
      mqttClient.disconnect();
    }
    return;
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }

  if (!deadlineReached(now, mqttRetry.nextAttemptMs)) {
    return;
  }

  if (connectMQTT()) {
    resetRetry(mqttRetry, App::MQTT_RETRY_MIN_MS);
    return;
  }

  scheduleRetry(mqttRetry, now, App::MQTT_RETRY_MAX_MS);
}

void forceNetworkReconnect() {
  Serial.println("Network: manual reconnect");

  mqttClient.disconnect();
  WiFi.disconnect(false, false);

  resetRetry(wifiRetry, App::WIFI_RETRY_MIN_MS);
  resetRetry(mqttRetry, App::MQTT_RETRY_MIN_MS);

  startWiFiConnection();

  wifiRetry.nextAttemptMs =
    millis() + App::WIFI_CONNECT_TIMEOUT_MS;
}

// =============================================================================
// Buttons
// =============================================================================

void handleButtonAShort() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  sleepDisplay();
}

void handleButtonALong() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  registerUserActivity();
  cycleBrightness();
  drawScreen();
}

void handleButtonBShort() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  registerUserActivity();

  currentPage =
    currentPage == Page::Main
      ? Page::Status
      : Page::Main;

  drawScreen();
}

void handleButtonBLong() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  registerUserActivity();
  Serial.println("Button B: long press reserved");
}

void handleButtonCShort() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  registerUserActivity();
  currentPage = Page::Main;
  drawScreen();
}

void handleButtonCLong() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  registerUserActivity();
  forceNetworkReconnect();
  drawScreen();
}

void processButton(
  ButtonState& state,
  bool pressed,
  uint32_t now,
  void (*shortPressHandler)(),
  void (*longPressHandler)()) {
  if (pressed && !state.wasPressed) {
    state.pressedAtMs = now;
    state.longPressHandled = false;
  }

  if (pressed && !state.longPressHandled && intervalElapsed(now, state.pressedAtMs, App::LONG_PRESS_MS)) {
    state.longPressHandled = true;
    longPressHandler();
  }

  if (!pressed && state.wasPressed && !state.longPressHandled) {
    shortPressHandler();
  }

  state.wasPressed = pressed;
}

void processButtons(uint32_t now) {
  processButton(
    buttonA,
    M5.BtnA.isPressed(),
    now,
    handleButtonAShort,
    handleButtonALong);

  processButton(
    buttonB,
    M5.BtnB.isPressed(),
    now,
    handleButtonBShort,
    handleButtonBLong);

  processButton(
    buttonC,
    M5.BtnC.isPressed(),
    now,
    handleButtonCShort,
    handleButtonCLong);
}

// =============================================================================
// Diagnostics
// =============================================================================

const char* resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "POWERON";
    case ESP_RST_EXT:
      return "EXTERNAL";
    case ESP_RST_SW:
      return "SOFTWARE";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INT_WDT";
    case ESP_RST_TASK_WDT:
      return "TASK_WDT";
    case ESP_RST_WDT:
      return "WDT";
    case ESP_RST_DEEPSLEEP:
      return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_UNKNOWN:
    default:
      return "UNKNOWN";
  }
}

void logBootDiagnostics() {
  const esp_reset_reason_t reason = esp_reset_reason();

  Serial.printf(
    "Boot: reset_reason=%s (%d)\n",
    resetReasonText(reason),
    static_cast<int>(reason));

  Serial.printf(
    "Heap: free=%u largest=%u min_free=%u\n",
    ESP.getFreeHeap(),
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
    ESP.getMinFreeHeap());
}

void logHealthIfDue(uint32_t now) {
  if (!intervalElapsed(
        now,
        lastHealthLogMs,
        App::HEALTH_LOG_INTERVAL_MS)) {
    return;
  }

  lastHealthLogMs = now;

  char uptime[24];
  formatUptime(uptime, sizeof(uptime));

  Serial.printf(
    "HEALTH: uptime=%s wifi=%s mqtt=%s rssi=%d "
    "heap=%u min_heap=%u largest=%u data=%s age_s=%lu\n",
    uptime,
    WiFi.status() == WL_CONNECTED ? "up" : "down",
    mqttClient.connected() ? "up" : "down",
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
    ESP.getFreeHeap(),
    ESP.getMinFreeHeap(),
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
    dataStateText(getDataState()),
    sensorData.valid
      ? static_cast<unsigned long>(sensorAgeMs() / 1000UL)
      : 0UL);
}

// =============================================================================
// Setup
// =============================================================================

void setupDisplay() {
  M5.Display.setRotation(1);

  preferences.begin("envmonitor", false);
  loadBrightness();

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);

  Serial.printf(
    "Font: %s\n",
    HAVE_JETBRAINS_MONO
      ? "JetBrains Mono embedded"
      : "FreeMono fallback");

  Serial.printf(
    "Heap before canvas: free=%u largest=%u\n",
    ESP.getFreeHeap(),
    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  canvas.setColorDepth(8);

  if (canvas.createSprite(
        App::SCREEN_WIDTH,
        App::SCREEN_HEIGHT)
      == nullptr) {
    Serial.println("FATAL: canvas allocation failed");

    // A visible hard failure is preferable to continuing in an undefined UI
    // state. Networking is not started when the required framebuffer is absent.
    while (true) {
      delay(1000);
    }
  }

  canvas.setTextWrap(false);

  lastUserActivityMs = millis();
  lastDisplayRefreshMs = millis();

  drawScreen();
}

// =============================================================================
// Arduino entry points
// =============================================================================

void setup() {
  const auto config = M5.config();
  M5.begin(config);

  Serial.begin(115200);
  delay(100);

  Serial.printf(
    "\n%s v%s\n",
    App::NAME,
    App::VERSION);

  logBootDiagnostics();

  setupDisplay();
  configureMQTT();
  configureWiFi();
}

void loop() {
  const uint32_t now = millis();

  M5.update();
  processButtons(now);

  maintainWiFi(now);
  maintainTime();
  maintainMQTT(now);

  maintainDisplaySleep(now);
  refreshDisplayIfDue(now);

  logHealthIfDue(now);

  // Yield to the ESP32 system tasks without introducing a meaningful
  // application-level blocking delay.
  delay(2);
}
