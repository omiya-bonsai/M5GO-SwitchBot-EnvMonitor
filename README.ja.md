[English](README.md) \| **日本語**

# M5GO SwitchBot EnvMonitor

M5Stack M5GO v2.7 を、SwitchBot 温湿度計の常設環境モニターとして利用する
Arduino プロジェクトです。

SwitchBot の温度・湿度を Home Assistant
で取得し、MQTT（Mosquitto）経由で M5GO に配信します。M5GO
は温湿度のメイン画面に加えて通信状態を確認できる STATUS 画面を備え、3
分間ボタン操作がない場合はバックライトを自動消灯します。

## システム構成

``` text
SwitchBot 温湿度計
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

MQTT ペイロード例:

``` json
{
  "id": "switchbot-b3d8",
  "temperature": 24.9,
  "humidity": 62.0
}
```

Home Assistant 側では、値の変化時に加えて 1 分ごとの heartbeat Publish
を行う構成を推奨します。これにより、M5GO は MQTT
経路の生存状態を監視できます。

## 主な機能

-   SwitchBot の温度・湿度を MQTT から受信
-   320 × 240 の 2 カラム MAIN 画面
-   Wi-Fi、MQTT、データ経過時間、IP アドレス、uptime を表示する STATUS
    画面
-   8-bit `M5Canvas` によるオフスクリーン描画
-   3 分間ボタン操作がない場合に Display バックライトを自動消灯
-   Display 消灯中も Wi-Fi / MQTT / NTP は継続動作
-   MQTT 受信では Display を自動点灯しない
-   5 段階の輝度調整
-   輝度設定を NVS (`Preferences`) に保存
-   JetBrains Mono 対応
-   JetBrains Mono が利用できない場合は FreeMono にフォールバック
-   NTP による日本時間表示
-   MQTT データの LIVE / STALE / OFFLINE 表示

## ボタン操作

  ボタン   短押し                   長押し
  -------- ------------------------ ---------------------
  A        Display ON / OFF         輝度変更
  B        MAIN / STATUS 切り替え   予約
  C        MAIN に戻る              Wi-Fi / MQTT 再接続

Display 消灯中は、A / B / C のどのボタンを押しても最初の操作は **Display
の復帰だけ**を行います。復帰時には最新の受信値を描画します。

輝度は次の 5 段階です。

``` text
40 → 80 → 120 → 160 → 220 → 40 ...
```

## Display の自動消灯

最後のボタン操作から 180 秒経過すると、バックライトを消灯します。

``` text
3分間ボタン操作なし
        ↓
Display brightness = 0
        ↓
Wi-Fi / MQTT / NTP は継続
```

ESP32 自体を Sleep させる機能ではありません。そのため MQTT
接続を維持したまま、ボタン操作で即座に画面を復帰できます。

M5GO v2.7 は TFT LCD
のため、本プロジェクトではスクリーンセーバーを動かし続けるのではなく、一定時間後にバックライトを消す方式を採用しています。

## 画面

### MAIN

温度・湿度を大きく表示する通常画面です。

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

M5GO 単体で通信状態を確認するための診断画面です。

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

## 必要環境

開発・動作確認時の構成:

-   M5Stack M5GO v2.7
-   M5Stack ESP32 board package 3.3.9
-   M5Unified 0.2.25
-   M5GFX 0.2.32
-   PubSubClient 2.8
-   ArduinoJson 7.4.3
-   ESP32 `Preferences`
-   Wi-Fi 接続
-   MQTT broker（Mosquitto を想定）
-   Home Assistant
-   SwitchBot 温湿度センサー

## プロジェクト構成

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

`config.h` には Wi-Fi や MQTT の認証情報を含むため Git
管理対象から除外します。

## 設定

`config.example.h` を `config.h`
としてコピーし、環境に合わせて編集します。

``` bash
cp config.example.h config.h
```

例:

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

`config.h` はコミットしないでください。

## Home Assistant / MQTT

使用する Home Assistant エンティティの例:

``` text
sensor.meter_b3d8_temperature
sensor.meter_b3d8_humidity
```

MQTT topic:

``` text
home/env/switchbot-b3d8/raw
```

値の変化時、Home Assistant 起動時、および 1 分ごとに Publish
する構成を推奨します。

`retain: true` にすると、M5GO
が再接続した直後にも最後の値を取得できます。

## データ鮮度

M5GO は MQTT メッセージを受信した時刻を基準に状態を判定します。

``` text
0 ～ 179秒     LIVE
180 ～ 599秒   STALE
600秒以上      OFFLINE
```

1 分 heartbeat を使用した場合、この状態は主に **Home Assistant →
Mosquitto → M5GO の通信経路が生きているか**を表します。

SwitchBot センサーそのものの最終測定時刻とは異なる点に注意してください。

## JetBrains Mono

UI は JetBrains Mono の利用を想定しています。

以前の LittleFS + `loadFont()` 方式は、M5GFX 0.2.32 / ESP32 core 3.3.9
の組み合わせで互換性問題が発生したため使用しません。

プロジェクトでは JetBrains Mono を M5GFX / Adafruit GFX
互換フォントとしてファームウェア側へ組み込む方式を採用します。フォント生成前や利用できない場合は
M5GFX 内蔵 FreeMono にフォールバックできます。

JetBrains Mono は JetBrains により SIL Open Font License 1.1
で公開されています。

## セキュリティ

次の情報を Git リポジトリへコミットしないでください。

-   Wi-Fi SSID / password
-   MQTT username / password
-   その他ローカル環境固有の秘密情報

`.gitignore` には `config.h` を含めてください。

既に `config.h` を Git 管理下へ追加している場合は、追跡を解除します。

``` bash
git rm --cached config.h
```

## ライセンス

プロジェクト本体のライセンスはリポジトリで指定したライセンスに従います。

JetBrains Mono は SIL Open Font License 1.1 に従います。
