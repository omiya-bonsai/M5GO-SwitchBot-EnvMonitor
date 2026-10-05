#include <M5Unified.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
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
constexpr char VERSION[] = "0.5.0";

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

// -----------------------------------------------------------------------------
// microSD
// -----------------------------------------------------------------------------

constexpr int SD_CS_PIN = 4;

constexpr uint32_t SD_LOG_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t GRAPH_RELOAD_INTERVAL_MS = 5UL * 60UL * 1000UL;

constexpr size_t GRAPH_POINT_COUNT = 288;  // 24 h / 5 min
constexpr uint32_t GRAPH_BUCKET_SECONDS = 5UL * 60UL;

// -----------------------------------------------------------------------------
// IMU
// -----------------------------------------------------------------------------

constexpr uint32_t IMU_SAMPLE_INTERVAL_MS = 100UL;

// Difference in acceleration magnitude required to wake the display.
// This is intentionally conservative to avoid desk vibration waking the LCD.
constexpr float IMU_WAKE_DELTA_G = 0.22f;

constexpr uint32_t IMU_WAKE_COOLDOWN_MS = 3000UL;

constexpr uint8_t BRIGHTNESS_LEVELS[] = {
  40, 80, 120, 160, 220
};

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
  TemperatureGraph,
  HumidityGraph,
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

struct GraphPoint {
  bool valid = false;
  time_t timestamp = 0;
  float temperature = NAN;
  float humidity = NAN;
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

RetryState wifiRetry{
  0,
  App::WIFI_RETRY_MIN_MS
};

RetryState mqttRetry{
  0,
  App::MQTT_RETRY_MIN_MS
};

GraphPoint graphPoints[App::GRAPH_POINT_COUNT];

bool displaySleeping = false;

bool timeConfigured = false;
bool timeReadyLogged = false;

bool sdAvailable = false;
bool imuAvailable = false;

uint8_t brightnessIndex =
  App::DEFAULT_BRIGHTNESS_INDEX;

uint32_t lastUserActivityMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t lastHealthLogMs = 0;

uint32_t lastSdLogMs = 0;
uint32_t lastGraphReloadMs = 0;

uint32_t lastImuSampleMs = 0;
uint32_t lastImuWakeMs = 0;

float previousAccelMagnitude = NAN;

// =============================================================================
// Time helpers
// =============================================================================

bool intervalElapsed(
  uint32_t now,
  uint32_t since,
  uint32_t interval) {

  return static_cast<uint32_t>(now - since) >= interval;
}

bool deadlineReached(
  uint32_t now,
  uint32_t deadline) {

  return static_cast<int32_t>(now - deadline) >= 0;
}

bool isClockValid() {
  constexpr time_t MIN_VALID_TIME = 1704067200;
  return time(nullptr) >= MIN_VALID_TIME;
}

bool getLocalTimeSafe(struct tm& localTime) {

  if (!isClockValid()) {
    return false;
  }

  return getLocalTime(&localTime, 10);
}

void formatClock(
  char* buffer,
  size_t size) {

  struct tm localTime;

  if (!getLocalTimeSafe(localTime)) {
    snprintf(buffer, size, "--:--");
    return;
  }

  snprintf(
    buffer,
    size,
    "%02d:%02d",
    localTime.tm_hour,
    localTime.tm_min);
}

void formatDate(
  char* buffer,
  size_t size) {

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

void formatUptime(
  char* buffer,
  size_t size) {

  const uint32_t totalSeconds =
    millis() / 1000UL;

  const uint32_t days =
    totalSeconds / 86400UL;

  const uint32_t hours =
    (totalSeconds % 86400UL) / 3600UL;

  const uint32_t minutes =
    (totalSeconds % 3600UL) / 60UL;

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

  return static_cast<uint32_t>(
    millis() - sensorData.lastReceivedMs);
}

DataState getDataState() {

  if (!sensorData.valid) {
    return DataState::Waiting;
  }

  const uint32_t ageMs =
    sensorAgeMs();

  if (ageMs < App::DATA_STALE_MS) {
    return DataState::Live;
  }

  if (ageMs < App::DATA_OFFLINE_MS) {
    return DataState::Stale;
  }

  return DataState::Offline;
}

const char* dataStateText(
  DataState state) {

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

uint16_t dataStateColor(
  DataState state) {

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

void formatDataAge(
  char* buffer,
  size_t size) {

  if (!sensorData.valid) {

    snprintf(
      buffer,
      size,
      "waiting");

    return;
  }

  const uint32_t ageSeconds =
    sensorAgeMs() / 1000UL;

  if (ageSeconds < 60UL) {

    snprintf(
      buffer,
      size,
      "%lus ago",
      static_cast<unsigned long>(ageSeconds));

  } else if (ageSeconds < 3600UL) {

    snprintf(
      buffer,
      size,
      "%lum ago",
      static_cast<unsigned long>(
        ageSeconds / 60UL));

  } else if (ageSeconds < 86400UL) {

    snprintf(
      buffer,
      size,
      "%luh ago",
      static_cast<unsigned long>(
        ageSeconds / 3600UL));

  } else {

    snprintf(
      buffer,
      size,
      "%lud ago",
      static_cast<unsigned long>(
        ageSeconds / 86400UL));
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

  canvas.drawString(
    text,
    x,
    y);
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

  canvas.drawString(
    text,
    x,
    y);
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

  canvas.drawString(
    text,
    x,
    y);
}

// =============================================================================
// microSD
// =============================================================================

void ensureLogDirectory() {

  if (!sdAvailable) {
    return;
  }

  if (!SD.exists("/logs")) {

    if (!SD.mkdir("/logs")) {
      Serial.println("SD: failed to create /logs");
    }
  }
}

bool buildLogPath(
  time_t timestamp,
  char* buffer,
  size_t size) {

  struct tm localTime;

  localtime_r(
    &timestamp,
    &localTime);

  if (localTime.tm_year < 120) {
    return false;
  }

  snprintf(
    buffer,
    size,
    "/logs/%04d-%02d-%02d.csv",
    localTime.tm_year + 1900,
    localTime.tm_mon + 1,
    localTime.tm_mday);

  return true;
}

void initializeSdCard() {

  Serial.println("SD: initializing");

  if (!SD.begin(
        App::SD_CS_PIN,
        SPI,
        25000000)) {

    sdAvailable = false;

    Serial.println(
      "SD: unavailable - logging disabled");

    return;
  }

  sdAvailable = true;

  ensureLogDirectory();

  const uint64_t cardSize =
    SD.cardSize();

  Serial.printf(
    "SD: ready size=%llu MB\n",
    static_cast<unsigned long long>(
      cardSize / (1024ULL * 1024ULL)));
}

void writeCsvHeaderIfNeeded(
  const char* path) {

  if (!sdAvailable) {
    return;
  }

  if (SD.exists(path)) {
    return;
  }

  File file =
    SD.open(
      path,
      FILE_WRITE);

  if (!file) {

    Serial.printf(
      "SD: cannot create %s\n",
      path);

    return;
  }

  file.println(
    "timestamp,temperature,humidity,rssi,mqtt");

  file.close();
}

bool appendEnvironmentLog() {

  if (!sdAvailable || !sensorData.valid || !isClockValid()) {

    return false;
  }

  const time_t now =
    time(nullptr);

  char path[32];

  if (!buildLogPath(
        now,
        path,
        sizeof(path))) {

    return false;
  }

  writeCsvHeaderIfNeeded(path);

  File file =
    SD.open(
      path,
      FILE_APPEND);

  if (!file) {

    Serial.printf(
      "SD: open failed %s\n",
      path);

    return false;
  }

  struct tm localTime;

  localtime_r(
    &now,
    &localTime);

  char timestamp[24];

  snprintf(
    timestamp,
    sizeof(timestamp),
    "%04d-%02d-%02d %02d:%02d:%02d",
    localTime.tm_year + 1900,
    localTime.tm_mon + 1,
    localTime.tm_mday,
    localTime.tm_hour,
    localTime.tm_min,
    localTime.tm_sec);

  const int rssi =
    WiFi.status() == WL_CONNECTED
      ? WiFi.RSSI()
      : 0;

  file.printf(
    "%s,%.1f,%.1f,%d,%d\n",
    timestamp,
    sensorData.temperature,
    sensorData.humidity,
    rssi,
    mqttClient.connected() ? 1 : 0);

  file.flush();
  file.close();

  Serial.printf(
    "SD: logged %s %.1f C %.1f %%\n",
    timestamp,
    sensorData.temperature,
    sensorData.humidity);

  return true;
}

void maintainSdLogging(
  uint32_t now) {

  if (!sdAvailable || !sensorData.valid || !isClockValid()) {

    return;
  }

  if (!intervalElapsed(
        now,
        lastSdLogMs,
        App::SD_LOG_INTERVAL_MS)) {

    return;
  }

  lastSdLogMs = now;

  appendEnvironmentLog();
}

// =============================================================================
// Graph data
// =============================================================================

void clearGraphPoints() {

  for (size_t i = 0;
       i < App::GRAPH_POINT_COUNT;
       ++i) {

    graphPoints[i] = GraphPoint{};
  }
}

bool parseCsvTimestamp(
  const char* text,
  time_t& result) {

  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;

  if (sscanf(
        text,
        "%d-%d-%d %d:%d:%d",
        &year,
        &month,
        &day,
        &hour,
        &minute,
        &second)
      != 6) {

    return false;
  }

  struct tm value = {};

  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  value.tm_isdst = -1;

  result = mktime(&value);

  return result > 0;
}

void processGraphCsvLine(
  const char* line,
  time_t windowStart,
  time_t windowEnd) {

  char timestampText[24];

  float temperature;
  float humidity;

  int rssi;
  int mqtt;

  if (sscanf(
        line,
        "%23[^,],%f,%f,%d,%d",
        timestampText,
        &temperature,
        &humidity,
        &rssi,
        &mqtt)
      != 5) {

    return;
  }

  time_t timestamp;

  if (!parseCsvTimestamp(
        timestampText,
        timestamp)) {

    return;
  }

  if (timestamp < windowStart || timestamp > windowEnd) {

    return;
  }

  const uint32_t offset =
    static_cast<uint32_t>(
      timestamp - windowStart);

  const size_t bucket =
    offset / App::GRAPH_BUCKET_SECONDS;

  if (bucket >= App::GRAPH_POINT_COUNT) {
    return;
  }

  graphPoints[bucket].valid = true;
  graphPoints[bucket].timestamp = timestamp;
  graphPoints[bucket].temperature = temperature;
  graphPoints[bucket].humidity = humidity;
}

void loadGraphFile(
  const char* path,
  time_t windowStart,
  time_t windowEnd) {

  if (!SD.exists(path)) {
    return;
  }

  File file =
    SD.open(
      path,
      FILE_READ);

  if (!file) {

    Serial.printf(
      "GRAPH: cannot open %s\n",
      path);

    return;
  }

  // Skip CSV header.
  file.readStringUntil('\n');

  char lineBuffer[128];

  while (file.available()) {

    size_t index = 0;

    while (file.available() && index < sizeof(lineBuffer) - 1) {

      const char c =
        static_cast<char>(
          file.read());

      if (c == '\n') {
        break;
      }

      if (c != '\r') {
        lineBuffer[index++] = c;
      }
    }

    lineBuffer[index] = '\0';

    if (index > 0) {

      processGraphCsvLine(
        lineBuffer,
        windowStart,
        windowEnd);
    }
  }

  file.close();
}

void loadGraphData() {

  if (!sdAvailable || !isClockValid()) {

    return;
  }

  clearGraphPoints();

  const time_t windowEnd =
    time(nullptr);

  const time_t windowStart =
    windowEnd - (24UL * 60UL * 60UL);

  char todayPath[32];
  char yesterdayPath[32];

  buildLogPath(
    windowEnd,
    todayPath,
    sizeof(todayPath));

  buildLogPath(
    windowStart,
    yesterdayPath,
    sizeof(yesterdayPath));

  if (strcmp(
        todayPath,
        yesterdayPath)
      != 0) {

    loadGraphFile(
      yesterdayPath,
      windowStart,
      windowEnd);
  }

  loadGraphFile(
    todayPath,
    windowStart,
    windowEnd);

  lastGraphReloadMs = millis();

  Serial.println(
    "GRAPH: 24h data loaded");
}

void reloadGraphIfNeeded() {

  if (currentPage != Page::TemperatureGraph && currentPage != Page::HumidityGraph) {

    return;
  }

  const uint32_t now =
    millis();

  if (lastGraphReloadMs == 0 || intervalElapsed(now, lastGraphReloadMs, App::GRAPH_RELOAD_INTERVAL_MS)) {

    loadGraphData();
  }
}

// =============================================================================
// IMU
// =============================================================================

void initializeImu() {

  M5.Imu.update();

  float ax;
  float ay;
  float az;

  if (M5.Imu.getAccelData(
        &ax,
        &ay,
        &az)) {

    imuAvailable = true;

    previousAccelMagnitude =
      sqrtf(
        ax * ax + ay * ay + az * az);

    Serial.printf(
      "IMU: ready accel=%.3f g\n",
      previousAccelMagnitude);

  } else {

    imuAvailable = false;

    Serial.println(
      "IMU: unavailable - motion wake disabled");
  }
}

void maintainImuWake(
  uint32_t now) {

  if (!imuAvailable) {
    return;
  }

  if (!displaySleeping) {
    return;
  }

  if (!intervalElapsed(
        now,
        lastImuSampleMs,
        App::IMU_SAMPLE_INTERVAL_MS)) {

    return;
  }

  lastImuSampleMs = now;

  M5.Imu.update();

  float ax;
  float ay;
  float az;

  if (!M5.Imu.getAccelData(
        &ax,
        &ay,
        &az)) {

    return;
  }

  const float magnitude =
    sqrtf(
      ax * ax + ay * ay + az * az);

  if (!isfinite(previousAccelMagnitude)) {

    previousAccelMagnitude =
      magnitude;

    return;
  }

  const float delta =
    fabsf(
      magnitude - previousAccelMagnitude);

  previousAccelMagnitude =
    magnitude;

  if (!intervalElapsed(
        now,
        lastImuWakeMs,
        App::IMU_WAKE_COOLDOWN_MS)) {

    return;
  }

  if (delta < App::IMU_WAKE_DELTA_G) {
    return;
  }

  lastImuWakeMs = now;

  Serial.printf(
    "IMU: motion wake delta=%.3f g\n",
    delta);

  displaySleeping = false;
  lastUserActivityMs = now;

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);
}

// =============================================================================
// Graph drawing
// =============================================================================

void findGraphRange(
  bool temperature,
  float& minimum,
  float& maximum,
  bool& hasData) {

  minimum = 100000.0f;
  maximum = -100000.0f;
  hasData = false;

  for (size_t i = 0;
       i < App::GRAPH_POINT_COUNT;
       ++i) {

    if (!graphPoints[i].valid) {
      continue;
    }

    const float value =
      temperature
        ? graphPoints[i].temperature
        : graphPoints[i].humidity;

    if (!isfinite(value)) {
      continue;
    }

    minimum =
      min(minimum, value);

    maximum =
      max(maximum, value);

    hasData = true;
  }

  if (!hasData) {
    return;
  }

  float margin =
    (maximum - minimum) * 0.15f;

  if (margin < 0.5f) {
    margin = 0.5f;
  }

  minimum -= margin;
  maximum += margin;
}

void drawGraph(
  bool temperature) {

  constexpr int LEFT = 42;
  constexpr int RIGHT = 306;
  constexpr int TOP = 58;
  constexpr int BOTTOM = 184;

  float minimum;
  float maximum;
  bool hasData;

  findGraphRange(
    temperature,
    minimum,
    maximum,
    hasData);

  if (!hasData) {

    drawMedium(
      sdAvailable
        ? "NO 24H DATA"
        : "SD UNAVAILABLE",
      160,
      118,
      middle_center,
      Color::MUTED);

    return;
  }

  // Grid.
  for (int i = 0; i <= 4; ++i) {

    const int y =
      TOP + ((BOTTOM - TOP) * i / 4);

    canvas.drawFastHLine(
      LEFT,
      y,
      RIGHT - LEFT,
      Color::DIVIDER);

    const float value =
      maximum - (maximum - minimum) * static_cast<float>(i) / 4.0f;

    char label[16];

    if (temperature) {

      snprintf(
        label,
        sizeof(label),
        "%.1f",
        value);

    } else {

      snprintf(
        label,
        sizeof(label),
        "%.0f",
        value);
    }

    drawSmall(
      label,
      LEFT - 5,
      y,
      middle_right,
      Color::MUTED);
  }

  canvas.drawFastVLine(
    LEFT,
    TOP,
    BOTTOM - TOP,
    Color::DIVIDER);

  canvas.drawFastHLine(
    LEFT,
    BOTTOM,
    RIGHT - LEFT,
    Color::DIVIDER);

  drawSmall(
    "-24h",
    LEFT,
    BOTTOM + 13,
    top_left,
    Color::MUTED);

  drawSmall(
    "-12h",
    (LEFT + RIGHT) / 2,
    BOTTOM + 13,
    top_center,
    Color::MUTED);

  drawSmall(
    "now",
    RIGHT,
    BOTTOM + 13,
    top_right,
    Color::MUTED);

  bool previousValid = false;

  int previousX = 0;
  int previousY = 0;

  const uint16_t graphColor =
    temperature
      ? Color::ORANGE
      : Color::CYAN;

  for (size_t i = 0;
       i < App::GRAPH_POINT_COUNT;
       ++i) {

    if (!graphPoints[i].valid) {

      previousValid = false;
      continue;
    }

    const float value =
      temperature
        ? graphPoints[i].temperature
        : graphPoints[i].humidity;

    if (!isfinite(value)) {

      previousValid = false;
      continue;
    }

    const int x =
      LEFT + static_cast<int>((RIGHT - LEFT) * static_cast<float>(i) / static_cast<float>(App::GRAPH_POINT_COUNT - 1));

    float normalized =
      (value - minimum) / (maximum - minimum);

    normalized =
      constrain(
        normalized,
        0.0f,
        1.0f);

    const int y =
      BOTTOM - static_cast<int>(normalized * (BOTTOM - TOP));

    if (previousValid) {

      canvas.drawLine(
        previousX,
        previousY,
        x,
        y,
        graphColor);
    }

    previousX = x;
    previousY = y;
    previousValid = true;
  }
}

// =============================================================================
// Display
// =============================================================================

const char* pageTitle() {

  switch (currentPage) {

    case Page::Main:
      return "ENV / B3D8";

    case Page::Status:
      return "SYSTEM STATUS";

    case Page::TemperatureGraph:
      return "TEMP / 24H";

    case Page::HumidityGraph:
      return "HUM / 24H";

    default:
      return "ENV";
  }
}

void drawHeader() {

  char date[8];
  char clock[8];
  char dateTime[20];

  formatDate(
    date,
    sizeof(date));

  formatClock(
    clock,
    sizeof(clock));

  snprintf(
    dateTime,
    sizeof(dateTime),
    "%s %s",
    date,
    clock);

  drawSmall(
    pageTitle(),
    14,
    21,
    middle_left,
    Color::CYAN);

  drawSmall(
    dateTime,
    306,
    21,
    middle_right,
    Color::TEXT);

  canvas.drawFastHLine(
    14,
    39,
    292,
    Color::DIVIDER);
}

void drawMainPage() {

  constexpr int LEFT_CENTER_X = 82;
  constexpr int RIGHT_CENTER_X = 238;

  canvas.drawFastVLine(
    160,
    52,
    135,
    Color::DIVIDER);

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

    snprintf(
      value,
      sizeof(value),
      "%.1f",
      sensorData.temperature);

  } else {

    snprintf(
      value,
      sizeof(value),
      "--.-");
  }

  drawLarge(
    value,
    LEFT_CENTER_X,
    119,
    middle_center,
    Color::ORANGE);

  if (sensorData.valid && !isnan(sensorData.humidity)) {

    snprintf(
      value,
      sizeof(value),
      "%.0f",
      sensorData.humidity);

  } else {

    snprintf(
      value,
      sizeof(value),
      "--");
  }

  drawLarge(
    value,
    RIGHT_CENTER_X,
    119,
    middle_center,
    Color::CYAN);

  canvas.drawCircle(
    LEFT_CENTER_X - 11,
    158,
    3,
    Color::ORANGE);

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

  constexpr int ROW_Y[] = {
    56,
    78,
    100,
    122,
    144,
    166,
    188
  };

  char value[32];

  drawSmall(
    "Wi-Fi",
    LABEL_X,
    ROW_Y[0],
    middle_left,
    Color::MUTED);

  if (WiFi.status() == WL_CONNECTED) {

    snprintf(
      value,
      sizeof(value),
      "%d dBm",
      WiFi.RSSI());

    drawSmall(
      value,
      VALUE_X,
      ROW_Y[0],
      middle_right,
      Color::GREEN);

  } else {

    drawSmall(
      "OFFLINE",
      VALUE_X,
      ROW_Y[0],
      middle_right,
      Color::RED);
  }

  drawSmall(
    "MQTT",
    LABEL_X,
    ROW_Y[1],
    middle_left,
    Color::MUTED);

  drawSmall(
    mqttClient.connected()
      ? "CONNECTED"
      : "OFFLINE",
    VALUE_X,
    ROW_Y[1],
    middle_right,
    mqttClient.connected()
      ? Color::GREEN
      : Color::RED);

  drawSmall(
    "Sensor",
    LABEL_X,
    ROW_Y[2],
    middle_left,
    Color::MUTED);

  drawSmall(
    "B3D8",
    VALUE_X,
    ROW_Y[2],
    middle_right,
    Color::TEXT);

  drawSmall(
    "Data age",
    LABEL_X,
    ROW_Y[3],
    middle_left,
    Color::MUTED);

  formatDataAge(
    value,
    sizeof(value));

  drawSmall(
    value,
    VALUE_X,
    ROW_Y[3],
    middle_right,
    dataStateColor(
      getDataState()));

  drawSmall(
    "SD",
    LABEL_X,
    ROW_Y[4],
    middle_left,
    Color::MUTED);

  drawSmall(
    sdAvailable
      ? "READY"
      : "OFFLINE",
    VALUE_X,
    ROW_Y[4],
    middle_right,
    sdAvailable
      ? Color::GREEN
      : Color::RED);

  drawSmall(
    "IMU",
    LABEL_X,
    ROW_Y[5],
    middle_left,
    Color::MUTED);

  drawSmall(
    imuAvailable
      ? "READY"
      : "OFFLINE",
    VALUE_X,
    ROW_Y[5],
    middle_right,
    imuAvailable
      ? Color::GREEN
      : Color::RED);

  drawSmall(
    "Uptime",
    LABEL_X,
    ROW_Y[6],
    middle_left,
    Color::MUTED);

  formatUptime(
    value,
    sizeof(value));

  drawSmall(
    value,
    VALUE_X,
    ROW_Y[6],
    middle_right,
    Color::TEXT);
}

void drawFooter() {

  canvas.drawFastHLine(
    14,
    207,
    292,
    Color::DIVIDER);

  canvas.fillCircle(
    18,
    226,
    4,
    mqttClient.connected()
      ? Color::GREEN
      : Color::RED);

  drawSmall(
    "MQTT",
    28,
    226,
    middle_left,
    Color::MUTED);

  if (currentPage == Page::Main) {

    const DataState state =
      getDataState();

    drawSmall(
      dataStateText(state),
      83,
      226,
      middle_left,
      dataStateColor(state));

    char age[24];

    formatDataAge(
      age,
      sizeof(age));

    drawSmall(
      age,
      306,
      226,
      middle_right,
      Color::MUTED);

    return;
  }

  char version[16];

  snprintf(
    version,
    sizeof(version),
    "v%s",
    App::VERSION);

  drawSmall(
    version,
    306,
    226,
    middle_right,
    Color::MUTED);
}

void drawScreen() {

  if (displaySleeping) {
    return;
  }

  reloadGraphIfNeeded();

  canvas.fillSprite(
    Color::BG);

  drawHeader();

  switch (currentPage) {

    case Page::Main:
      drawMainPage();
      break;

    case Page::Status:
      drawStatusPage();
      break;

    case Page::TemperatureGraph:
      drawGraph(true);
      break;

    case Page::HumidityGraph:
      drawGraph(false);
      break;
  }

  drawFooter();

  canvas.pushSprite(
    0,
    0);
}

void refreshDisplayIfDue(
  uint32_t now) {

  if (displaySleeping) {
    return;
  }

  if (!intervalElapsed(
        now,
        lastDisplayRefreshMs,
        App::DISPLAY_REFRESH_MS)) {

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

  // Establish a fresh acceleration baseline.
  previousAccelMagnitude = NAN;

  Serial.println(
    "Display: sleep");
}

void wakeDisplay() {

  displaySleeping = false;

  registerUserActivity();

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);

  drawScreen();

  Serial.println(
    "Display: wake");
}

void loadBrightness() {

  brightnessIndex =
    preferences.getUChar(
      "brightness",
      App::DEFAULT_BRIGHTNESS_INDEX);

  if (brightnessIndex >= App::BRIGHTNESS_LEVEL_COUNT) {

    brightnessIndex =
      App::DEFAULT_BRIGHTNESS_INDEX;
  }
}

void cycleBrightness() {

  brightnessIndex =
    (brightnessIndex + 1) % App::BRIGHTNESS_LEVEL_COUNT;

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);

  preferences.putUChar(
    "brightness",
    brightnessIndex);

  Serial.printf(
    "Display: brightness=%u\n",
    App::BRIGHTNESS_LEVELS[brightnessIndex]);
}

void maintainDisplaySleep(
  uint32_t now) {

  if (displaySleeping) {
    return;
  }

  if (intervalElapsed(
        now,
        lastUserActivityMs,
        App::DISPLAY_SLEEP_MS)) {

    sleepDisplay();
  }
}

// =============================================================================
// Retry helpers
// =============================================================================

void resetRetry(
  RetryState& retry,
  uint32_t minimumDelayMs) {

  retry.delayMs =
    minimumDelayMs;

  retry.nextAttemptMs = 0;
}

void scheduleRetry(
  RetryState& retry,
  uint32_t now,
  uint32_t maximumDelayMs) {

  retry.nextAttemptMs =
    now + retry.delayMs;

  if (retry.delayMs < maximumDelayMs) {

    retry.delayMs =
      min(
        retry.delayMs * 2UL,
        maximumDelayMs);
  }
}

// =============================================================================
// Wi-Fi
// =============================================================================

void startWiFiConnection() {

  Serial.printf(
    "WiFi: connecting to %s\n",
    WIFI_SSID);

  WiFi.disconnect(
    false,
    false);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD);
}

void configureWiFi() {

  WiFi.mode(
    WIFI_STA);

  WiFi.persistent(
    false);

  WiFi.setAutoReconnect(
    true);

  WiFi.setSleep(
    false);

  startWiFiConnection();

  wifiRetry.nextAttemptMs =
    millis() + App::WIFI_CONNECT_TIMEOUT_MS;
}

void maintainWiFi(
  uint32_t now) {

  static wl_status_t previousStatus =
    WL_NO_SHIELD;

  const wl_status_t status =
    WiFi.status();

  if (status != previousStatus) {

    previousStatus = status;

    if (status == WL_CONNECTED) {

      const IPAddress ip =
        WiFi.localIP();

      Serial.printf(
        "WiFi: connected IP=%u.%u.%u.%u RSSI=%d dBm\n",
        ip[0],
        ip[1],
        ip[2],
        ip[3],
        WiFi.RSSI());

      resetRetry(
        wifiRetry,
        App::WIFI_RETRY_MIN_MS);

      return;
    }

    Serial.printf(
      "WiFi: disconnected status=%d\n",
      status);
  }

  if (status == WL_CONNECTED) {
    return;
  }

  if (!deadlineReached(
        now,
        wifiRetry.nextAttemptMs)) {

    return;
  }

  Serial.printf(
    "WiFi: reconnecting, retry delay=%lu ms\n",
    static_cast<unsigned long>(
      wifiRetry.delayMs));

  startWiFiConnection();

  scheduleRetry(
    wifiRetry,
    now,
    App::WIFI_RETRY_MAX_MS);
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

  Serial.println(
    "NTP: configured");
}

void maintainTime() {

  if (!timeConfigured) {
    configureTimeIfNeeded();
  }

  if (!timeReadyLogged && isClockValid()) {

    timeReadyLogged = true;

    char date[8];
    char clock[8];

    formatDate(
      date,
      sizeof(date));

    formatClock(
      clock,
      sizeof(clock));

    Serial.printf(
      "NTP: synchronized %s %s\n",
      date,
      clock);
  }
}

// =============================================================================
// MQTT
// =============================================================================

void mqttCallback(
  char* topic,
  byte* payload,
  unsigned int length) {

  if (strcmp(
        topic,
        MQTT_TOPIC_ENV)
      != 0) {

    return;
  }

  JsonDocument document;

  const DeserializationError error =
    deserializeJson(
      document,
      payload,
      length);

  if (error) {

    Serial.printf(
      "MQTT: JSON error=%s\n",
      error.c_str());

    return;
  }

  if (!document["temperature"].is<float>() && !document["temperature"].is<int>()) {

    Serial.println(
      "MQTT: temperature missing/invalid");

    return;
  }

  if (!document["humidity"].is<float>() && !document["humidity"].is<int>()) {

    Serial.println(
      "MQTT: humidity missing/invalid");

    return;
  }

  const float temperature =
    document["temperature"].as<float>();

  const float humidity =
    document["humidity"].as<float>();

  if (!isfinite(temperature) || !isfinite(humidity) || temperature < -50.0f || temperature > 80.0f || humidity < 0.0f || humidity > 100.0f) {

    Serial.println(
      "MQTT: environmental value out of range");

    return;
  }

  sensorData.temperature =
    temperature;

  sensorData.humidity =
    humidity;

  sensorData.valid = true;

  sensorData.lastReceivedMs =
    millis();

  Serial.printf(
    "ENV: %.1f C %.1f %%\n",
    sensorData.temperature,
    sensorData.humidity);

  if (!displaySleeping) {

    drawScreen();

    lastDisplayRefreshMs =
      millis();
  }
}

void configureMQTT() {

  mqttClient.setServer(
    MQTT_HOST,
    MQTT_PORT);

  mqttClient.setCallback(
    mqttCallback);

  mqttClient.setBufferSize(
    512);

  mqttClient.setKeepAlive(
    App::MQTT_KEEPALIVE_SEC);

  mqttClient.setSocketTimeout(
    App::MQTT_SOCKET_TIMEOUT_SEC);
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

    connected =
      mqttClient.connect(
        MQTT_CLIENT_ID);
  }

  if (!connected) {

    Serial.printf(
      "MQTT: connect failed state=%d\n",
      mqttClient.state());

    return false;
  }

  if (!mqttClient.subscribe(
        MQTT_TOPIC_ENV,
        0)) {

    Serial.println(
      "MQTT: subscribe failed");

    mqttClient.disconnect();

    return false;
  }

  Serial.printf(
    "MQTT: connected and subscribed %s\n",
    MQTT_TOPIC_ENV);

  return true;
}

void maintainMQTT(
  uint32_t now) {

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

  if (!deadlineReached(
        now,
        mqttRetry.nextAttemptMs)) {

    return;
  }

  if (connectMQTT()) {

    resetRetry(
      mqttRetry,
      App::MQTT_RETRY_MIN_MS);

    return;
  }

  scheduleRetry(
    mqttRetry,
    now,
    App::MQTT_RETRY_MAX_MS);
}

void forceNetworkReconnect() {

  Serial.println(
    "Network: manual reconnect");

  mqttClient.disconnect();

  WiFi.disconnect(
    false,
    false);

  resetRetry(
    wifiRetry,
    App::WIFI_RETRY_MIN_MS);

  resetRetry(
    mqttRetry,
    App::MQTT_RETRY_MIN_MS);

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

  switch (currentPage) {

    case Page::Main:
      currentPage = Page::Status;
      break;

    case Page::Status:
      currentPage = Page::TemperatureGraph;
      loadGraphData();
      break;

    case Page::TemperatureGraph:
      currentPage = Page::HumidityGraph;
      break;

    case Page::HumidityGraph:
    default:
      currentPage = Page::Main;
      break;
  }

  drawScreen();
}

void handleButtonBLong() {

  if (displaySleeping) {

    wakeDisplay();
    return;
  }

  registerUserActivity();

  Serial.println(
    "Button B: long press reserved");
}

void handleButtonCShort() {

  if (displaySleeping) {

    wakeDisplay();
    return;
  }

  registerUserActivity();

  currentPage =
    Page::Main;

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

    state.pressedAtMs =
      now;

    state.longPressHandled =
      false;
  }

  if (pressed && !state.longPressHandled && intervalElapsed(now, state.pressedAtMs, App::LONG_PRESS_MS)) {

    state.longPressHandled =
      true;

    longPressHandler();
  }

  if (!pressed && state.wasPressed && !state.longPressHandled) {

    shortPressHandler();
  }

  state.wasPressed =
    pressed;
}

void processButtons(
  uint32_t now) {

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

const char* resetReasonText(
  esp_reset_reason_t reason) {

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

  const esp_reset_reason_t reason =
    esp_reset_reason();

  Serial.printf(
    "Boot: reset_reason=%s (%d)\n",
    resetReasonText(reason),
    static_cast<int>(reason));

  Serial.printf(
    "Heap: free=%u largest=%u min_free=%u\n",
    ESP.getFreeHeap(),
    heap_caps_get_largest_free_block(
      MALLOC_CAP_8BIT),
    ESP.getMinFreeHeap());
}

void logHealthIfDue(
  uint32_t now) {

  if (!intervalElapsed(
        now,
        lastHealthLogMs,
        App::HEALTH_LOG_INTERVAL_MS)) {

    return;
  }

  lastHealthLogMs =
    now;

  char uptime[24];

  formatUptime(
    uptime,
    sizeof(uptime));

  Serial.printf(
    "HEALTH: uptime=%s wifi=%s mqtt=%s rssi=%d "
    "heap=%u min_heap=%u largest=%u data=%s age_s=%lu "
    "sd=%s imu=%s\n",
    uptime,
    WiFi.status() == WL_CONNECTED
      ? "up"
      : "down",
    mqttClient.connected()
      ? "up"
      : "down",
    WiFi.status() == WL_CONNECTED
      ? WiFi.RSSI()
      : 0,
    ESP.getFreeHeap(),
    ESP.getMinFreeHeap(),
    heap_caps_get_largest_free_block(
      MALLOC_CAP_8BIT),
    dataStateText(
      getDataState()),
    sensorData.valid
      ? static_cast<unsigned long>(
        sensorAgeMs() / 1000UL)
      : 0UL,
    sdAvailable
      ? "ready"
      : "offline",
    imuAvailable
      ? "ready"
      : "offline");
}

// =============================================================================
// Setup
// =============================================================================

void setupDisplay() {

  M5.Display.setRotation(1);

  preferences.begin(
    "envmonitor",
    false);

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
    heap_caps_get_largest_free_block(
      MALLOC_CAP_8BIT));

  canvas.setColorDepth(8);

  if (canvas.createSprite(
        App::SCREEN_WIDTH,
        App::SCREEN_HEIGHT)
      == nullptr) {

    Serial.println(
      "FATAL: canvas allocation failed");

    while (true) {
      delay(1000);
    }
  }

  canvas.setTextWrap(false);

  lastUserActivityMs =
    millis();

  lastDisplayRefreshMs =
    millis();

  drawScreen();
}

// =============================================================================
// Arduino entry points
// =============================================================================

void setup() {

  auto config =
    M5.config();

  config.internal_imu = true;

  M5.begin(config);

  Serial.begin(115200);

  delay(100);

  Serial.printf(
    "\n%s v%s\n",
    App::NAME,
    App::VERSION);

  logBootDiagnostics();

  setupDisplay();

  initializeImu();

  initializeSdCard();

  configureMQTT();

  configureWiFi();
}

void loop() {

  const uint32_t now =
    millis();

  M5.update();

  processButtons(now);

  maintainWiFi(now);

  maintainTime();

  maintainMQTT(now);

  maintainSdLogging(now);

  maintainDisplaySleep(now);

  maintainImuWake(now);

  refreshDisplayIfDue(now);

  logHealthIfDue(now);

  delay(2);
}