#include <M5Unified.h>
#include <M5_STHS34PF80.h>
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
#include <esp32-hal-rmt.h>

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
constexpr char VERSION[] = "0.7.0";
constexpr uint32_t UNIT_I2C_HZ = 100000;
constexpr uint32_t TMOS_POLL_MS = 50;

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
constexpr char MQTT_TOPIC_STUDY_LIGHT_TOGGLE[] =
  "home/control/study/ceiling_light/toggle";
constexpr char MQTT_LIGHT_PAYLOAD[] = "PRESS";
constexpr uint32_t LIGHT_LED_FRAME_MS = 30UL;
constexpr char LIGHT_TOGGLE_SOUND_PATH[] = "/sounds/light-toggle.wav";
constexpr uint8_t LIGHT_TOGGLE_SOUND_VOLUME = 48;  // M5Unified range: 0..255
constexpr uint8_t LIGHT_SOUND_CHANNEL = 0;
constexpr size_t LIGHT_SOUND_BUFFER_BYTES = 8192;
constexpr uint16_t MQTT_KEEPALIVE_SEC = 30;
constexpr uint16_t MQTT_SOCKET_TIMEOUT_SEC = 5;

constexpr uint32_t DATA_STALE_MS = 3UL * 60UL * 1000UL;
constexpr uint32_t DATA_OFFLINE_MS = 10UL * 60UL * 1000UL;

constexpr uint32_t HEALTH_LOG_INTERVAL_MS = 5UL * 60UL * 1000UL;

// microSD
constexpr int SD_CS_PIN = 4;
constexpr uint32_t SD_LOG_INTERVAL_MS = 60UL * 1000UL;
constexpr uint32_t GRAPH_RELOAD_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr size_t GRAPH_POINT_COUNT = 288;
constexpr uint32_t GRAPH_BUCKET_SECONDS = 5UL * 60UL;

// IMU
constexpr uint32_t IMU_SAMPLE_INTERVAL_MS = 100UL;
constexpr float IMU_WAKE_DELTA_G = 0.22f;
constexpr uint32_t IMU_WAKE_COOLDOWN_MS = 3000UL;

constexpr float IMU_TILT_THRESHOLD_G = 0.55f;
constexpr float IMU_TILT_RELEASE_G = 0.30f;
constexpr uint32_t IMU_TILT_HOLD_MS = 500UL;
constexpr uint32_t IMU_TILT_COOLDOWN_MS = 1500UL;

// Set true if physical left/right is reversed on the installed orientation.
constexpr bool IMU_REVERSE_LEFT_RIGHT = false;

// M5GO v2.7 SK6812 x10
constexpr int RGB_LED_PIN = 15;
constexpr size_t RGB_LED_COUNT = 10;
constexpr uint8_t RGB_LED_BRIGHTNESS = 12;

// Environmental thresholds.
// NORMAL: all LEDs off.
// WARNING: first 3 LEDs amber.
// CRITICAL: all 10 LEDs red.
constexpr float TEMP_WARNING_LOW_C = 18.0f;
constexpr float TEMP_WARNING_HIGH_C = 28.0f;
constexpr float TEMP_CRITICAL_LOW_C = 15.0f;
constexpr float TEMP_CRITICAL_HIGH_C = 32.0f;

constexpr float HUM_WARNING_LOW_PCT = 40.0f;
constexpr float HUM_WARNING_HIGH_PCT = 70.0f;
constexpr float HUM_CRITICAL_LOW_PCT = 30.0f;
constexpr float HUM_CRITICAL_HIGH_PCT = 80.0f;

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

// Stable NVS page IDs; keep existing values when adding pages.
enum class Page : uint8_t {
  Main = 0,
  Status = 1,
  TemperatureGraph = 2,
  HumidityGraph = 3,
  TemperatureStats = 4,
  HumidityStats = 5,
};

enum class DataState {
  Waiting,
  Live,
  Stale,
  Offline,
};

enum class EnvironmentState {
  Normal,
  Warning,
  Critical,
};

enum class TiltDirection {
  None,
  Left,
  Right,
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

struct MetricStats {
  bool valid = false;
  uint32_t count = 0;
  float current = NAN;
  float average = NAN;
  float minimum = NAN;
  float maximum = NAN;
  bool previousValid = false;
  uint32_t previousCount = 0;
  float previousAverage = NAN;
};

// =============================================================================
// Global state
// =============================================================================

M5Canvas canvas(&M5.Display);
Preferences preferences;
bool preferencesAvailable = false;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

Page currentPage = Page::Main;
SensorData sensorData;

ButtonState buttonA;
ButtonState buttonB;
ButtonState buttonC;

RetryState wifiRetry{ 0, App::WIFI_RETRY_MIN_MS };
RetryState mqttRetry{ 0, App::MQTT_RETRY_MIN_MS };

GraphPoint graphPoints[App::GRAPH_POINT_COUNT];

// Historical results only; NOW continues to use the latest MQTT sample.
struct StatsCache {
  MetricStats temperature, humidity;
  bool valid = false, dirty = true;
  time_t calculatedAt = 0;
};
StatsCache statsCache;

// The displayed 24 h snapshot advances in whole 5 min buckets, as before.
// Keep the in-progress bucket separately until it enters the displayed window.
struct GraphCache {
  bool valid = false, dirty = true, attempted = false;
  bool loading = false, readFailed = false;
  time_t windowStart = 0, checkedAt = 0;
  uint32_t checkedMs = 0;
  GraphPoint pending;
};
GraphCache graphCache;

bool displaySleeping = false;
bool timeConfigured = false;
bool timeReadyLogged = false;
bool sdAvailable = false;
bool imuAvailable = false;
bool rgbAvailable = false;
bool lightFeedbackActive = false;
uint32_t lightFeedbackStartedMs = 0;
uint8_t lightFeedbackFrame = 255;

uint8_t brightnessIndex = App::DEFAULT_BRIGHTNESS_INDEX;

uint32_t lastUserActivityMs = 0;
uint32_t lastDisplayRefreshMs = 0;
uint32_t lastHealthLogMs = 0;
uint32_t lastSdLogMs = 0;
uint32_t lastGraphReloadMs = 0;
uint32_t lastImuSampleMs = 0;
uint32_t lastImuWakeMs = 0;
uint32_t lastTiltActionMs = 0;
uint32_t tiltStartedMs = 0;

float previousAccelMagnitude = NAN;
TiltDirection pendingTilt = TiltDirection::None;
bool tiltLatched = false;

// Diagnostic instrumentation only; cumulative counters since boot, times in us.
namespace Perf {
struct Timing {
  uint32_t last = 0, maximum = 0, calls = 0;
  void add(uint32_t us) { last = us; if (us > maximum) maximum = us; ++calls; }
};
struct Latency {
  uint32_t maximum = 0, over[5] = {};
  void add(uint32_t us) {
    if (us > maximum) maximum = us;
    constexpr uint32_t limits[] = {20000, 50000, 100000, 500000, 1000000};
    for (unsigned i = 0; i < 5; ++i) if (us > limits[i]) ++over[i];
  }
};
Timing stats, graph, draw, push, mqttConnect, mqttLoop, sdlog, nvs, output;
Timing tmosPoll;
uint32_t tmosErrors = 0;
Latency gap, loop;
uint32_t cacheHit = 0, cacheMiss = 0, cacheRefresh = 0;
uint32_t graphHit = 0, graphMiss = 0, graphRebuild = 0, graphIncremental = 0;
uint32_t lastUpdate = 0;
bool haveUpdate = false, bChanging = false;
uint32_t* csvFiles = nullptr;
uint32_t* csvLines = nullptr;
uint32_t statsFiles = 0, statsLines = 0, graphFiles = 0, graphLines = 0;
const char* pageName(Page p) {
  constexpr const char* names[] = {"MAIN", "SYSTEM_STATUS", "TEMP_24H",
    "HUM_24H", "TEMP_STATS", "HUM_STATS"};
  return names[static_cast<uint8_t>(p)];
}
struct Scope {
  Timing& metric; uint32_t start;
  explicit Scope(Timing& m) : metric(m), start(micros()) {}
  ~Scope() { metric.add(static_cast<uint32_t>(micros() - start)); }
};
// Spread summary over loops. One complete line only when UART TX has room.
void summary() {
  static uint32_t lastSummary = 0;
  static uint8_t row = 0;
  static char text[192];
  static size_t length = 0, offset = 0;
  const uint32_t begin = micros();
  if (row == 0 && static_cast<uint32_t>(millis() - lastSummary) >= 10000) {
    lastSummary = millis(); row = 1;
  }
  if (!row) return;
  if (offset == length) {
    offset = 0;
    if (row == 1) {
      length = snprintf(text, sizeof(text), "PERF SUMMARY uptime=%lu page=%s cumulative units=us\n",
        (unsigned long)(millis()/1000), pageName(currentPage));
    } else if (row == 15) {
      length = snprintf(text, sizeof(text), "tmos_poll_max_us=%lu last_us=%lu calls=%lu\n",
        (unsigned long)tmosPoll.maximum, (unsigned long)tmosPoll.last, (unsigned long)tmosPoll.calls);
    } else if (row == 16) {
      length = snprintf(text, sizeof(text), "tmos_errors=%lu\n", (unsigned long)tmosErrors);
    } else if (row == 14) {
      length = snprintf(text, sizeof(text), "graph_cache hit=%lu miss=%lu rebuild=%lu incremental=%lu dirty=%u valid=%u\n",
        (unsigned long)graphHit, (unsigned long)graphMiss, (unsigned long)graphRebuild,
        (unsigned long)graphIncremental, graphCache.dirty ? 1U : 0U, graphCache.valid ? 1U : 0U);
    } else if (row == 13) {
      length = snprintf(text, sizeof(text), "stats_cache hit=%lu miss=%lu refresh=%lu dirty=%u valid=%u\n",
        (unsigned long)cacheHit, (unsigned long)cacheMiss, (unsigned long)cacheRefresh,
        statsCache.dirty ? 1U : 0U, statsCache.valid ? 1U : 0U);
    } else if (row <= 3) {
      const Latency& m = row == 2 ? gap : loop;
      length = snprintf(text, sizeof(text), "%s max=%lu >20=%lu >50=%lu >100=%lu >500=%lu >1000=%lu\n",
        row == 2 ? "update_gap" : "loop", (unsigned long)m.maximum,
        (unsigned long)m.over[0], (unsigned long)m.over[1], (unsigned long)m.over[2],
        (unsigned long)m.over[3], (unsigned long)m.over[4]);
    } else {
      Timing* metrics[] = {&stats,&graph,&draw,&push,&mqttConnect,&mqttLoop,&sdlog,&nvs,&output};
      const char* names[] = {"stats","graph","draw","push","mqtt_connect","mqtt_loop","sdlog","nvs_page","perf_output"};
      const unsigned i = row - 4;
      const Timing& m = *metrics[i];
      length = snprintf(text, sizeof(text), "%s last=%lu max=%lu calls=%lu",
        names[i], (unsigned long)m.last, (unsigned long)m.maximum, (unsigned long)m.calls);
      if (i < 2) length += snprintf(text + length, sizeof(text) - length,
        " files=%lu lines=%lu", (unsigned long)(i ? graphFiles : statsFiles),
        (unsigned long)(i ? graphLines : statsLines));
      length += snprintf(text + length, sizeof(text) - length, "\n");
    }
  }
  int room = Serial.availableForWrite();
  if (room >= static_cast<int>(length - offset)) {
    offset += Serial.write(reinterpret_cast<const uint8_t*>(text + offset), length - offset);
  }
  if (offset == length) { if (++row > 16) row = 0; }
  output.add(static_cast<uint32_t>(micros() - begin));
}
}  // namespace Perf

// =============================================================================
// Optional Port A units: share M5Unified's existing bus with the internal IMU.
// =============================================================================

class EnvTmos : public M5_STHS34PF80 {
 public:
  bool beginShared() {
    initializing = true; failed = false; started = millis();
    sensor.handle = this; sensor.read_reg = readBus; sensor.write_reg = writeBus; sensor.mdelay = pause;
    bool ok = init() == 0;
    if (ok) ok = setPresenceThreshold(200) == 0 && setMotionThreshold(200) == 0 &&
      setPresenceHysteresis(50) == 0 && setMotionHysteresis(50) == 0 &&
      setTmosODR(STHS34PF80_TMOS_ODR_AT_8Hz) == 0;
    initializing = false;
    return ok && !failed;
  }
 private:
  bool initializing = false, failed = false;
  uint32_t started = 0;
  bool expired() { return initializing && static_cast<uint32_t>(millis() - started) >= 500; }
  static void pause(uint32_t ms) { delay(ms); }  // Official reset, setup only.
  static int32_t readBus(void* handle, uint8_t reg, uint8_t* data, uint16_t len) {
    auto& self = *static_cast<EnvTmos*>(handle);
    memset(data, 0, len);  // Error/expiry also terminates the ST DRDY wait.
    if (!self.expired() && M5.In_I2C.readRegister(0x5A, reg, data, len, App::UNIT_I2C_HZ)) return 0;
    memset(data, 0, len);
    self.failed = true;
    return -1;
  }
  static int32_t writeBus(void* handle, uint8_t reg, const uint8_t* data, uint16_t len) {
    auto& self = *static_cast<EnvTmos*>(handle);
    if (!self.expired() && M5.In_I2C.writeRegister(0x5A, reg, data, len, App::UNIT_I2C_HZ)) return 0;
    self.failed = true;
    return -1;
  }
};
EnvTmos tmosUnit;
bool tmosAvailable = false, tmosReadOk = true;
bool tmosPresence = false, tmosMotion = false;
uint32_t lastTmosPollMs = 0;

void initializePortAUnits() {
  // M5GO Port A and internal MPU6886 share GPIO21/22. Never release/reinitialize the bus.
  if (M5.In_I2C.getSDA() != 21 || M5.In_I2C.getSCL() != 22) {
    Serial.println("UNITS: unexpected internal bus pins - disabled"); return;
  }
  if (M5.In_I2C.scanID(0x5A, App::UNIT_I2C_HZ)) tmosAvailable = tmosUnit.beginShared();
  Serial.printf("TMOS: %s\n", tmosAvailable ? "ready ODR=8Hz" : "unavailable");

}

void maintainPortAUnits() {
  uint32_t now = millis();
  if (tmosAvailable && static_cast<uint32_t>(now - lastTmosPollMs) >= App::TMOS_POLL_MS) {
    lastTmosPollMs = now;
    sths34pf80_tmos_drdy_status_t ready{};
    sths34pf80_tmos_func_status_t status{};
    bool ok;
    { Perf::Scope timer(Perf::tmosPoll);
      ok = tmosUnit.getDataReady(&ready) == 0;
      if (ok && ready.drdy) ok = tmosUnit.getStatus(&status) == 0;
    }
    if (!ok) ++Perf::tmosErrors;
    if (ok != tmosReadOk) { Serial.printf("TMOS: read %s\n", ok ? "recovered" : "failed"); tmosReadOk = ok; }
    if (ok && ready.drdy) { tmosPresence = status.pres_flag; tmosMotion = status.mot_flag; }
  }

}

// =============================================================================
// Persistent UI state
// =============================================================================

void setCurrentPage(Page page) {
  if (page == currentPage) return;

  const Page previous = currentPage;
  currentPage = page;
  if (Perf::bChanging) Serial.printf("PERF B page t=%lu %s->%s\n",
    (unsigned long)millis(), Perf::pageName(previous), Perf::pageName(page));
  if (preferencesAvailable) {
    Perf::Scope timer(Perf::nvs);
    if (preferences.putUChar("page", static_cast<uint8_t>(page)) != 1) {
      Serial.println("NVS: page save failed");
    }
  }
}

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
  constexpr time_t MIN_VALID_TIME = 1704067200;
  return time(nullptr) >= MIN_VALID_TIME;
}

bool getLocalTimeSafe(struct tm& localTime) {
  if (!isClockValid()) return false;
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
  snprintf(buffer, size, "%02d/%02d", localTime.tm_mon + 1, localTime.tm_mday);
}

void formatUptime(char* buffer, size_t size) {
  const uint32_t totalSeconds = millis() / 1000UL;
  const uint32_t days = totalSeconds / 86400UL;
  const uint32_t hours = (totalSeconds % 86400UL) / 3600UL;
  const uint32_t minutes = (totalSeconds % 3600UL) / 60UL;

  if (days > 0) {
    snprintf(buffer, size, "%lud %02luh",
             static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours));
  } else {
    snprintf(buffer, size, "%luh %02lum",
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  }
}

void formatTimestamp(time_t value, char* buffer, size_t size) {
  struct tm localTime;
  localtime_r(&value, &localTime);
  snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d",
           localTime.tm_year + 1900,
           localTime.tm_mon + 1,
           localTime.tm_mday,
           localTime.tm_hour,
           localTime.tm_min,
           localTime.tm_sec);
}

// =============================================================================
// Data freshness
// =============================================================================

uint32_t sensorAgeMs() {
  if (!sensorData.valid) return 0;
  return static_cast<uint32_t>(millis() - sensorData.lastReceivedMs);
}

DataState getDataState() {
  if (!sensorData.valid) return DataState::Waiting;

  const uint32_t ageMs = sensorAgeMs();

  if (ageMs < App::DATA_STALE_MS) return DataState::Live;
  if (ageMs < App::DATA_OFFLINE_MS) return DataState::Stale;
  return DataState::Offline;
}

const char* dataStateText(DataState state) {
  switch (state) {
    case DataState::Live: return "LIVE";
    case DataState::Stale: return "STALE";
    case DataState::Offline: return "OFFLINE";
    case DataState::Waiting:
    default: return "WAIT";
  }
}

uint16_t dataStateColor(DataState state) {
  switch (state) {
    case DataState::Live: return Color::GREEN;
    case DataState::Stale: return Color::YELLOW;
    case DataState::Offline: return Color::RED;
    case DataState::Waiting:
    default: return Color::PURPLE;
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
    snprintf(buffer, size, "%lum ago",
             static_cast<unsigned long>(ageSeconds / 60UL));
  } else if (ageSeconds < 86400UL) {
    snprintf(buffer, size, "%luh ago",
             static_cast<unsigned long>(ageSeconds / 3600UL));
  } else {
    snprintf(buffer, size, "%lud ago",
             static_cast<unsigned long>(ageSeconds / 86400UL));
  }
}

// =============================================================================
// Environment state / RGB LEDs
// =============================================================================

EnvironmentState getEnvironmentState() {
  if (!sensorData.valid) return EnvironmentState::Normal;

  const float t = sensorData.temperature;
  const float h = sensorData.humidity;

  if (t < App::TEMP_CRITICAL_LOW_C || t > App::TEMP_CRITICAL_HIGH_C || h < App::HUM_CRITICAL_LOW_PCT || h > App::HUM_CRITICAL_HIGH_PCT) {
    return EnvironmentState::Critical;
  }

  if (t < App::TEMP_WARNING_LOW_C || t > App::TEMP_WARNING_HIGH_C || h < App::HUM_WARNING_LOW_PCT || h > App::HUM_WARNING_HIGH_PCT) {
    return EnvironmentState::Warning;
  }

  return EnvironmentState::Normal;
}

const char* environmentStateText(EnvironmentState state) {
  switch (state) {
    case EnvironmentState::Warning: return "WARNING";
    case EnvironmentState::Critical: return "CRITICAL";
    case EnvironmentState::Normal:
    default: return "NORMAL";
  }
}

// SK6812/WS2812-compatible signal generation using the Arduino-ESP32 3.x
// RMT API. M5GO v2.7 connects the 10-LED strip to GPIO15.
//
// RMT runs at 10 MHz, so one tick is 100 ns.
// 0 bit: 0.4 us HIGH + 0.8 us LOW
// 1 bit: 0.8 us HIGH + 0.4 us LOW
//
// SK6812 on M5GO uses GRB byte order.
constexpr size_t RGB_RMT_SYMBOL_COUNT =
  App::RGB_LED_COUNT * 24;

rmt_data_t rgbRmtData[RGB_RMT_SYMBOL_COUNT];
uint32_t rgbLedColors[App::RGB_LED_COUNT] = {};

void encodeRgbBit(
  rmt_data_t& symbol,
  bool one) {

  symbol.level0 = 1;
  symbol.duration0 = one ? 8 : 4;
  symbol.level1 = 0;
  symbol.duration1 = one ? 4 : 8;
}

void encodeRgbByte(
  uint8_t value,
  size_t& symbolIndex) {

  for (int bit = 7; bit >= 0; --bit) {

    encodeRgbBit(
      rgbRmtData[symbolIndex++],
      (value & (1U << bit)) != 0);
  }
}

bool transmitRgbLeds() {

  if (!rgbAvailable) {
    return false;
  }

  size_t symbolIndex = 0;

  for (size_t i = 0;
       i < App::RGB_LED_COUNT;
       ++i) {

    const uint32_t color =
      rgbLedColors[i];

    const uint8_t red =
      static_cast<uint8_t>(
        (color >> 16) & 0xFF);

    const uint8_t green =
      static_cast<uint8_t>(
        (color >> 8) & 0xFF);

    const uint8_t blue =
      static_cast<uint8_t>(
        color & 0xFF);

    // SK6812 / NeoPixel-compatible GRB order.
    encodeRgbByte(
      green,
      symbolIndex);

    encodeRgbByte(
      red,
      symbolIndex);

    encodeRgbByte(
      blue,
      symbolIndex);
  }

  return rmtWrite(
    App::RGB_LED_PIN,
    rgbRmtData,
    RGB_RMT_SYMBOL_COUNT,
    100);
}

void setRgbLed(
  size_t index,
  uint32_t color) {

  if (!rgbAvailable || index >= App::RGB_LED_COUNT) {

    return;
  }

  rgbLedColors[index] =
    color;
}

void clearRgbLeds() {

  if (!rgbAvailable) {
    return;
  }

  for (size_t i = 0;
       i < App::RGB_LED_COUNT;
       ++i) {

    rgbLedColors[i] =
      0x000000;
  }

  transmitRgbLeds();
}

void updateEnvironmentLeds() {
  // MQTT may update sensorData during feedback; restore the latest state later.
  if (lightFeedbackActive) return;

  if (!rgbAvailable) {
    return;
  }

  for (size_t i = 0;
       i < App::RGB_LED_COUNT;
       ++i) {

    rgbLedColors[i] =
      0x000000;
  }

  const EnvironmentState state =
    getEnvironmentState();

  if (state == EnvironmentState::Warning) {

    const uint32_t amber =
      (static_cast<uint32_t>(
         App::RGB_LED_BRIGHTNESS)
       << 16)
      | (static_cast<uint32_t>(
           App::RGB_LED_BRIGHTNESS / 2)
         << 8);

    for (size_t i = 0;
         i < 3;
         ++i) {

      setRgbLed(
        i,
        amber);
    }

  } else if (
    state == EnvironmentState::Critical) {

    const uint32_t red =
      static_cast<uint32_t>(
        App::RGB_LED_BRIGHTNESS)
      << 16;

    for (size_t i = 0;
         i < App::RGB_LED_COUNT;
         ++i) {

      setRgbLed(
        i,
        red);
    }
  }

  if (!transmitRgbLeds()) {

    Serial.println(
      "RGB: transmit failed");
  }
}

// SD PCM streaming: three buffers keep queued data alive until consumed.
File lightSoundFile;
int16_t lightSoundBuffers[3][App::LIGHT_SOUND_BUFFER_BYTES / 2];
bool lightSoundActive = false;
uint32_t lightSoundRemaining = 0;
uint32_t lightSoundRate = 0;
bool lightSoundStereo = false;
uint8_t lightSoundBufferIndex = 0;

uint32_t wavLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool readLightSoundHeader() {
  uint8_t header[16];
  const uint32_t fileSize = lightSoundFile.size();
  if (lightSoundFile.read(header, 12) != 12 ||
      memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4)) return false;
  const uint32_t riffSize = wavLe32(header + 4);
  if (riffSize < 4 || riffSize > fileSize - 8) return false;
  const uint32_t end = riffSize + 8;
  bool formatReady = false;
  // Bounded scan of RIFF chunks; no decoder or whole-file allocation.
  for (unsigned chunks = 0; chunks < 64; ++chunks) {
    const uint32_t position = lightSoundFile.position();
    if (position > end || end - position < 8 ||
        lightSoundFile.read(header, 8) != 8) return false;
    const uint32_t length = wavLe32(header + 4);
    const uint32_t dataStart = position + 8;
    if (length > end - dataStart) return false;
    const bool isFormat = !memcmp(header, "fmt ", 4);
    const bool isData = !memcmp(header, "data", 4);
    if (isFormat) {
      if (length < 16 || lightSoundFile.read(header, 16) != 16) return false;
      const uint16_t channels = header[2] | (uint16_t(header[3]) << 8);
      const uint16_t alignment = header[12] | (uint16_t(header[13]) << 8);
      lightSoundRate = wavLe32(header + 4);
      if (header[0] != 1 || header[1] != 0 ||
          (channels != 1 && channels != 2) || header[14] != 16 || header[15] != 0 ||
          alignment != channels * 2 || lightSoundRate < 8000 || lightSoundRate > 48000 ||
          wavLe32(header + 8) != lightSoundRate * alignment) return false;
      lightSoundStereo = channels == 2;
      formatReady = true;
    } else if (isData) {
      if (!formatReady || length == 0 || length % (lightSoundStereo ? 4 : 2)) return false;
      lightSoundRemaining = length;
      return true;  // File position now points to PCM, not RIFF metadata.
    }
    const uint32_t padding = length & 1;
    if (padding > end - dataStart - length ||
        !lightSoundFile.seek(dataStart + length + padding)) return false;
  }
  return false;
}

void maintainLightSound() {
  if (!lightSoundActive) return;
  const size_t queued = M5.Speaker.isPlaying(App::LIGHT_SOUND_CHANNEL);
  if (lightSoundRemaining == 0) {
    if (queued == 0) lightSoundActive = false;
    return;
  }
  if (queued >= 2) return;  // Never wait for a Speaker queue slot.

  const size_t bytes = lightSoundRemaining < App::LIGHT_SOUND_BUFFER_BYTES
    ? lightSoundRemaining : App::LIGHT_SOUND_BUFFER_BYTES;
  int16_t* buffer = lightSoundBuffers[lightSoundBufferIndex];
  if (lightSoundFile.read(reinterpret_cast<uint8_t*>(buffer), bytes) != bytes ||
      !M5.Speaker.isRunning() ||
      !M5.Speaker.playRaw(buffer, bytes / sizeof(int16_t), lightSoundRate,
                          lightSoundStereo, 1, App::LIGHT_SOUND_CHANNEL, false)) {
    Serial.println("AUDIO: playback failed");
    lightSoundFile.close();
    lightSoundRemaining = 0;
    // Drain existing queued buffers before allowing a new sound to reuse RAM.
    return;
  }
  lightSoundBufferIndex = (lightSoundBufferIndex + 1) % 3;
  lightSoundRemaining -= bytes;
  if (lightSoundRemaining == 0) lightSoundFile.close();
}

void startLightSound() {
  if (lightSoundActive || M5.Speaker.isPlaying(App::LIGHT_SOUND_CHANNEL)) {
    Serial.println("AUDIO: busy - feedback skipped");
    return;
  }
  if (!sdAvailable) {
    Serial.println("AUDIO: SD unavailable");
    return;
  }
  lightSoundFile = SD.open(App::LIGHT_TOGGLE_SOUND_PATH, FILE_READ);
  if (!lightSoundFile) {
    Serial.printf("AUDIO: cannot open %s\n", App::LIGHT_TOGGLE_SOUND_PATH);
    return;
  }
  if (!readLightSoundHeader()) {
    Serial.println("AUDIO: unsupported or invalid WAV (16-bit PCM mono/stereo required)");
    lightSoundFile.close();
    return;
  }
  if (!M5.Speaker.begin()) {
    Serial.println("AUDIO: speaker initialization failed");
    lightSoundFile.close();
    return;
  }
  M5.Speaker.setVolume(App::LIGHT_TOGGLE_SOUND_VOLUME);
  lightSoundBufferIndex = 0;
  lightSoundActive = true;
  maintainLightSound();  // Queue the first chunk, not the full 2.5 seconds.
  if (lightSoundRemaining || M5.Speaker.isPlaying(App::LIGHT_SOUND_CHANNEL)) {
    Serial.printf("AUDIO: playing %s\n", App::LIGHT_TOGGLE_SOUND_PATH);
  }
}

// One subdued moving LED, blue -> cyan -> aqua -> muted green.
// Scaled from the requested palette to the existing channel intensity 12.
void maintainLightFeedback(uint32_t now) {
  if (!lightFeedbackActive) return;

  const uint32_t frame =
    static_cast<uint32_t>(now - lightFeedbackStartedMs) / App::LIGHT_LED_FRAME_MS;
  if (frame >= App::RGB_LED_COUNT) {
    lightFeedbackActive = false;
    updateEnvironmentLeds();
    return;
  }
  if (frame == lightFeedbackFrame) return;
  lightFeedbackFrame = static_cast<uint8_t>(frame);

  constexpr uint32_t palette[] = {
    0x040608, 0x060709, 0x06090A, 0x060908, 0x070806
  };
  for (size_t i = 0; i < App::RGB_LED_COUNT; ++i) rgbLedColors[i] = 0;
  setRgbLed(frame, palette[frame * 4 / (App::RGB_LED_COUNT - 1)]);
  if (!transmitRgbLeds()) {
    Serial.println("RGB: light feedback transmit failed");
    lightFeedbackActive = false;
    updateEnvironmentLeds();
  }
}

void publishStudyLightToggle() {
  if (!mqttClient.connected()) {
    Serial.println("LIGHT: publish failed - MQTT disconnected");
    return;
  }
  if (!mqttClient.publish(App::MQTT_TOPIC_STUDY_LIGHT_TOGGLE,
                          App::MQTT_LIGHT_PAYLOAD, false)) {
    Serial.println("LIGHT: publish failed");
    return;
  }

  // PubSubClient success is a local send result, not light-state confirmation.
  Serial.println("LIGHT: toggle command published");
  if (rgbAvailable) {
    lightFeedbackStartedMs = millis();
    lightFeedbackFrame = 255;
    lightFeedbackActive = true;
    maintainLightFeedback(lightFeedbackStartedMs);
  }
  startLightSound();
}

void initializeRgbLeds() {

  // M5Stack recommends open-drain initialization for the M5GO GPIO15
  // LED-strip signal pin.
  pinMode(
    App::RGB_LED_PIN,
    OUTPUT_OPEN_DRAIN);

  // 10 LEDs x 24 bits = 240 RMT symbols.
  // Classic ESP32 has 64 symbols per block, therefore four blocks provide
  // enough room for one complete frame (256 symbols).
  if (!rmtInit(
        App::RGB_LED_PIN,
        RMT_TX_MODE,
        RMT_MEM_NUM_BLOCKS_4,
        10000000)) {

    rgbAvailable = false;

    Serial.println(
      "RGB: RMT initialization failed");

    return;
  }

  rgbAvailable = true;

  for (size_t i = 0;
       i < App::RGB_LED_COUNT;
       ++i) {

    rgbLedColors[i] =
      0x000000;
  }

  if (!transmitRgbLeds()) {

    rgbAvailable = false;

    Serial.println(
      "RGB: initial clear failed");

    return;
  }

  Serial.println(
    "RGB: ready 10 LEDs via RMT");
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

void drawSmall(const char* text, int x, int y, textdatum_t datum, uint16_t color) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  setSmallFont();
  canvas.drawString(text, x, y);
}

void drawMedium(const char* text, int x, int y, textdatum_t datum, uint16_t color) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  setMediumFont();
  canvas.drawString(text, x, y);
}

void drawLarge(const char* text, int x, int y, textdatum_t datum, uint16_t color) {
  canvas.setTextDatum(datum);
  canvas.setTextColor(color);
  setLargeFont();
  canvas.drawString(text, x, y);
}

// =============================================================================
// microSD
// =============================================================================

void ensureDirectory(const char* path) {
  if (!sdAvailable) return;

  if (!SD.exists(path) && !SD.mkdir(path)) {
    Serial.printf("SD: failed to create %s\n", path);
  }
}

bool buildDatedPath(time_t timestamp,
                    const char* directory,
                    char* buffer,
                    size_t size) {
  struct tm localTime;
  localtime_r(&timestamp, &localTime);

  if (localTime.tm_year < 120) return false;

  snprintf(buffer, size, "%s/%04d-%02d-%02d.csv",
           directory,
           localTime.tm_year + 1900,
           localTime.tm_mon + 1,
           localTime.tm_mday);
  return true;
}

bool buildLogPath(time_t timestamp, char* buffer, size_t size) {
  return buildDatedPath(timestamp, "/logs", buffer, size);
}

bool buildSystemLogPath(time_t timestamp, char* buffer, size_t size) {
  return buildDatedPath(timestamp, "/system", buffer, size);
}

void initializeSdCard() {
  Serial.println("SD: initializing");

  if (!SD.begin(App::SD_CS_PIN, SPI, 25000000)) {
    sdAvailable = false;
    Serial.println("SD: unavailable - logging disabled");
    return;
  }

  sdAvailable = true;
  ensureDirectory("/logs");
  ensureDirectory("/system");

  const uint64_t cardSize = SD.cardSize();

  Serial.printf("SD: ready size=%llu MB\n",
                static_cast<unsigned long long>(
                  cardSize / (1024ULL * 1024ULL)));
}

void writeHeaderIfNeeded(const char* path, const char* header) {
  if (!sdAvailable || SD.exists(path)) return;

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    Serial.printf("SD: cannot create %s\n", path);
    return;
  }

  file.println(header);
  file.close();
}

void updateGraphFromLog(time_t timestamp, float temperature, float humidity);

bool appendEnvironmentLog() {
  if (!sdAvailable || !sensorData.valid || !isClockValid()) return false;

  const time_t now = time(nullptr);

  char path[40];
  if (!buildLogPath(now, path, sizeof(path))) return false;

  writeHeaderIfNeeded(path, "timestamp,temperature,humidity,rssi,mqtt");

  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    Serial.printf("SD: open failed %s\n", path);
    return false;
  }

  char timestamp[24];
  formatTimestamp(now, timestamp, sizeof(timestamp));

  const int rssi = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;

  const size_t written = file.printf("%s,%.1f,%.1f,%d,%d\n",
              timestamp,
              sensorData.temperature,
              sensorData.humidity,
              rssi,
              mqttClient.connected() ? 1 : 0);

  const bool saved = written > 0 && file.getWriteError() == 0;
  file.close();
  if (!saved) return false;
  statsCache.dirty = true;
  updateGraphFromLog(now, sensorData.temperature, sensorData.humidity);

  Serial.printf("SD: logged %s %.1f C %.1f %%\n",
                timestamp,
                sensorData.temperature,
                sensorData.humidity);

  return true;
}

bool appendSystemLog() {
  if (!sdAvailable || !isClockValid()) return false;

  const time_t now = time(nullptr);

  char path[40];
  if (!buildSystemLogPath(now, path, sizeof(path))) return false;

  writeHeaderIfNeeded(
    path,
    "timestamp,rssi,wifi,mqtt,heap,min_heap,largest_heap,battery_pct,battery_mv,data_state,data_age_s");

  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    Serial.printf("SD: system log open failed %s\n", path);
    return false;
  }

  char timestamp[24];
  formatTimestamp(now, timestamp, sizeof(timestamp));

  const bool wifiUp = WiFi.status() == WL_CONNECTED;
  const int rssi = wifiUp ? WiFi.RSSI() : 0;
  const int batteryPct = M5.Power.getBatteryLevel();
  const int batteryMv = M5.Power.getBatteryVoltage();

  file.printf("%s,%d,%d,%d,%u,%u,%u,%d,%d,%s,%lu\n",
              timestamp,
              rssi,
              wifiUp ? 1 : 0,
              mqttClient.connected() ? 1 : 0,
              ESP.getFreeHeap(),
              ESP.getMinFreeHeap(),
              heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
              batteryPct,
              batteryMv,
              dataStateText(getDataState()),
              sensorData.valid
                ? static_cast<unsigned long>(sensorAgeMs() / 1000UL)
                : 0UL);

  file.close();
  return true;
}

void maintainSdLogging(uint32_t now) {
  if (!sdAvailable || !isClockValid()) return;

  if (!intervalElapsed(now, lastSdLogMs, App::SD_LOG_INTERVAL_MS)) return;

  Perf::Scope timer(Perf::sdlog);
  lastSdLogMs = now;

  if (sensorData.valid) appendEnvironmentLog();
  appendSystemLog();
}

// =============================================================================
// CSV parsing / graph / statistics
// =============================================================================

bool parseCsvTimestamp(const char* text, time_t& result) {
  int year, month, day, hour, minute, second;

  if (sscanf(text, "%d-%d-%d %d:%d:%d",
             &year, &month, &day, &hour, &minute, &second)
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

bool parseEnvironmentCsvLine(const char* line,
                             time_t& timestamp,
                             float& temperature,
                             float& humidity) {
  char timestampText[24];
  int rssi;
  int mqtt;

  if (sscanf(line, "%23[^,],%f,%f,%d,%d",
             timestampText,
             &temperature,
             &humidity,
             &rssi,
             &mqtt)
      != 5) {
    return false;
  }

  return parseCsvTimestamp(timestampText, timestamp);
}

void clearGraphPoints() {
  for (size_t i = 0; i < App::GRAPH_POINT_COUNT; ++i) {
    graphPoints[i] = GraphPoint{};
  }
}

void processGraphCsvLine(const char* line, time_t windowStart, time_t windowEnd) {
  time_t timestamp;
  float temperature;
  float humidity;

  if (!parseEnvironmentCsvLine(line, timestamp, temperature, humidity)) return;
  if (timestamp < windowStart || timestamp > windowEnd) return;

  const uint32_t offset =
    static_cast<uint32_t>(timestamp - windowStart);

  const size_t bucket =
    offset / App::GRAPH_BUCKET_SECONDS;

  // The exact window-end sample was previously excluded from the 288 points.
  // Retain it for the next window without changing the displayed buckets.
  if (bucket > App::GRAPH_POINT_COUNT) return;
  GraphPoint& point = bucket == App::GRAPH_POINT_COUNT
    ? graphCache.pending : graphPoints[bucket];
  point.valid = true;
  point.timestamp = timestamp;
  point.temperature = temperature;
  point.humidity = humidity;
}

template<typename LineHandler>
void readCsvFileLines(const char* path, LineHandler handler) {
  if (!SD.exists(path)) return;

  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.printf("SD: cannot open %s\n", path);
    if (graphCache.loading) graphCache.readFailed = true;
    return;
  }

  if (Perf::csvFiles) ++*Perf::csvFiles;
  file.readStringUntil('\n');

  char lineBuffer[160];

  while (file.available()) {
    size_t index = 0;

    while (file.available() && index < sizeof(lineBuffer) - 1) {
      const char c = static_cast<char>(file.read());
      if (c == '\n') break;
      if (c != '\r') lineBuffer[index++] = c;
    }

    lineBuffer[index] = '\0';

    if (index > 0) {
      if (Perf::csvLines) ++*Perf::csvLines;
      handler(lineBuffer);
    }
  }

  file.close();
}

void loadGraphFile(const char* path, time_t windowStart, time_t windowEnd) {
  readCsvFileLines(path, [&](const char* line) {
    processGraphCsvLine(line, windowStart, windowEnd);
  });
}

void loadEnvironmentFilesForRange(time_t rangeStart,
                                  time_t rangeEnd,
                                  void (*handler)(const char*, time_t, time_t)) {
  if (!sdAvailable) return;

  // At most three calendar dates are required for a 48 h range.
  time_t cursor = rangeStart;
  char previousPath[40] = "";

  while (cursor <= rangeEnd + 86400) {
    char path[40];

    if (buildLogPath(cursor, path, sizeof(path)) && strcmp(path, previousPath) != 0) {
      handler(path, rangeStart, rangeEnd);
      snprintf(previousPath, sizeof(previousPath), "%s", path);
    }

    cursor += 86400;
  }
}

void invalidateGraphCache() {
  graphCache.valid = false;
  graphCache.dirty = true;
  graphCache.attempted = false;
}

// No SD I/O. Compare wall-clock movement with monotonic elapsed time, then
// advance by complete buckets. Epoch seconds work across local midnight.
bool advanceGraphWindow(time_t now) {
  if (!graphCache.valid) return false;
  const uint32_t ms = millis();
  const int64_t wallSeconds = static_cast<int64_t>(now) - graphCache.checkedAt;
  const uint32_t elapsedSeconds = static_cast<uint32_t>(ms - graphCache.checkedMs) / 1000UL;
  const int64_t clockError = wallSeconds - elapsedSeconds;
  if (wallSeconds < 0 || wallSeconds >= 86400 || clockError < -2 || clockError > 2) {
    invalidateGraphCache();
    return false;
  }
  graphCache.checkedAt = now;
  graphCache.checkedMs = ms;
  const time_t end = graphCache.windowStart + 86400;
  if (now < end) { invalidateGraphCache(); return false; }
  const size_t steps = static_cast<size_t>((now - end) / App::GRAPH_BUCKET_SECONDS);
  if (!steps) return false;
  if (steps >= App::GRAPH_POINT_COUNT) { invalidateGraphCache(); return false; }
  graphCache.windowStart += steps * App::GRAPH_BUCKET_SECONDS;
  for (size_t i = 0; i < App::GRAPH_POINT_COUNT; ++i) {
    graphPoints[i] = i + steps < App::GRAPH_POINT_COUNT
      ? graphPoints[i + steps] : GraphPoint{};
  }
  if (graphCache.pending.valid) {
    const time_t offset = graphCache.pending.timestamp - graphCache.windowStart;
    if (offset >= 0 && offset < 86400) {
      graphPoints[static_cast<size_t>(offset / App::GRAPH_BUCKET_SECONDS)] = graphCache.pending;
    }
  }
  graphCache.pending = GraphPoint{};
  return true;
}

// Called only after the persistent CSV has been written and closed successfully.
void updateGraphFromLog(time_t timestamp, float temperature, float humidity) {
  if (!graphCache.valid) return;
  advanceGraphWindow(timestamp);
  if (!graphCache.valid || timestamp < graphCache.windowStart) return;
  const size_t bucket = static_cast<size_t>((timestamp - graphCache.windowStart) / App::GRAPH_BUCKET_SECONDS);
  if (bucket > App::GRAPH_POINT_COUNT) { invalidateGraphCache(); return; }
  GraphPoint& point = bucket == App::GRAPH_POINT_COUNT
    ? graphCache.pending : graphPoints[bucket];
  // Match the CSV's one-decimal representation, including printf rounding.
  char values[32];
  snprintf(values, sizeof(values), "%.1f,%.1f", temperature, humidity);
  if (sscanf(values, "%f,%f", &temperature, &humidity) != 2) {
    invalidateGraphCache(); return;
  }
  point.valid = true;
  point.timestamp = timestamp;
  point.temperature = temperature;
  point.humidity = humidity;
  ++Perf::graphIncremental;
}

void loadGraphData() {
  Perf::Scope timer(Perf::graph);
  Perf::graphFiles = Perf::graphLines = 0;
  if (!sdAvailable || !isClockValid()) return;

  ++Perf::graphMiss;
  graphCache.attempted = true;
  lastGraphReloadMs = millis();  // Also bounds retries after an SD open failure.
  graphCache.loading = true;
  graphCache.readFailed = false;
  clearGraphPoints();
  graphCache.pending = GraphPoint{};
  const time_t windowEnd = time(nullptr);
  graphCache.windowStart = windowEnd - 86400;

  Perf::csvFiles = &Perf::graphFiles; Perf::csvLines = &Perf::graphLines;
  loadEnvironmentFilesForRange(graphCache.windowStart, windowEnd, loadGraphFile);
  Perf::csvFiles = Perf::csvLines = nullptr;
  graphCache.loading = false;
  graphCache.valid = !graphCache.readFailed;
  graphCache.dirty = !graphCache.valid;
  graphCache.checkedAt = windowEnd;
  graphCache.checkedMs = lastGraphReloadMs;
  if (graphCache.valid) ++Perf::graphRebuild;
  Serial.println(graphCache.valid ? "GRAPH: 24h data loaded" : "GRAPH: rebuild failed");
}

void drawScreen();

// Only this loop service can start a full rebuild; render/callbacks never do.
void maintainGraphCache() {
  const bool graphPage = currentPage == Page::TemperatureGraph || currentPage == Page::HumidityGraph;
  if (!isClockValid()) { if (graphCache.valid) invalidateGraphCache(); return; }
  const bool moved = advanceGraphWindow(time(nullptr));
  if (displaySleeping || !graphPage) return;
  if (!graphCache.valid && sdAvailable &&
      (!graphCache.attempted || intervalElapsed(millis(), lastGraphReloadMs, App::GRAPH_RELOAD_INTERVAL_MS))) {
    loadGraphData();
    drawScreen();
  } else if (moved) {
    drawScreen();
  }
}

void resetMetricStats(MetricStats& stats) {
  stats = MetricStats{};
}

void accumulateMetric(MetricStats& stats, float value) {
  if (!isfinite(value)) return;

  if (!stats.valid) {
    stats.valid = true;
    stats.count = 1;
    stats.average = value;
    stats.minimum = value;
    stats.maximum = value;
    return;
  }

  ++stats.count;
  stats.average += (value - stats.average) / static_cast<float>(stats.count);
  stats.minimum = min(stats.minimum, value);
  stats.maximum = max(stats.maximum, value);
}

void accumulatePrevious(MetricStats& stats, float value) {
  if (!isfinite(value)) return;

  if (!stats.previousValid) {
    stats.previousValid = true;
    stats.previousCount = 1;
    stats.previousAverage = value;
    return;
  }

  ++stats.previousCount;
  stats.previousAverage +=
    (value - stats.previousAverage) / static_cast<float>(stats.previousCount);
}

struct StatsContext {
  MetricStats* temperature;
  MetricStats* humidity;
  time_t previousStart;
  time_t currentStart;
  time_t end;
};

StatsContext statsContext;

void processStatsFile(const char* path, time_t rangeStart, time_t rangeEnd) {
  (void)rangeStart;
  (void)rangeEnd;

  readCsvFileLines(path, [&](const char* line) {
    time_t timestamp;
    float temperature;
    float humidity;

    if (!parseEnvironmentCsvLine(line, timestamp, temperature, humidity)) return;

    if (timestamp >= statsContext.currentStart && timestamp <= statsContext.end) {
      accumulateMetric(*statsContext.temperature, temperature);
      accumulateMetric(*statsContext.humidity, humidity);
    } else if (timestamp >= statsContext.previousStart && timestamp < statsContext.currentStart) {
      accumulatePrevious(*statsContext.temperature, temperature);
      accumulatePrevious(*statsContext.humidity, humidity);
    }
  });
}

void calculateStats(MetricStats& temperatureStats,
                    MetricStats& humidityStats) {
  Perf::Scope timer(Perf::stats);
  Perf::statsFiles = Perf::statsLines = 0;
  resetMetricStats(temperatureStats);
  resetMetricStats(humidityStats);

  if (sensorData.valid) {
    temperatureStats.current = sensorData.temperature;
    humidityStats.current = sensorData.humidity;
  }

  if (!sdAvailable || !isClockValid()) return;

  const time_t end = time(nullptr);
  const time_t currentStart = end - 24UL * 60UL * 60UL;
  const time_t previousStart = end - 48UL * 60UL * 60UL;

  statsContext.temperature = &temperatureStats;
  statsContext.humidity = &humidityStats;
  statsContext.previousStart = previousStart;
  statsContext.currentStart = currentStart;
  statsContext.end = end;

  Perf::csvFiles = &Perf::statsFiles; Perf::csvLines = &Perf::statsLines;
  loadEnvironmentFilesForRange(previousStart, end, processStatsFile);
  Perf::csvFiles = Perf::csvLines = nullptr;
}

// =============================================================================
// IMU
// =============================================================================

void initializeImu() {
  M5.Imu.update();

  float ax, ay, az;

  if (M5.Imu.getAccelData(&ax, &ay, &az)) {
    imuAvailable = true;
    previousAccelMagnitude = sqrtf(ax * ax + ay * ay + az * az);
    Serial.printf("IMU: ready accel=%.3f g\n", previousAccelMagnitude);
  } else {
    imuAvailable = false;
    Serial.println("IMU: unavailable - motion features disabled");
  }
}

void wakeDisplay();

void performTiltAction(TiltDirection direction,
                       float ax,
                       float ay,
                       float az) {
  if (direction == TiltDirection::None) return;

  if (direction == TiltDirection::Left) {
    setCurrentPage(Page::TemperatureGraph);
    Serial.printf(
      "IMU: tilt ax=%.2f ay=%.2f az=%.2f -> LEFT -> TEMP / 24H\n",
      ax, ay, az);
  } else {
    setCurrentPage(Page::HumidityGraph);
    Serial.printf(
      "IMU: tilt ax=%.2f ay=%.2f az=%.2f -> RIGHT -> HUM / 24H\n",
      ax, ay, az);
  }

  lastUserActivityMs = millis();
}

void maintainImu(uint32_t now) {
  if (!imuAvailable) return;

  if (!intervalElapsed(now, lastImuSampleMs, App::IMU_SAMPLE_INTERVAL_MS)) {
    return;
  }

  lastImuSampleMs = now;

  M5.Imu.update();

  float ax, ay, az;

  if (!M5.Imu.getAccelData(&ax, &ay, &az)) return;

  const float magnitude = sqrtf(ax * ax + ay * ay + az * az);

  // Motion wake while LCD is sleeping.
  if (displaySleeping) {
    if (!isfinite(previousAccelMagnitude)) {
      previousAccelMagnitude = magnitude;
      return;
    }

    const float delta = fabsf(magnitude - previousAccelMagnitude);
    previousAccelMagnitude = magnitude;

    if (!intervalElapsed(now, lastImuWakeMs, App::IMU_WAKE_COOLDOWN_MS)) return;
    if (delta < App::IMU_WAKE_DELTA_G) return;

    lastImuWakeMs = now;

    Serial.printf("IMU: motion wake delta=%.3f g\n", delta);
    wakeDisplay();

    // Do not interpret the same movement as a tilt gesture.
    pendingTilt = TiltDirection::None;
    tiltStartedMs = 0;
    tiltLatched = true;
    lastTiltActionMs = now;
    return;
  }

  previousAccelMagnitude = magnitude;

  // For rotation=1, the physical left/right direction may map to either
  // accelerometer X polarity depending on assembly/orientation.
  float horizontal = ax;
  if (App::IMU_REVERSE_LEFT_RIGHT) horizontal = -horizontal;

  if (fabsf(horizontal) <= App::IMU_TILT_RELEASE_G) {
    pendingTilt = TiltDirection::None;
    tiltStartedMs = 0;
    tiltLatched = false;
    return;
  }

  if (tiltLatched) return;

  if (!intervalElapsed(now, lastTiltActionMs, App::IMU_TILT_COOLDOWN_MS)) {
    return;
  }

  TiltDirection detected = TiltDirection::None;

  if (horizontal <= -App::IMU_TILT_THRESHOLD_G) {
    detected = TiltDirection::Left;
  } else if (horizontal >= App::IMU_TILT_THRESHOLD_G) {
    detected = TiltDirection::Right;
  } else {
    pendingTilt = TiltDirection::None;
    tiltStartedMs = 0;
    return;
  }

  if (detected != pendingTilt) {
    pendingTilt = detected;
    tiltStartedMs = now;
    return;
  }

  if (!intervalElapsed(now, tiltStartedMs, App::IMU_TILT_HOLD_MS)) return;

  performTiltAction(detected, ax, ay, az);

  lastTiltActionMs = now;
  tiltLatched = true;
  pendingTilt = TiltDirection::None;
  tiltStartedMs = 0;
}

// =============================================================================
// Graph drawing
// =============================================================================

void findGraphRange(bool temperature,
                    float& minimum,
                    float& maximum,
                    bool& hasData) {
  minimum = 100000.0f;
  maximum = -100000.0f;
  hasData = false;
  if (!graphCache.valid) return;

  for (size_t i = 0; i < App::GRAPH_POINT_COUNT; ++i) {
    if (!graphPoints[i].valid) continue;

    const float value =
      temperature ? graphPoints[i].temperature : graphPoints[i].humidity;

    if (!isfinite(value)) continue;

    minimum = min(minimum, value);
    maximum = max(maximum, value);
    hasData = true;
  }

  if (!hasData) return;

  float margin = (maximum - minimum) * 0.15f;
  if (margin < 0.5f) margin = 0.5f;

  minimum -= margin;
  maximum += margin;
}

void drawGraph(bool temperature) {
  if (graphCache.valid) ++Perf::graphHit;
  constexpr int LEFT = 42;
  constexpr int RIGHT = 306;
  constexpr int TOP = 58;
  constexpr int BOTTOM = 184;

  float minimum, maximum;
  bool hasData;

  findGraphRange(temperature, minimum, maximum, hasData);

  if (!hasData) {
    drawMedium(sdAvailable ? "NO 24H DATA" : "SD UNAVAILABLE",
               160, 118, middle_center, Color::MUTED);
    return;
  }

  for (int i = 0; i <= 4; ++i) {
    const int y = TOP + ((BOTTOM - TOP) * i / 4);

    canvas.drawFastHLine(LEFT, y, RIGHT - LEFT, Color::DIVIDER);

    const float value =
      maximum - (maximum - minimum) * static_cast<float>(i) / 4.0f;

    char label[16];

    if (temperature) {
      snprintf(label, sizeof(label), "%.1f", value);
    } else {
      snprintf(label, sizeof(label), "%.0f", value);
    }

    drawSmall(label, LEFT - 5, y, middle_right, Color::MUTED);
  }

  canvas.drawFastVLine(LEFT, TOP, BOTTOM - TOP, Color::DIVIDER);
  canvas.drawFastHLine(LEFT, BOTTOM, RIGHT - LEFT, Color::DIVIDER);

  drawSmall("-24h", LEFT, BOTTOM + 13, top_left, Color::MUTED);
  drawSmall("-12h", (LEFT + RIGHT) / 2, BOTTOM + 13, top_center, Color::MUTED);
  drawSmall("now", RIGHT, BOTTOM + 13, top_right, Color::MUTED);

  bool previousValid = false;
  int previousX = 0;
  int previousY = 0;

  const uint16_t graphColor =
    temperature ? Color::ORANGE : Color::CYAN;

  for (size_t i = 0; i < App::GRAPH_POINT_COUNT; ++i) {
    if (!graphPoints[i].valid) {
      previousValid = false;
      continue;
    }

    const float value =
      temperature ? graphPoints[i].temperature : graphPoints[i].humidity;

    if (!isfinite(value)) {
      previousValid = false;
      continue;
    }

    const int x =
      LEFT + static_cast<int>((RIGHT - LEFT) * static_cast<float>(i) / static_cast<float>(App::GRAPH_POINT_COUNT - 1));

    float normalized = (value - minimum) / (maximum - minimum);
    normalized = constrain(normalized, 0.0f, 1.0f);

    const int y =
      BOTTOM - static_cast<int>(normalized * (BOTTOM - TOP));

    if (previousValid) {
      canvas.drawLine(previousX, previousY, x, y, graphColor);
    }

    previousX = x;
    previousY = y;
    previousValid = true;
  }
}

// =============================================================================
// Statistics drawing
// =============================================================================

void drawStatsPage(bool temperature) {
  if (statsCache.valid) ++Perf::cacheHit;
  MetricStats stats = temperature ? statsCache.temperature : statsCache.humidity;
  stats.current = sensorData.valid
    ? (temperature ? sensorData.temperature : sensorData.humidity) : NAN;
  const uint16_t valueColor = temperature ? Color::ORANGE : Color::CYAN;
  const char* unit = temperature ? "C" : "%";

  constexpr int LABEL_X = 24;
  constexpr int VALUE_X = 298;
  constexpr int ROWS[] = { 62, 88, 114, 140, 166, 190 };

  char value[32];

  drawSmall("NOW", LABEL_X, ROWS[0], middle_left, Color::MUTED);
  if (isfinite(stats.current)) {
    snprintf(value, sizeof(value),
             temperature ? "%.1f %s" : "%.0f %s",
             stats.current, unit);
  } else {
    snprintf(value, sizeof(value), "--");
  }
  drawMedium(value, VALUE_X, ROWS[0], middle_right, valueColor);

  drawSmall("24H AVG", LABEL_X, ROWS[1], middle_left, Color::MUTED);
  if (stats.valid) {
    snprintf(value, sizeof(value),
             temperature ? "%.1f %s" : "%.0f %s",
             stats.average, unit);
  } else {
    snprintf(value, sizeof(value), "--");
  }
  drawSmall(value, VALUE_X, ROWS[1], middle_right, Color::TEXT);

  drawSmall("24H MIN", LABEL_X, ROWS[2], middle_left, Color::MUTED);
  if (stats.valid) {
    snprintf(value, sizeof(value),
             temperature ? "%.1f %s" : "%.0f %s",
             stats.minimum, unit);
  } else {
    snprintf(value, sizeof(value), "--");
  }
  drawSmall(value, VALUE_X, ROWS[2], middle_right, Color::TEXT);

  drawSmall("24H MAX", LABEL_X, ROWS[3], middle_left, Color::MUTED);
  if (stats.valid) {
    snprintf(value, sizeof(value),
             temperature ? "%.1f %s" : "%.0f %s",
             stats.maximum, unit);
  } else {
    snprintf(value, sizeof(value), "--");
  }
  drawSmall(value, VALUE_X, ROWS[3], middle_right, Color::TEXT);

  drawSmall("PREV AVG", LABEL_X, ROWS[4], middle_left, Color::MUTED);
  if (stats.previousValid) {
    snprintf(value, sizeof(value),
             temperature ? "%.1f %s" : "%.0f %s",
             stats.previousAverage, unit);
  } else {
    snprintf(value, sizeof(value), "--");
  }
  drawSmall(value, VALUE_X, ROWS[4], middle_right, Color::TEXT);

  drawSmall("vs PREV", LABEL_X, ROWS[5], middle_left, Color::MUTED);
  if (stats.valid && stats.previousValid) {
    const float delta = stats.average - stats.previousAverage;
    snprintf(value, sizeof(value),
             temperature ? "%+.1f %s" : "%+.0f %s",
             delta, unit);
    drawSmall(value, VALUE_X, ROWS[5], middle_right,
              delta > 0.0f ? Color::ORANGE : delta < 0.0f ? Color::CYAN
                                                          : Color::TEXT);
  } else {
    drawSmall("--", VALUE_X, ROWS[5], middle_right, Color::MUTED);
  }
}

// =============================================================================
// Display
// =============================================================================

const char* pageTitle() {
  switch (currentPage) {
    case Page::Main: return "ENV / B3D8";
    case Page::Status: return "SYSTEM STATUS";
    case Page::TemperatureGraph: return "TEMP / 24H";
    case Page::HumidityGraph: return "HUM / 24H";
    case Page::TemperatureStats: return "TEMP / STATS";
    case Page::HumidityStats: return "HUM / STATS";
    default: return "ENV";
  }
}

void drawHeader() {
  char date[8];
  char clock[8];
  char dateTime[20];

  formatDate(date, sizeof(date));
  formatClock(clock, sizeof(clock));

  snprintf(dateTime, sizeof(dateTime), "%s %s", date, clock);

  drawSmall(pageTitle(), 14, 21, middle_left, Color::CYAN);
  drawSmall(dateTime, 306, 21, middle_right, Color::TEXT);

  canvas.drawFastHLine(14, 39, 292, Color::DIVIDER);
}

void drawMainPage() {
  constexpr int LEFT_CENTER_X = 82;
  constexpr int RIGHT_CENTER_X = 238;

  canvas.drawFastVLine(160, 52, 135, Color::DIVIDER);

  drawSmall("TEMPERATURE", LEFT_CENTER_X, 61, middle_center, Color::MUTED);
  drawSmall("HUMIDITY", RIGHT_CENTER_X, 61, middle_center, Color::MUTED);

  char value[16];

  if (sensorData.valid && !isnan(sensorData.temperature)) {
    snprintf(value, sizeof(value), "%.1f", sensorData.temperature);
  } else {
    snprintf(value, sizeof(value), "--.-");
  }

  drawLarge(value, LEFT_CENTER_X, 119, middle_center, Color::ORANGE);

  if (sensorData.valid && !isnan(sensorData.humidity)) {
    snprintf(value, sizeof(value), "%.0f", sensorData.humidity);
  } else {
    snprintf(value, sizeof(value), "--");
  }

  drawLarge(value, RIGHT_CENTER_X, 119, middle_center, Color::CYAN);

  canvas.drawCircle(LEFT_CENTER_X - 11, 158, 3, Color::ORANGE);
  drawMedium("C", LEFT_CENTER_X + 4, 171, middle_center, Color::ORANGE);
  drawMedium("%", RIGHT_CENTER_X, 171, middle_center, Color::CYAN);
}

void drawStatusPage() {
  constexpr int LABEL_X = 18;
  constexpr int VALUE_X = 302;
  constexpr int ROW_Y[] = { 54, 74, 94, 114, 134, 154, 174, 194 };

  char value[32];

  drawSmall("Wi-Fi", LABEL_X, ROW_Y[0], middle_left, Color::MUTED);

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(value, sizeof(value), "%d dBm", WiFi.RSSI());
    drawSmall(value, VALUE_X, ROW_Y[0], middle_right, Color::GREEN);
  } else {
    drawSmall("OFFLINE", VALUE_X, ROW_Y[0], middle_right, Color::RED);
  }

  drawSmall("MQTT", LABEL_X, ROW_Y[1], middle_left, Color::MUTED);
  drawSmall(mqttClient.connected() ? "CONNECTED" : "OFFLINE",
            VALUE_X, ROW_Y[1], middle_right,
            mqttClient.connected() ? Color::GREEN : Color::RED);

  drawSmall("Data age", LABEL_X, ROW_Y[2], middle_left, Color::MUTED);
  formatDataAge(value, sizeof(value));
  drawSmall(value, VALUE_X, ROW_Y[2], middle_right,
            dataStateColor(getDataState()));

  drawSmall("Environment", LABEL_X, ROW_Y[3], middle_left, Color::MUTED);
  const EnvironmentState envState = getEnvironmentState();
  drawSmall(environmentStateText(envState),
            VALUE_X, ROW_Y[3], middle_right,
            envState == EnvironmentState::Normal ? Color::GREEN : envState == EnvironmentState::Warning ? Color::YELLOW
                                                                                                        : Color::RED);

  drawSmall("SD", LABEL_X, ROW_Y[4], middle_left, Color::MUTED);
  drawSmall(sdAvailable ? "READY" : "OFFLINE",
            VALUE_X, ROW_Y[4], middle_right,
            sdAvailable ? Color::GREEN : Color::RED);

  drawSmall("IMU", LABEL_X, ROW_Y[5], middle_left, Color::MUTED);
  drawSmall(imuAvailable ? "READY" : "OFFLINE",
            VALUE_X, ROW_Y[5], middle_right,
            imuAvailable ? Color::GREEN : Color::RED);

  drawSmall("Battery", LABEL_X, ROW_Y[6], middle_left, Color::MUTED);
  snprintf(value, sizeof(value), "%d%%",
           M5.Power.getBatteryLevel());
  drawSmall(value, VALUE_X, ROW_Y[6], middle_right, Color::TEXT);

  drawSmall("Uptime", LABEL_X, ROW_Y[7], middle_left, Color::MUTED);
  formatUptime(value, sizeof(value));
  drawSmall(value, VALUE_X, ROW_Y[7], middle_right, Color::TEXT);
}

void drawFooter() {
  canvas.drawFastHLine(14, 207, 292, Color::DIVIDER);

  canvas.fillCircle(18, 226, 4,
                    mqttClient.connected() ? Color::GREEN : Color::RED);

  drawSmall("MQTT", 28, 226, middle_left, Color::MUTED);

  if (currentPage == Page::Main) {
    const DataState state = getDataState();

    drawSmall(dataStateText(state),
              83, 226, middle_left, dataStateColor(state));

    char age[24];
    formatDataAge(age, sizeof(age));

    drawSmall(age, 306, 226, middle_right, Color::MUTED);
    return;
  }

  char version[16];
  snprintf(version, sizeof(version), "v%s", App::VERSION);

  drawSmall(version, 306, 226, middle_right, Color::MUTED);
}

void drawScreen() {
  Perf::Scope timer(Perf::draw);
  if (displaySleeping) return;

  canvas.fillSprite(Color::BG);
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
    case Page::TemperatureStats:
      drawStatsPage(true);
      break;
    case Page::HumidityStats:
      drawStatsPage(false);
      break;
  }

  drawFooter();
  { Perf::Scope transfer(Perf::push);
    canvas.pushSprite(0, 0);
  }
  lastDisplayRefreshMs = millis();
}

// Deliberately outside drawScreen/MQTT callback. One scan produces both metrics.
void maintainStatsCache() {
  if (displaySleeping ||
      (currentPage != Page::TemperatureStats && currentPage != Page::HumidityStats)) return;
  if (!sdAvailable || !isClockValid()) return;
  const time_t now = time(nullptr);
  // Bound rolling-window staleness even when no new sample can be logged.
  if (statsCache.valid &&
      (now < statsCache.calculatedAt || now - statsCache.calculatedAt >= 60)) {
    statsCache.dirty = true;
  }
  if (statsCache.valid && !statsCache.dirty) return;
  ++Perf::cacheMiss;
  calculateStats(statsCache.temperature, statsCache.humidity);
  statsCache.calculatedAt = now;
  statsCache.valid = true;
  statsCache.dirty = false;
  ++Perf::cacheRefresh;
  drawScreen();
}

void refreshDisplayIfDue(uint32_t now) {
  if (displaySleeping) return;

  if (!intervalElapsed(now, lastDisplayRefreshMs, App::DISPLAY_REFRESH_MS)) {
    return;
  }

  drawScreen();
}

// =============================================================================
// Display power / brightness
// =============================================================================

void registerUserActivity() {
  lastUserActivityMs = millis();
}

void sleepDisplay() {
  if (displaySleeping) return;

  displaySleeping = true;
  M5.Display.setBrightness(0);

  previousAccelMagnitude = NAN;
  pendingTilt = TiltDirection::None;
  tiltStartedMs = 0;

  Serial.println("Display: sleep");
}

void wakeDisplay() {
  displaySleeping = false;
  registerUserActivity();

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);

  drawScreen();

  Serial.println("Display: wake");
}

void loadUiState() {
  // Defaults also cover an unavailable namespace or missing/wrong-type keys.
  currentPage = Page::Main;
  brightnessIndex = App::DEFAULT_BRIGHTNESS_INDEX;
  displaySleeping = false;  // Always show successful startup; OFF is not saved.

  if (!preferencesAvailable) return;

  const uint8_t savedPage = preferences.getUChar("page", 0);
  if (savedPage <= static_cast<uint8_t>(Page::HumidityStats)) {
    currentPage = static_cast<Page>(savedPage);
  }

  const uint8_t savedBrightness =
    preferences.getUChar("brightness", App::DEFAULT_BRIGHTNESS_INDEX);
  if (savedBrightness < App::BRIGHTNESS_LEVEL_COUNT) {
    brightnessIndex = savedBrightness;
  }
}

void cycleBrightness() {
  const uint8_t nextIndex =
    (brightnessIndex + 1) % App::BRIGHTNESS_LEVEL_COUNT;
  if (nextIndex == brightnessIndex) return;
  brightnessIndex = nextIndex;

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);

  if (preferencesAvailable &&
      preferences.putUChar("brightness", brightnessIndex) != 1) {
    Serial.println("NVS: brightness save failed");
  }

  Serial.printf("Display: brightness=%u\n",
                App::BRIGHTNESS_LEVELS[brightnessIndex]);
}

void maintainDisplaySleep(uint32_t now) {
  if (displaySleeping) return;

  if (intervalElapsed(now, lastUserActivityMs, App::DISPLAY_SLEEP_MS)) {
    sleepDisplay();
  }
}

// =============================================================================
// Retry helpers
// =============================================================================

void resetRetry(RetryState& retry, uint32_t minimumDelayMs) {
  retry.delayMs = minimumDelayMs;
  retry.nextAttemptMs = 0;
}

void scheduleRetry(RetryState& retry,
                   uint32_t now,
                   uint32_t maximumDelayMs) {
  retry.nextAttemptMs = now + retry.delayMs;

  if (retry.delayMs < maximumDelayMs) {
    retry.delayMs = min(retry.delayMs * 2UL, maximumDelayMs);
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
  WiFi.setSleep(false);

  startWiFiConnection();

  wifiRetry.nextAttemptMs =
    millis() + App::WIFI_CONNECT_TIMEOUT_MS;
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
        ip[0], ip[1], ip[2], ip[3], WiFi.RSSI());

      resetRetry(wifiRetry, App::WIFI_RETRY_MIN_MS);
      return;
    }

    Serial.printf("WiFi: disconnected status=%d\n", status);
  }

  if (status == WL_CONNECTED) return;

  if (!deadlineReached(now, wifiRetry.nextAttemptMs)) return;

  Serial.printf("WiFi: reconnecting, retry delay=%lu ms\n",
                static_cast<unsigned long>(wifiRetry.delayMs));

  startWiFiConnection();
  scheduleRetry(wifiRetry, now, App::WIFI_RETRY_MAX_MS);
}

// =============================================================================
// NTP
// =============================================================================

void configureTimeIfNeeded() {
  if (timeConfigured || WiFi.status() != WL_CONNECTED) return;

  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);

  timeConfigured = true;
  Serial.println("NTP: configured");
}

void maintainTime() {
  if (!timeConfigured) configureTimeIfNeeded();

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

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_ENV) != 0) return;

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

  Serial.printf("ENV: %.1f C %.1f %%\n",
                sensorData.temperature,
                sensorData.humidity);

  updateEnvironmentLeds();

  if (!displaySleeping) {
    drawScreen();
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
  Perf::Scope timer(Perf::mqttConnect);
  Serial.printf("MQTT: connecting to %s:%u\n", MQTT_HOST, MQTT_PORT);

  bool connected = false;

  if (strlen(MQTT_USERNAME) > 0) {
    connected =
      mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    connected = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (!connected) {
    Serial.printf("MQTT: connect failed state=%d\n", mqttClient.state());
    return false;
  }

  if (!mqttClient.subscribe(MQTT_TOPIC_ENV, 0)) {
    Serial.println("MQTT: subscribe failed");
    mqttClient.disconnect();
    return false;
  }

  Serial.printf("MQTT: connected and subscribed %s\n", MQTT_TOPIC_ENV);
  return true;
}

void maintainMQTT(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (mqttClient.connected()) mqttClient.disconnect();
    return;
  }

  if (mqttClient.connected()) {
    Perf::Scope timer(Perf::mqttLoop);
    mqttClient.loop();
    return;
  }

  if (!deadlineReached(now, mqttRetry.nextAttemptMs)) return;

  if (connectMQTT()) {
    resetRetry(mqttRetry, App::MQTT_RETRY_MIN_MS);
    return;
  }

  scheduleRetry(mqttRetry, now, App::MQTT_RETRY_MAX_MS);
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
  const uint32_t perfStart = micros();
  Serial.printf("PERF B handler t=%lu\n", (unsigned long)millis());
  if (displaySleeping) {
    Serial.println("PERF B wake_only");
    wakeDisplay();
    Serial.printf("PERF B render_done elapsed_us=%lu\n", (unsigned long)(micros()-perfStart));
    return;
  }

  registerUserActivity();

  Perf::bChanging = true;
  switch (currentPage) {
    case Page::Main:
      setCurrentPage(Page::Status);
      break;

    case Page::Status:
      setCurrentPage(Page::TemperatureGraph);
      break;

    case Page::TemperatureGraph:
      setCurrentPage(Page::HumidityGraph);
      break;

    case Page::HumidityGraph:
      setCurrentPage(Page::TemperatureStats);
      break;

    case Page::TemperatureStats:
      setCurrentPage(Page::HumidityStats);
      break;

    case Page::HumidityStats:
    default:
      setCurrentPage(Page::Main);
      break;
  }

  drawScreen();
  Perf::bChanging = false;
  Serial.printf("PERF B render_done elapsed_us=%lu\n", (unsigned long)(micros()-perfStart));
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
  setCurrentPage(Page::Main);
  drawScreen();
}

void handleButtonCLong() {
  if (displaySleeping) {
    wakeDisplay();
    return;
  }

  registerUserActivity();
  publishStudyLightToggle();
  drawScreen();
}

void processButton(ButtonState& state,
                   bool pressed,
                   uint32_t now,
                   void (*shortPressHandler)(),
                   void (*longPressHandler)()) {
  if (pressed && !state.wasPressed) {
    if (&state == &buttonB) Serial.printf("PERF B press t=%lu page=%s\n", (unsigned long)millis(), Perf::pageName(currentPage));
    state.pressedAtMs = now;
    state.longPressHandled = false;
  }

  if (pressed && !state.longPressHandled && intervalElapsed(now, state.pressedAtMs, App::LONG_PRESS_MS)) {
    state.longPressHandled = true;
    if (&state == &buttonB) {
      Serial.printf("PERF B long t=%lu\n", (unsigned long)millis());
      if (displaySleeping) Serial.println("PERF B wake_only");
    }
    longPressHandler();
  }

  if (!pressed && state.wasPressed && &state == &buttonB) {
    Serial.printf("PERF B release t=%lu held_ms=%lu\n", (unsigned long)millis(), (unsigned long)(now-state.pressedAtMs));
  }
  if (!pressed && state.wasPressed && !state.longPressHandled) {
    if (&state == &buttonB) Serial.printf("PERF B short t=%lu\n", (unsigned long)millis());
    shortPressHandler();
  }

  state.wasPressed = pressed;
}

void processButtons(uint32_t now) {
  processButton(buttonA, M5.BtnA.isPressed(), now,
                handleButtonAShort, handleButtonALong);

  processButton(buttonB, M5.BtnB.isPressed(), now,
                handleButtonBShort, handleButtonBLong);

  processButton(buttonC, M5.BtnC.isPressed(), now,
                handleButtonCShort, handleButtonCLong);
}

// =============================================================================
// Diagnostics
// =============================================================================

const char* resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXTERNAL";
    case ESP_RST_SW: return "SOFTWARE";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_UNKNOWN:
    default: return "UNKNOWN";
  }
}

void logBootDiagnostics() {
  const esp_reset_reason_t reason = esp_reset_reason();

  Serial.printf("Boot: reset_reason=%s (%d)\n",
                resetReasonText(reason),
                static_cast<int>(reason));

  Serial.printf("Heap: free=%u largest=%u min_free=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                ESP.getMinFreeHeap());
}

void logHealthIfDue(uint32_t now) {
  if (!intervalElapsed(now, lastHealthLogMs,
                       App::HEALTH_LOG_INTERVAL_MS)) {
    return;
  }

  lastHealthLogMs = now;

  char uptime[24];
  formatUptime(uptime, sizeof(uptime));

  Serial.printf(
    "HEALTH: uptime=%s wifi=%s mqtt=%s rssi=%d "
    "heap=%u min_heap=%u largest=%u data=%s age_s=%lu "
    "sd=%s imu=%s battery=%d%% env=%s\n",
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
      : 0UL,
    sdAvailable ? "ready" : "offline",
    imuAvailable ? "ready" : "offline",
    M5.Power.getBatteryLevel(),
    environmentStateText(getEnvironmentState()));
}

// =============================================================================
// Setup
// =============================================================================

void setupDisplay() {
  M5.Display.setRotation(1);

  preferencesAvailable = preferences.begin("envmonitor", false);
  if (!preferencesAvailable) {
    Serial.println("NVS: unavailable - UI persistence disabled");
  }
  loadUiState();

  M5.Display.setBrightness(
    App::BRIGHTNESS_LEVELS[brightnessIndex]);

  Serial.printf("Font: %s\n",
                HAVE_JETBRAINS_MONO
                  ? "JetBrains Mono embedded"
                  : "FreeMono fallback");

  Serial.printf("Heap before canvas: free=%u largest=%u\n",
                ESP.getFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  canvas.setColorDepth(8);

  if (canvas.createSprite(App::SCREEN_WIDTH, App::SCREEN_HEIGHT) == nullptr) {
    Serial.println("FATAL: canvas allocation failed");

    while (true) {
      delay(1000);
    }
  }

  canvas.setTextWrap(false);

  lastUserActivityMs = millis();
  lastDisplayRefreshMs = millis();
  // First page draw is deferred until SD and the other peripherals are ready.
}

// =============================================================================
// Arduino entry points
// =============================================================================

void setup() {
  auto config = M5.config();

  config.internal_imu = true;
  config.internal_spk = true;

  M5.begin(config);

  Serial.begin(115200);
  delay(100);

  Serial.printf("\n%s v%s\n", App::NAME, App::VERSION);

  logBootDiagnostics();

  setupDisplay();
  initializeImu();
  initializeRgbLeds();
  initializeSdCard();
  initializePortAUnits();
  configureMQTT();
  configureWiFi();

  // History caches are prepared by loop services once SD/clock are ready;
  // drawing itself never starts a CSV scan.
  drawScreen();
  lastUserActivityMs = millis();
  lastDisplayRefreshMs = millis();
}

void loop() {
  const uint32_t perfLoopStart = micros();
  const uint32_t now = millis();

  const uint32_t perfUpdate = micros();
  if (Perf::haveUpdate) Perf::gap.add(static_cast<uint32_t>(perfUpdate - Perf::lastUpdate));
  Perf::lastUpdate = perfUpdate; Perf::haveUpdate = true;
  M5.update();

  processButtons(now);
  maintainPortAUnits();

  maintainWiFi(now);
  maintainTime();
  maintainMQTT(now);
  maintainSdLogging(now);
  maintainLightFeedback(millis());
  maintainLightSound();

  maintainDisplaySleep(millis());
  maintainImu(now);

  maintainGraphCache();
  maintainStatsCache();
  refreshDisplayIfDue(millis());
  logHealthIfDue(now);

  delay(2);
  Perf::loop.add(static_cast<uint32_t>(micros()-perfLoopStart));
  Perf::summary();  // Excluded from loop time, included in the real update gap.
}
