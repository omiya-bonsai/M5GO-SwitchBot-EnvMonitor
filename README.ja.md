[English](README.md) | **日本語**

# M5GO SwitchBot EnvMonitor — v0.8.0

M5Stack M5GO v2.7 を常設環境モニターとして使う Arduino プロジェクトです。Home Assistant が取得した SwitchBot の温湿度を MQTT broker 経由で受信し、現在値、SD 上の履歴・統計を表示します。内蔵 LED は環境異常のインジケーターとして使います。

## 機能と構成

`SwitchBot → Home Assistant → MQTT broker（Mosquitto 等）→ M5GO`

| 機能 | 動作 |
| --- | --- |
| 現在値 | 320 × 240 MAIN 画面、LIVE / STALE / OFFLINE / WAIT |
| 履歴 | 互換性を維持した1分間隔の環境 CSV、直近24時間グラフ |
| 統計 | 温湿度の平均・最小・最大、前の24時間との比較 |
| 操作 | A/B/C ボタン、モーション Wake、左右傾斜の HOLD |
| UI 復元 | ページと輝度を NVS に保存、起動時 Display ON |
| 環境インジケーター | 内蔵10灯、NORMAL 消灯／WARNING アンバー／CRITICAL 赤 |
| ブラックボックス | 通信・heap・バッテリー・データ鮮度の独立したシステム CSV |
| 常時運用 | 通信自動再接続、無操作時 LCD 消灯、ESP32 は継続動作 |
| 描画 | 8-bit M5Canvas、組み込み JetBrains Mono／FreeMono fallback |

## ハードウェアとソフトウェア

| ハードウェア | 用途 |
| --- | --- |
| M5Stack M5GO v2.7 / ESP32 | 対象機種、常時メインループを実行 |
| MPU6886 | 内蔵加速度センサー、Wake と傾斜操作 |
| SK6812 RGB ×10 | 内蔵環境インジケーター、GPIO15 |
| microSD | 環境／システム CSV、CS GPIO4、SPI 25 MHz |
| LCD | 320 × 240、rotation 1、バックライト制御 |
| A/B/C buttons | IMU 操作と併用可能な物理ボタン |
| Battery | M5.Power による残量 %、電圧の生値はシステム CSV のみ維持 |
| Wi-Fi | MQTT と NTP の通信 |
| TMOS／DLight（任意） | Port Aでpresence／motion・照度を観測。照明自動操作なし |

Charger Base は任意であり、ファームウェア動作の必須要件ではありません。常時運用には適切な電源が必要です。バッテリー持続時間は規定していません。

以下は提示された対象環境です。リポジトリに依存バージョンの固定設定はありません。v0.8.0 はローカル導入済みの M5Stack ESP32 package 3.3.9、M5Unified 0.2.21、M5GFX 0.2.28、PubSubClient 2.8、ArduinoJson 7.4.3 でビルド成功しました（`m5stack:esp32:m5stack_core`、プレースホルダー設定）。指定の M5Unified 0.2.25／M5GFX 0.2.32 の組み合わせや実機動作を検証したものではありません。

| 依存ソフトウェア | バージョン／用途 |
| --- | --- |
| ESP32 board package | 3.3.9、Arduino-ESP32 3.x RMT API |
| M5Unified | 0.2.25 |
| M5GFX | 0.2.32 |
| PubSubClient | 2.8 |
| ArduinoJson | 7.4.3 |
| M5-STHS34PF80 | 0.0.1、TMOS公式センサードライバー |
| ESP32 同梱 API | WiFi、Preferences、SD、SPI、time、heap 診断、RMT |

外部 RGB LED ライブラリは不要です。

書斎context版ローカルビルド：Flash 1,293,263 / 1,310,720 bytes（98%）、残り17,457 bytes。global RAM 86,612 / 327,680 bytes（26%）。TMOS単独版（Flash 1,291,039、RAM 86,556 bytes）からFlash +2,224、RAM +56 bytesです。確認環境はESP32 3.3.9、M5Unified 0.2.21、M5GFX 0.2.28、M5-STHS34PF80 0.0.1。contextの実機動作はユーザー確認済みです（10秒hold満了・再入室による遷移、motionでholdを延長しないこと）。

## 画面とボタン

B 短押しによる循環順序：

`MAIN → SYSTEM STATUS → TEMP / 24H → HUM / 24H → TEMP / STATS → HUM / STATS → MAIN`

| ページ | 内容 |
| --- | --- |
| MAIN（`ENV / B3D8`） | 最新温湿度、data state、data age |
| SYSTEM STATUS | Wi-Fi RSSI／offline、MQTT、data age、環境状態、SD、IMU、バッテリー %、uptime |
| TEMP / 24H | `/logs` による直近24時間の温度グラフ |
| HUM / 24H | `/logs` による直近24時間の湿度グラフ |
| TEMP / STATS | 温度 NOW、24H AVG/MIN/MAX、PREV AVG、vs PREV |
| HUM / STATS | 同じ項目の湿度統計 |

| ボタン | 短押し | 長押し（800 ms） |
| --- | --- | --- |
| A | Display OFF | LCD 輝度を変更 |
| B | 次のページ | 予約、操作時刻の更新のみ |
| C | MAIN に戻る | 書斎シーリングライトの toggle コマンド送信 |

短押しは離したとき、長押しは保持中に1回実行し、その後の短押し処理は抑制します。Display OFF 中は、どのボタンの短押し／長押しも現在のページのまま Wake するだけで、通常の操作は実行しません。短押しは離した時点、長押しは800 ms到達時点で Wake します。

LCD 輝度は `40 → 80 → 120 → 160 → 220 → 40`。初期値160、NVS（`envmonitor` / `brightness`）へ保存します。点灯中は約1秒ごと、有効な MQTT 受信時、ボタン操作時に描画します。傾斜操作はページ変更とグラフ読み込みを行い、通常の更新で描画されます。

最後の操作時刻から180秒でバックライトを消灯します。ボタン、Wake、成立した傾斜操作が操作時刻を更新します。点灯中の通常の動きや MQTT 受信では更新しません。ESP32 の Sleep ではなく、消灯中も通信・再接続・NTP・SD ログ・IMU・HEALTH 監視を継続します。MQTT 受信で Display を Wake しません。

## 再起動後の UI 状態復元（v0.6.1）

USB 抜去時には一瞬の電源断と ESP32 の `POWERON_RESET` が観測され、IP5306 boost keep-on の検証でも解消していません。v0.6.1 は再起動後に UI 設定を復元するもので、USB→バッテリーの完全無停止切替を保証しません。一時的な IP5306 検証処理は削除し、通常の M5Unified 電源初期化を使用します。

| NVS 項目 | 保存値／方針 |
| --- | --- |
| namespace | 既存の `envmonitor` |
| `page` | unsigned byte：MAIN=0、STATUS=1、TEMP / 24H=2、HUM / 24H=3、TEMP / STATS=4、HUM / STATS=5 |
| `brightness` | 既存の unsigned-byte index 0〜4、40/80/120/160/220 に対応 |
| Display ON/OFF | 保存せず起動時は必ず ON。正常起動が見えなくなるのを防ぐため |

キー欠落、型不一致、範囲外の値は MAIN／輝度160へフォールバックします。既存の輝度保存値も引き継ぎます。NVS は B/C または IMU でページが実際に変わったとき、および A 長押しで輝度が変わったときだけ書き込みます。同じページの再選択は保存しません。起動時の復元、再描画、MQTT 受信、通常の loop、Wake、自動／手動消灯では書き込みません。NVS 初期化失敗時は既定値で継続して永続化を無効にし、保存失敗時もログを出して動作を継続します。書き込み確定前の電源断では、前の保存値が残る可能性があります。

起動順序は M5 初期化 → LCD/NVS 状態読み込み・8-bit canvas 確保 → IMU → RGB → SD → MQTT 設定 → Wi-Fi 接続開始 → 復元ページの初回描画です。SD 初期化前に復元ページを描画しません。グラフ読み込みは SD／時計が有効になるまで待ち、NTP 後のグラフ再描画で再試行します。統計は描画時に計算します。時刻やデータが得られるまでは履歴なし表示や `--` になります。180秒の無操作タイマーは起動後に開始し直します。

Wi-Fi／MQTT は通常どおり再接続・subscribe し、retained MQTT から環境値を再取得します。温湿度、data age、時刻、RSSI、通信／セッション状態、グラフデータは NVS に保存せず、MQTT・時刻サービス・SD から再構築します。鮮度判定は従来どおり有効な受信時刻が基準です。

SYSTEM STATUS と HEALTH のバッテリー表示は残量%のみです。運用済みログとの互換性のため、`/system` CSV は v0.6.0 の `battery_mv` 列を維持します。観測された0は電圧取得不可／無意味な値であり、バッテリーが空という意味ではありません。CSV 列や保存先は削除していません。

## 書斎シーリングライト操作（v0.7.0）

Display ON 中の C 長押し（800 ms）で、既存の MQTT 接続を使い `home/control/study/ceiling_light/toggle` へ payload `PRESS` を1回、**retain=false**、QoS 0 で publish します。設定済み Home Assistant Automation が `light.sirinkuraito` に `light.toggle` を実行します。HA 側の変更は不要です。C 短押しは引き続き MAIN。C 長押しの手動強制再接続は廃止し、自動 Wi-Fi／MQTT 再接続と backoff は維持します。

既存の長押し latch により保持中は1回だけ実行し、離したときの短押しを抑制します。Display OFF 中の C 短押し／長押しは Wake のみです。一度離して改めて長押ししたときだけ送信します。コマンドのキュー保存・再試行・NVS 保存はしません。MQTT 未接続または publish が false を返した場合は Serial に失敗を出し、環境 LED をそのまま維持します。

ローカル publish 成功時だけ、1灯が index 0〜9 を30 ms刻みで流れる約300 msの演出を開始します。Omarchy 風の落ち着いた blue/light blue/cyan/aqua/muted green で、RGB 値は `(4,6,8)`、`(6,7,9)`、`(6,9,10)`、`(6,9,8)`、`(7,8,6)`（既存強度12未満）です。LED ライブラリは追加しません。`millis()` 状態機械を loop から進め、演出用 delay はありません。ただし既存の同期 publish・SD・RMT 処理で実際の時間は延び得ます。RMT 送信自体は既存の同期 API／100 ms timeout を維持します。

演出中は環境 LED を一時的に上書きします。MQTT の温湿度更新は継続し、終了後は最新の NORMAL/WARNING/CRITICAL を再判定して復帰します。RGB が使えなくてもコマンド送信は可能です。Serial は `LIGHT: toggle command published` または送信失敗を記録します。LED は**コマンド送信のフィードバックであり、照明の点灯／消灯を示しません**。M5GO は照明状態を管理せず、状態の購読や LCD レイアウト変更もありません。publish 成功は HA 実行や実際の照明動作の確認ではありません。

### SD 効果音フィードバック（任意）

照明コマンドの publish 成功後、既存 SD の `/sounds/light-toggle.wav` を M5Unified Speaker の `playRaw(int16_t*)` でストリーミング再生します。標準 RIFF/WAVE の signed 16-bit PCM、mono/stereo、8〜48 kHz に対応し、現在の約2.5秒・48 kHz stereo WAV は変換せず使用できます。WAV を Flash へ埋め込まず、Git へ追加せず、音声ライブラリも追加しません。適切な利用許諾のある WAV を配置してください。

音量は `App::LIGHT_TOGGLE_SOUND_VOLUME = 48`（0〜255）。3個の8 KB PCM バッファを順に使い、Speaker の非同期再生が参照するデータを維持します。loop は2スロットのキューに空きがあるときだけ最大1チャンク（8 KB）を読み込み、音声全体の終了を待ちません。SD 読み込み自体は同期処理で、CSV・統計・通信処理が遅いと音切れが起こり得ます。音質・音量は実機確認が必要です。同じ SD インスタンスをメインループの音声読み込みとログ書き込みで共有し、SD 再初期化や CSV 変更はしません。

LED は従来どおり約300 msで終了し、音と独立して環境表示へ戻ります。SD／ファイルなし、WAV 不正、Speaker 初期化／キュー投入失敗は AUDIO 警告を出すだけで、MQTT 成功と LED を妨げません。MQTT 失敗時や Display OFF の Wake では音を開始しません。再生中の再度の有効 C 長押しでは MQTT と LED は通常どおり実行し、音のみスキップして多重再生／再スタートしません。音はコマンド送信のフィードバックであり、実際の照明 ON/OFF や HA 実行確認ではありません。起動音は要求しません。圧縮／float／extensible WAV は拒否し、それらの場合のみ PCM 変換が必要です。指定の標準48 kHz WAV の変換は不要です。

## KEY UNIT（任意、Port B）

KEY UNITをPort Bへ接続します。白線のボタン信号はGPIO36（LOWで押下）、黄線のSK6812入力はGPIO26（未使用）です。[KEY公式PinMap](https://docs.m5stack.com/en/unit/Unit_Key)と[M5GO v2.7 Port B PinMap](https://docs.m5stack.com/en/core/M5GO_IoT_Kit_v2.7)に対応しています。追加ライブラリは不要です。

標準構成ではKEYをPort Bへ常設し、既定は`#define KEY_UNIT_ENABLED 1`です。KEYを取り外して運用する場合は`0`へ変更して再ビルドしてください。`0`ではKEY初期化・GPIO36監視・操作をコンパイル対象から除外します。GPIO36は入力専用で内部pull-upがないため、有効時は`INPUT`を使用し、UNIT内の10kΩ pull-upを前提とします。外付け抵抗は追加しません。UNIT未接続・取り外し時はKEYを無効にしてください。

30msのnon-blocking debounceで押下時に1回だけtoggleし、debounce済みrelease後に次の押下を受け付けます。起動時に押されていてもtoggleせず、一度離して押し直す必要があります。C長押しと同じ`publishStudyLightToggle()`を呼び、MQTT PRESS・成功時LED・`/sounds/light-toggle.wav`を共用します。失敗時・音声再生中の仕様も既存どおりです。KEYはDisplay OFF中も有効で、Display Wakeや無操作タイマー更新は行いません。A/B/CのWake-onlyは変更しません。KEY内蔵SK6812は制御せず、GPIO26には触れません。

debounce済みイベントは`PERF KEY press t=...`、`PERF KEY action t=...`、`PERF KEY release t=...`で確認できます。actionは操作試行を示し、MQTT成功／失敗は既存LIGHTログで確認します。

## Port A TMOS（任意）

TMOS PIR UNIT（STHS34PF80、I²C `0x5A`）をPort A（SDA GPIO21／SCL GPIO22）へ接続します。M5Unifiedの既存`In_I2C`を共有し、UNIT通信は100kHz。バスを解放・再初期化せず、内蔵IMUとの通信を維持します。

DRDY確認は50ms周期、ODR8Hz、presence／motion閾値200、hysteresis50です。正常なDRDY付き読み取りでpresence／motion flagを更新し、下記contextに使用します。TMOSからDisplay・照明を操作しません。Gesture依存のWake処理は撤去したままです。

依存ライブラリはM5-STHS34PF80 **0.0.1**です。未接続・初期化失敗はTMOSのみ無効化し、読み取り失敗・復帰を記録します。初期化期限500msはI²C通信の合間で確認します。初期化できなかったUNITを再接続した場合は再起動してください。

PERFは`tmos_poll_max_us`、`last_us`、`calls`、`tmos_errors`と、既存loop／UI／cache計測を維持しています。

スケッチ冒頭の`TMOS_DIAGNOSTICS`を`1`にして再ビルドすると、約1秒周期の`TMOS STATE:` flag／hold／freshness診断を有効にできます。既定値`0`では詳細診断コード・バッファをコンパイル対象から除外し、通常PERFとセンサー／context処理は維持します。

Gesture UNITは評価しましたが、false positiveのため本番の照明操作入力として採用しません。[実機評価記録](docs/gesture-unit-evaluation.md)を参照してください。

## 書斎環境コンテキスト（TMOS + DLight）

TMOS（`0x5A`）とDLight／BH1750FVI（`0x23`）をY字GROVEでPort Aへ接続します。SDA GPIO21／SCL GPIO22を共有し、UNIT通信は100kHz。内蔵IMUバスを再初期化しません。DLightはM5Unifiedの`In_I2C.start/write/read/stop`で、[公式M5-DLightのコマンド・lux換算](https://github.com/m5stack/M5-DLight/blob/master/src/M5_DLight.cpp)に従って通信します。DLightライブラリの追加は不要です。導入済みM5-DLight0.0.3は調査しましたがリンクしません。同版の`getLUX()`は生カウントを返し、今回採用した現行公式のraw／1.2換算とは異なります。

| 入力 | 役割・周期 |
| --- | --- |
| TMOS | presence／motion観測。既存50ms DRDY確認、ODR8Hz |
| DLight | 照度観測。連続高分解能モード（`0x10`）、1秒周期読み取り。16-bit big-endian生値／1.2 |
| 物理C長押し | 明示的な照明toggle。既存MQTT・LED・効果音を維持 |

**TMOS／DLightは照明の自動操作やDisplayのWake／sleepを行いません。** KEY UNITは有効化した場合のみ明示的な照明操作入力となります（上記参照）。Gestureは撤去したままです。

| lux | 照度分類 |
| --- | --- |
| `<50` | DARK |
| `50〜<300` | DIM |
| `300〜<1000` | NORMAL |
| `>=1000` | BRIGHT |

閾値は`App::DLIGHT_DARK_LUX`、`DLIGHT_NORMAL_LUX`、`DLIGHT_BRIGHT_LUX`で調整できます。照度分類のhysteresisはまだありません。occupancyは最後の正常なpresence=1から10秒保持し、単発の負sampleですぐ解除しません。motionは別に扱い、holdを延長しません。TMOSは2秒、DLightは3秒で取得値を期限切れとし、読み取りエラー時は即時無効にして次の正常なDRDY付きsample／読み取りで復帰します。未接続を暗い／不在と推定せずUNKNOWNとします。初期化は起動時のみなので、未接続UNITを再接続した場合は再起動してください。

有効な観測では`VACANT_DARK`、`VACANT_LIGHT`、`OCCUPIED_DARK`、`OCCUPIED_LIGHT`を生成します。DIM／NORMAL／BRIGHTはLIGHT扱いです。どちらかのセンサーが無効ならcontextは`UNKNOWN`です。これは観測結果であり、人の在室を保証する判定ではありません。

MAINの内容領域下部へ照度・hold後の在室状態を追加しました。無効時は`LUX --`／`PRESENCE --`。既存6ページと描画周期を維持します。

MQTTは`home/env/study/context`へnon-retained・QoS0でpublishします。分類／在室／motion／validity変化時、再接続後、または60秒heartbeatで送信し、試行は最短1秒間隔です。短時間の変化は最新状態へまとめられ、同じ照度分類内のlux変化はheartbeatで通知します。offline queueは追加しません。接続中の既存PubSubClient publishは同期処理なので、障害時の時間は実機PERFで確認してください。

```json
{"lux":327.4,"light_state":"NORMAL","occupied":true,"motion":false,"context":"OCCUPIED_LIGHT"}
```

無効なluxは`null`と`light_state:"UNKNOWN"`、無効なpresence／motionは`null`、複合contextは`UNKNOWN`です。MQTT失敗でも観測値や照明操作権限は変化しません。Serialは正常な照度値を1秒ごと（`DLIGHT: lux=327.4 state=NORMAL`）、context遷移とread failure／recoveryを出力します。PERFへ`dlight_poll_max_us`、`last_us`、`dlight_calls`、`dlight_errors`を追加しました。I²C readのみを測り、Serial／MQTT／LCD時間は含めません。変換待ちはセンサー内部で行い、loopで待ちません。I²C通信そのものは既存ドライバーのtimeoutに従う同期処理です。

context SDログとDLightグラフは追加していません。既存温湿度・システムCSVのschema、Graph／STATS cache、ボタン、NVS、IMU、180秒auto-offを維持しています。

## IMU ジェスチャー

| 操作／設定 | 実装 |
| --- | --- |
| サンプリング | 約100 ms ごとに加速度を取得 |
| モーション Wake（消灯中） | 連続サンプルの加速度ベクトルの大きさの差の絶対値 ≥0.22 g、Wake cooldown 3,000 ms |
| 左傾斜（点灯中） | 水平方向 ≤−0.55 g を500 ms HOLD → TEMP / 24H |
| 右傾斜（点灯中） | 水平方向 ≥+0.55 g を500 ms HOLD → HUM / 24H |
| 傾斜 cooldown | 傾斜操作成立またはモーション Wake から1,500 ms |
| neutral / release | 水平方向の絶対値 ≤0.30 g で保留中の判定を消去し latch 解除 |
| 向きの調整 | `App::IMU_REVERSE_LEFT_RIGHT = false`。設置方向で左右が逆になる場合は true で X の符号を反転 |

水平判定は加速度 X（`ax`、必要なら符号反転）です。閾値は角度ではなく加速度成分です。方向が変われば HOLD を開始し直し、傾斜閾値未満に戻れば保留中の HOLD を取り消します。成立後は neutral に戻るまで latch で再操作を防止します。モーション Wake 後は HOLD を消去して latch を設定し、同じ動きで即座にページが変わるのを防ぎます。消灯後の最初のサンプルは加速度の基準値を設定します。IMU 初期化で加速度を取得できない場合はモーション機能を無効にし、ボタン操作は利用可能です。

ユーザーによる実機確認済みの動作は、左 → TEMP / 24H、右 → HUM / 24H、モーション Wake です。今回の音声追加前に v0.7.0 の照明操作・RGB 演出もユーザーが実機確認済みです。これらの実機確認は今回繰り返していません。

## 環境閾値と RGB LED

CRITICAL を先に判定し、温度または湿度のどちらかで状態が決まります。範囲外の比較は厳密な `<` / `>` です。

| 状態 | 条件 | LED |
| --- | --- | --- |
| NORMAL | 温度18〜28 °C **かつ** 湿度40〜70%（境界を含む）。有効データ受信前も該当 | 全灯消灯 |
| WARNING | CRITICAL ではなく、温度 <18 または >28 °C **または** 湿度 <40 または >70% | 先頭3灯アンバー、残り7灯消灯 |
| CRITICAL | 温度 <15 または >32 °C **または** 湿度 <30 または >80% | 全10灯赤 |

ちょうど15/32 °C、30/80%は、別の値が CRITICAL でなければ WARNING です。`RGB_LED_BRIGHTNESS = 12` は0〜255のチャネル強度で、アンバー RGB=(12,6,0)、赤 RGB=(12,0,0)。LCD 輝度とは独立しています。正常時の装飾点灯ではなく、異常を知らせる設計です。

GPIO15 を Arduino-ESP32 RMT 10 MHz、メモリ4ブロック、240 symbols/frame で制御し、バイト順は GRB です。0 bit は HIGH 0.4 µs + LOW 0.8 µs、1 bit は HIGH 0.8 µs + LOW 0.4 µs。初期化時は全灯を消灯し、RMT 初期化／初回消灯送信が失敗すれば RGB 機能を無効にします。

LED は有効な MQTT 受信時に更新し、Display OFF とは独立しています。判定は最後の受信値を使用し、鮮度による抑制やヒステリシスはありません。古い異常値で点灯が続く場合があり、NORMAL は通信正常やデータの新しさを保証しません。

## SD ログとブラックボックス

```text
/
├── logs/
│   └── YYYY-MM-DD.csv     # 環境履歴、従来の形式を維持
├── sounds/
│   └── light-toggle.wav   # 任意の効果音、別途配置
└── system/
    └── YYYY-MM-DD.csv     # システム診断ブラックボックス
```

両方とも共通の60,000 ms間隔チェックで記録します。ファイル名はローカル日付（`TZ_INFO`、標準は JST）です。追記形式で、ファイルが存在しないときのみヘッダーを書き、毎回 close します。SD が利用可能で、時計が有効（`time >= 1704067200`、2024-01-01 UTC）なことが必要です。有効時刻が得られる前は両方とも記録せず、未同期用ファイルや後からの補完はありません。この時計判定はタイムスタンプによるもので、新しい NTP 応答の証明ではありません。

`/logs` はさらに有効な MQTT 温湿度を一度以上受信している必要があります。その後は STALE/OFFLINE でも最後の値を記録し続け、鮮度フィルターはありません。`/system` は WAIT 中も記録し、Wi-Fi/MQTT 障害やメモリ状態を後から調査するブラックボックスです。周期的な状態スナップショットであり、すべてのイベントの履歴ではありません。

### 環境 CSV（互換性維持）

```csv
timestamp,temperature,humidity,rssi,mqtt
```

| フィールド | 意味 |
| --- | --- |
| timestamp | 記録時のローカル `YYYY-MM-DD HH:MM:SS` |
| temperature | 最後に受理した温度 °C、小数1桁 |
| humidity | 最後に受理した相対湿度 %、小数1桁 |
| rssi | Wi-Fi RSSI、dBm。未接続時0 |
| mqtt | 接続1、未接続0 |

### システム CSV

```csv
timestamp,rssi,wifi,mqtt,heap,min_heap,largest_heap,battery_pct,battery_mv,data_state,data_age_s
```

| フィールド | 意味 |
| --- | --- |
| timestamp | 同じ形式のローカル記録日時 |
| rssi | Wi-Fi RSSI、dBm。未接続時0 |
| wifi | `WL_CONNECTED` なら1、それ以外0 |
| mqtt | 接続1、未接続0 |
| heap | 現在の free heap、bytes（`ESP.getFreeHeap`） |
| min_heap | 起動後の minimum free heap、bytes（`ESP.getMinFreeHeap`） |
| largest_heap | 最大の割り当て可能な8-bit heap block、bytes |
| battery_pct | M5.Power のバッテリー残量 % |
| battery_mv | M5.Power の電圧の生値 mV。この M5GO v2.7 構成では0を観測し、有効な電圧値ではない |
| data_state | WAIT / LIVE / STALE / OFFLINE |
| data_age_s | 最後の有効 MQTT 受信からの秒数、WAIT は0 |

SD 初期化失敗時はログと SD 履歴を無効にしますが、現在値表示と通信を継続します。ディレクトリ作成／ファイル open の失敗は Serial へ出力し、メインループを停止しません。SD 再マウント／復旧ループ、バッファ保存・補完、古いログの自動削除、書き込みバイト数／永続化の検証はありません。運用中の I/O エラー後も SD 表示が READY のままの場合があります。保存障害は Serial とファイルを確認し、CSV の退避・整理は外部で行ってください。

## 24時間グラフと統計

グラフは直近24時間の環境 CSV を288個の5分バケットに読み込みます。同一バケットは最後に読んだ行を採用し、平均化しません。欠損バケットでは線を切ります。STATUS → TEMP / 24H の遷移時、傾斜操作時、およびグラフ表示中の5分間隔で再読み込みします。縦軸はデータ範囲に15%（最低0.5）の余白を付けます。データなしは `NO 24H DATA`、SD 利用不可は `SD UNAVAILABLE` と表示します。

統計は統計ページの描画ごとに CSV を読み直します。パースできた行の有限値を温度／湿度それぞれで集計し、グラフのバケット、RSSI、MQTT 列による選別はしません。

| 項目 | 計算 |
| --- | --- |
| NOW | 最後の有効 MQTT 値。STALE/OFFLINE でも表示し、SD／時計は不要 |
| 24H AVG | `[現在 −24時間, 現在]` の行の算術平均、各行を同じ重みで集計 |
| 24H MIN / MAX | 同じ区間の最小／最大 |
| PREV AVG | `[現在 −48時間, 現在 −24時間)` の行の算術平均 |
| vs PREV | 24H AVG − PREV AVG。温度は °C、湿度はパーセントポイント |

PREV は**現在から24〜48時間前であり、前日の暦日ではありません**。補間、時間加重、24時間分の完全性チェックはありません。同じ古い値が繰り返し記録されれば繰り返し集計します。SD／時計／該当行がなければ履歴値は `--`、現在値がなければ NOW は `--` です。温度は小数1桁、湿度は整数表示ですが、計算は float です。差が正ならオレンジ、負ならシアンです。

## MQTT・設定・運用

`config.example.h` をローカルの `config.h` にコピーして編集します。

```sh
cp config.example.h config.h
```

`config.h` は Git 管理から除外し、SSID、Wi-Fi／MQTT password 等を公開しないでください。`WIFI_SSID`、`WIFI_PASSWORD`、`MQTT_HOST`、`MQTT_PORT`、`MQTT_USERNAME`、`MQTT_PASSWORD` は環境に合わせてローカルで設定し、文書へ転記しません。MQTT username が空なら username/password なしで接続します。

| 公開可能な設定 | 現在の標準値 |
| --- | --- |
| `MQTT_TOPIC_ENV` | `home/env/switchbot-b3d8/raw` |
| `MQTT_CLIENT_ID` | `m5go-switchbot-b3d8`（複数台は一意の ID に変更） |
| `MQTT_PORT` | 1883 |
| `TZ_INFO` | `JST-9` |
| NTP servers | `ntp.nict.jp`、`ntp.jst.mfeed.ad.jp`、`pool.ntp.org` |

```json
{"id":"switchbot-b3d8","temperature":24.9,"humidity":62.0}
```

指定 topic だけを QoS 0 で購読して受理します。JSON の temperature/humidity は数値必須で、有限値かつ温度 −50〜80 °C、湿度0〜100%（境界を含む）を受理します。不正なメッセージでは値も受信時刻も更新しません。`id` は検証せず必須でもありません。測定時刻も読みません。クライアントバッファは512 bytes。`WiFiClient` を使い TLS は実装せず、温湿度を受信し、上記の照明操作コマンドだけを publish します。

[Home Assistant 例](home-assistant/switchbot-b3d8-mqtt.yaml) は、どちらかの値の変化、HA 起動、毎分で実行し、2秒 delay と基本的な unavailable 状態フィルターを経て retain=true、QoS 0 で配信します。`sensor.meter_b3d8_temperature` / `sensor.meter_b3d8_humidity` は環境に合わせて変更してください。古い retained message も新しく受信したものとして扱います。data age は配信経路の鮮度であり、センサーの測定時刻からの経過時間ではありません。

| 通信／堅牢性 | 実装 |
| --- | --- |
| Wi-Fi | STA、persistent off、auto reconnect on、Wi-Fi sleep off |
| Wi-Fi retry | 初回接続待ち15秒。再試行待ち5 → 10 → 20 → 40 → 60秒、上限60秒、接続でリセット |
| MQTT retry | Wi-Fi 接続時のみ。失敗後の待ち2 → 4 → 8 → 16 → 32 → 60秒、上限60秒、接続＋購読成功でリセット |
| MQTT 接続 | keepalive 30秒、socket timeout 5秒、購読失敗で切断、Wi-Fi 未接続時は MQTT 切断 |
| データ鮮度 | 初回有効受信前 WAIT、180秒未満 LIVE、180〜600秒未満 STALE、600秒以上 OFFLINE |
| NTP | Wi-Fi 利用可能時に `configTzTime` を1回設定。以後の同期は時刻サービスが担当 |
| Serial 診断 | 115200 baud、起動時 reset reason／heap、通信・エラー、5分ごと HEALTH |
| HEALTH | uptime、Wi-Fi/MQTT、RSSI、free/minimum/largest heap、data state/age、SD/IMU、battery、環境状態 |

再試行はメインループの期限判定と exponential backoff で制御します。MQTT connect/socket 操作や SD 読み書きは同期処理であり、UI の無停止応答を保証するものではありません。heap は監視しますが、heap を理由とする自動再起動はありません。canvas 確保失敗は fatal で setup を停止します。SD/IMU/RGB 初期化失敗は個別に処理します。今回の v0.7.0 更新では実機／耐久運転試験を行っていません。

## リポジトリとフォント

| パス | 用途 |
| --- | --- |
| `M5GO-SwitchBot-EnvMonitor.ino` | ファームウェア、`App` 定数が閾値・時間設定の実装 |
| `config.example.h` / ローカル `config.h` | 設定例／非公開設定 |
| `README.md` / `README.ja.md` | 英語／日本語ドキュメント |
| `home-assistant/` | MQTT 配信例 |
| `JetBrainsMono*pt7b.h` | 生成済み組み込みフォント |
| `tools/install_jetbrains_mono.sh` | macOS 用フォント取得・変換補助 |
| `fonts/README.md`、`FONT-NOTICE.md`、`OFL-JetBrainsMono.txt` | フォント手順・ライセンス |

Arduino IDE でスケッチを開き、M5GO 対応の ESP32 ボードを選び、依存ライブラリを導入してください。生成済みフォントヘッダーは同梱されており、再生成は任意です。LittleFS フォント upload は使用しません。3つのヘッダーがすべてあれば JetBrains Mono、なければ M5GFX の FreeMono を使用します。

JetBrains Mono は SIL OFL 1.1 です。[フォント注記](FONT-NOTICE.md) と [OFL](OFL-JetBrainsMono.txt) を参照してください。現在の `LICENSE` は空で、アプリケーション本体のライセンスは指定されていません。
