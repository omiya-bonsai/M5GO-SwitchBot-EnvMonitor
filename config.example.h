#pragma once

// ============================================================================
// Wi-Fi
// ============================================================================

#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ============================================================================
// MQTT
// ============================================================================

#define MQTT_HOST "192.168.0.10"
#define MQTT_PORT 1883

#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

#define MQTT_CLIENT_ID "m5go-switchbot-b3d8"

#define MQTT_TOPIC_ENV "home/env/switchbot-b3d8/raw"

// ============================================================================
// Time / NTP
// ============================================================================

#define TZ_INFO "JST-9"

#define NTP_SERVER_1 "ntp.nict.jp"
#define NTP_SERVER_2 "ntp.jst.mfeed.ad.jp"
#define NTP_SERVER_3 "pool.ntp.org"