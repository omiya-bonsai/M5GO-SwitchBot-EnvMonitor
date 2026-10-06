#pragma once
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MQTT_HOST "192.168.3.200"
#define MQTT_PORT 1883
#define MQTT_USERNAME "mqtt"
#define MQTT_PASSWORD "YOUR_MQTT_PASSWORD"
#define MQTT_CLIENT_ID "m5go-switchbot-b3d8"
#define MQTT_TOPIC_ENV "home/env/switchbot-b3d8/raw"
#define TZ_INFO "JST-9"
#define NTP_SERVER_1 "ntp.nict.jp"
#define NTP_SERVER_2 "ntp.jst.mfeed.ad.jp"
#define NTP_SERVER_3 "pool.ntp.org"

// Display: occupancy keep-on, automatic lux brightness, inactivity seconds (0: no auto-sleep).
#define DISPLAY_KEEP_ON_WHEN_OCCUPIED 1
#define DISPLAY_AUTO_BRIGHTNESS 1
#define DISPLAY_SLEEP_TIMEOUT_SEC 180
