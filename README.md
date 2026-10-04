# M5GO-SwitchBot-EnvMonitor v0.3.1

M5GO v2.7 + M5Unified で Home Assistant / MQTT 経由の SwitchBot 温湿度を表示するモニターです。

## 機能
- MAIN: 温度・湿度、MQTT freshness
- STATUS: Wi-Fi RSSI / MQTT / data age / IP / uptime
- 3分無操作でバックライトOFF（ESP32/MQTTは稼働継続）
- A短押し: Display ON/OFF
- A長押し: 輝度 40/80/120/160/220、NVS保存
- B短押し: MAIN/STATUS
- C短押し: MAIN
- C長押し: Wi-Fi/MQTT再接続
- 8-bit M5Canvas
- JetBrains Mono GFXfont対応（LittleFS不使用）

## 対象環境
- M5Stack ESP32 board package 3.3.9
- M5Unified 0.2.25
- M5GFX 0.2.32
- PubSubClient 2.8
- ArduinoJson 7.4.3

## 1. config.h
`config.example.h` を `config.h` にコピーし、Wi-Fi/MQTT認証情報を設定します。

## 2. JetBrains Monoを生成
macOS Terminalでプロジェクトディレクトリへ移動し:

```bash
./tools/install_jetbrains_mono.sh
```

公式JetBrains Mono Regularを取得し、Adafruit `fontconvert` で9/12/24ptのGFXfontヘッダを生成します。LittleFSは使いません。

生成されるファイル:
- JetBrainsMono9pt7b.h
- JetBrainsMono12pt7b.h
- JetBrainsMono24pt7b.h
- OFL-JetBrainsMono.txt

生成前でもFreeMono fallbackでコンパイルできます。生成後は自動的にJetBrains Monoへ切り替わります。

## 3. Home Assistant
`home-assistant/switchbot-b3d8-mqtt.yaml` を参考にAutomationを設定してください。state change + HA start + 1分heartbeatでretain publishします。

Topic: `home/env/switchbot-b3d8/raw`

## 注意
現在のM5GOのData ageは「M5GOがMQTTを最後に受信した時刻」です。SwitchBot自身の最終測定時刻とは別です。
