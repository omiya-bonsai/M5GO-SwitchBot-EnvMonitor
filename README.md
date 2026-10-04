**English** \| [日本語](README-ja.md)

# M5GO SwitchBot EnvMonitor

An Arduino project that turns an M5Stack M5GO v2.7 into an
always-available environmental monitor for a SwitchBot
temperature/humidity sensor.

Home Assistant receives the SwitchBot measurements and publishes them
through MQTT (Mosquitto) to the M5GO. The device provides a two-column
MAIN screen, a diagnostic STATUS screen, physical button controls, and
automatic display backlight shutoff after three minutes of inactivity.

## Architecture

``` text
SwitchBot temperature/humidity sensor
        │
        │ Bluetooth / SwitchBot integration
        ▼
Home Assistant
        │
        │ mqtt.publish
        ▼
Mosquitto
        │
        │ home/env/switchbot-b3d8/raw
        ▼
M5GO v2.7
```

Example MQTT payload:

``` json
{
  "id": "switchbot-b3d8",
  "temperature": 24.9,
  "humidity": 62.0
}
```

A one-minute heartbeat publish from Home Assistant is recommended in
addition to state-change publishing. This allows the M5GO to monitor the
health of the MQTT delivery path even when the measured values remain
unchanged.

## Features

-   Receives SwitchBot temperature and humidity over MQTT
-   320 × 240 two-column MAIN display
-   STATUS page showing Wi-Fi, MQTT, data age, IP address, and uptime
-   Flicker-reduced rendering using an 8-bit `M5Canvas`
-   Automatic display backlight shutoff after three minutes without
    button input
-   Wi-Fi, MQTT, and NTP continue running while the display is off
-   Incoming MQTT messages do not wake the display
-   Five brightness levels
-   Brightness is persisted in NVS using `Preferences`
-   JetBrains Mono support
-   FreeMono fallback when JetBrains Mono is unavailable
-   JST clock synchronized by NTP
-   LIVE / STALE / OFFLINE MQTT-data status

## Button Controls

  Button   Short press            Long press
  -------- ---------------------- ------------------------
  A        Display ON / OFF       Cycle brightness
  B        Toggle MAIN / STATUS   Reserved
  C        Return to MAIN         Reconnect Wi-Fi / MQTT

When the display is off, the first press of **any** button only wakes
the display. It does not execute the button's normal action. The latest
received values are rendered immediately after wake-up.

Brightness cycles through:

``` text
40 → 80 → 120 → 160 → 220 → 40 ...
```

## Automatic Display Shutoff

The backlight is switched off after 180 seconds without physical button
activity.

``` text
No button input for 3 minutes
        ↓
Display brightness = 0
        ↓
Wi-Fi / MQTT / NTP remain active
```

This is not an ESP32 sleep mode. MQTT remains connected and the display
can be restored immediately with a button press.

Because the M5GO v2.7 uses a TFT LCD, the project turns the backlight
off instead of keeping a moving screensaver active.

## Screens

### MAIN

The default screen emphasizes the current temperature and humidity.

``` text
┌──────────────────────────────────────┐
│ ENV / B3D8               10/05 06:10│
│ ──────────────────────────────────── │
│ TEMPERATURE       │     HUMIDITY     │
│                   │                  │
│     24.9          │        62        │
│       C           │         %        │
│                   │                  │
│ ──────────────────────────────────── │
│ ● MQTT  LIVE                 28s ago │
└──────────────────────────────────────┘
```

### STATUS

The diagnostic page makes it possible to inspect basic connectivity
without opening a serial monitor.

``` text
┌──────────────────────────────────────┐
│ SYSTEM STATUS            10/05 06:10│
│ ──────────────────────────────────── │
│ Wi-Fi                       -57 dBm  │
│ MQTT                      CONNECTED  │
│ Sensor                         B3D8  │
│ Data age                    28s ago  │
│ IP                    192.168.3.xxx  │
│ Uptime                      2h 14m   │
│ ──────────────────────────────────── │
│ ● MQTT                       v0.3.x  │
└──────────────────────────────────────┘
```

## Environment

Development/test configuration:

-   M5Stack M5GO v2.7
-   M5Stack ESP32 board package 3.3.9
-   M5Unified 0.2.25
-   M5GFX 0.2.32
-   PubSubClient 2.8
-   ArduinoJson 7.4.3
-   ESP32 `Preferences`
-   Wi-Fi
-   MQTT broker (Mosquitto assumed)
-   Home Assistant
-   SwitchBot temperature/humidity sensor

## Project Layout

``` text
M5GO-SwitchBot-EnvMonitor/
├── M5GO-SwitchBot-EnvMonitor.ino
├── config.h
├── config.example.h
├── README.md
├── README-ja.md
├── .gitignore
└── tools/
    └── ...
```

`config.h` contains local Wi-Fi and MQTT credentials and must remain
outside version control.

## Configuration

Copy the example configuration and edit it for your environment:

``` bash
cp config.example.h config.h
```

Example:

``` cpp
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define MQTT_HOST "192.168.3.200"
#define MQTT_PORT 1883

#define MQTT_USERNAME "YOUR_MQTT_USERNAME"
#define MQTT_PASSWORD "YOUR_MQTT_PASSWORD"

#define MQTT_CLIENT_ID "m5go-switchbot-b3d8"
#define MQTT_TOPIC_ENV "home/env/switchbot-b3d8/raw"

#define TZ_INFO "JST-9"
#define NTP_SERVER_1 "ntp.nict.jp"
#define NTP_SERVER_2 "ntp.jst.mfeed.ad.jp"
#define NTP_SERVER_3 "pool.ntp.org"
```

Do not commit `config.h`.

## Home Assistant / MQTT

Example Home Assistant entities:

``` text
sensor.meter_b3d8_temperature
sensor.meter_b3d8_humidity
```

MQTT topic:

``` text
home/env/switchbot-b3d8/raw
```

Publishing is recommended when either sensor state changes, when Home
Assistant starts, and once every minute as a heartbeat.

Using `retain: true` lets the M5GO receive the last published values
immediately after reconnecting.

## Data Freshness

The M5GO currently determines freshness from the time at which it
receives an MQTT message.

``` text
0–179 seconds     LIVE
180–599 seconds   STALE
600+ seconds      OFFLINE
```

With a one-minute heartbeat, this primarily represents the health of the
**Home Assistant → Mosquitto → M5GO** delivery path.

It does not necessarily represent the age of the underlying SwitchBot
measurement itself.

## JetBrains Mono

The UI is designed to support JetBrains Mono.

The previous LittleFS + `loadFont()` implementation is not used because
it caused a compatibility problem with the M5GFX 0.2.32 / ESP32 core
3.3.9 combination.

Instead, the project uses a firmware-embedded M5GFX / Adafruit GFX
compatible font approach. If the generated JetBrains Mono font is
unavailable, the application can fall back to the FreeMono fonts
included with M5GFX.

JetBrains Mono is distributed by JetBrains under the SIL Open Font
License 1.1.

## Security

Never commit:

-   Wi-Fi SSIDs or passwords
-   MQTT usernames or passwords
-   Other environment-specific secrets

Keep `config.h` in `.gitignore`.

If it was previously tracked by Git, remove it from the index:

``` bash
git rm --cached config.h
```

## License

The application source follows the license specified by this repository.

JetBrains Mono is distributed under the SIL Open Font License 1.1.
