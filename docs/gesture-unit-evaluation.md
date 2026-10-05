# PAJ7620U2 Gesture UNIT 実機評価

## 結論

**Gesture UNITは、M5GO-SwitchBot-EnvMonitorのシーリングライト操作入力として採用しない。**

センサー自体とI²C統合は正常に動作したが、操作意図のない身体動作や周辺の動きをGestureとして検出することがあり、この用途に要求される入力信頼性を満たさなかった。技術的に動作しなかったという評価ではない。

本書はユーザーが報告した実機評価結果とログ抜粋を記録する。数値は確認した範囲の観測値であり、長期安定性や誤検出率を定量的に保証するものではない。評価対象は未コミットのGesture/TMOS統合実験版であり、本書の追加は実装の採用・削除を意味しない。

## 目的

M5GO v2.7の非接触操作入力としてPAJ7620U2 Gesture UNITを利用できるか確認した。I²C共存と処理負荷に加え、照明操作を任せられる認識品質を評価した。

## ハードウェア構成

```text
M5GO v2.7 Port A（SDA GPIO21 / SCL GPIO22、5V / GND）
  │
  └─ Y字GROVEケーブル
       ├─ Gesture UNIT：PAJ7620U2、I²C 0x73
       └─ TMOS PIR UNIT：STHS34PF80、I²C 0x5A
```

PaHubは使用せず、両UNITが同じSDA/SCLを共有した。UNIT通信は100kHz、アドレス競合はない。

## 実装した実験

| 条件 | 実験版の動作 |
| --- | --- |
| Display ON、cooldown外 | Cボタン長押し相当の共通処理でシーリングライトtoggleコマンドを送信 |
| Display ON、cooldown中 | 照明操作を抑制し、分類診断を継続 |
| Display OFF、TMOS presence有効 | Display Wakeのみ。照明toggleなし |
| Display OFF、TMOS presence無効 | Display Wake・照明toggleとも行わず、Serial診断を継続 |

評価した9種類のGestureはすべて同じ機能へ接続した。分類名はSerialと英語音声による認識精度の診断にのみ使用し、LEFT・RIGHTなどに別々の操作を割り当てていない。

Gesture専用cooldownは2秒。TMOS presenceは最後の正常なpresence=1から10秒保持する構成とした。Gesture pollingは5ms周期、TMOS DRDY確認は50ms周期、ODRは8Hzとした。

## 性能測定

実機PERFの観測例：

```text
gesture_poll_max_us=1877
tmos_poll_max_us=1490

gesture_errors=0
tmos_errors=0
```

| 項目 | 観測結果 | 評価 |
| --- | --- | --- |
| Gesture poll最大時間 | 1,877µs（約1.88ms） | 観測例では約2ms未満 |
| TMOS poll最大時間 | 1,490µs（約1.49ms） | 観測例では約2ms未満 |
| 両UNITのI²C読み取りエラー | 各0 | 確認した範囲で共存は良好 |

poll計測はセンサー読み取り部分を対象とし、照明操作・音声開始・Serial出力を含む処理全体の時間とは区別する。今回の観測ではpolling負荷は小さく、Y字GROVEによる同時接続でI²Cエラーは発生しなかった。

既存Bボタン操作のログ抜粋：

```text
held_ms=224
render_done elapsed_us=37569

held_ms=252
render_done elapsed_us=37185
```

短押しが正常に検出され、検出後のrender完了は約37msだった。held_msは押下保持時間であり、入力遅延そのものではない。Gesture/TMOS追加による明確なUI responsiveness低下は、確認した範囲では認められなかった。厳密な追加前後の比較測定や全loop処理の最大時間を示す結果ではない。

## Gesture認識結果と音声診断

ユーザー作成済みのSD上の9WAVを使用した。形式は48kHz・16-bit PCM・stereo。認識結果を発話することで、Serial Monitorを見なくてもセンサーが何と判断したか確認できた。

| Gesture分類 | SD上の音声ファイル |
| --- | --- |
| UP | `/sounds/gesture/up.wav` |
| DOWN | `/sounds/gesture/down.wav` |
| LEFT | `/sounds/gesture/left.wav` |
| RIGHT | `/sounds/gesture/right.wav` |
| FORWARD | `/sounds/gesture/forward.wav` |
| BACKWARD | `/sounds/gesture/backward.wav` |
| CLOCKWISE | `/sounds/gesture/clockwise.wav` |
| COUNTERCLOCKWISE | `/sounds/gesture/counterclockwise.wav` |
| WAVE | `/sounds/gesture/wave.wav` |

例えばLEFTなら「left」、CLOCKWISEなら「clockwise」と発話する。この音声診断自体は正常に機能した。再生中の新しい音声はスキップするため、Serialに残る全イベントが必ず発話されるわけではない。

実機ログ例：

```text
LIGHT: toggle command published
AUDIO: playing /sounds/gesture/clockwise.wav
GESTURE: CLOCKWISE action=executed audio=started

LIGHT: toggle command published
AUDIO: playing /sounds/gesture/counterclockwise.wav
GESTURE: COUNTERCLOCKWISE action=executed audio=started
```

LIGHTログはtoggleコマンドのpublish成功を示す。照明の実状態を返信で確認したログではない。

## false positiveの問題

Gesture分類の誤認識が多く、意図した動作とは異なる分類になる問題があった。ただし、採用判断でより重大だったのは、M5GOを操作する意図がない身体動作や周辺の動きもGestureとして検出された点である。

```text
操作意図のない動作
  → false positive（意図しないGesture event）
  → シーリングライトtoggle
```

9分類を同じ機能へ割り当てたため、LEFTをRIGHTと誤分類するだけなら操作は同じになる。一方、操作意図がない時点でイベントが発生すると、照明を意図せず操作してしまう。実際にこの挙動が観測され、照明入力としての影響が大きく、許容できないと判断した。

評価時間、試行回数、距離別の認識率、分類ごとの誤認識率は今回の報告に含まれないため、定量値は記載しない。

## cooldownの評価

2秒cooldownは、1回の手の動きからRIGHT・LEFT・CLOCKWISEなど複数のイベントが発生した場合に、連続toggleを抑える目的では有効だった。

```text
GESTURE: RIGHT action=executed audio=started
GESTURE: LEFT action=cooldown audio=started
```

対応するPERF例：

```text
gesture_events=6
gesture_actions=5
gesture_cooldown_skips=1
```

しかしcooldownは、操作意図がないときの最初の誤検出を防がない。cooldown外でfalse positiveが発生すれば最初のtoggleは実行されるため、時間を長くすることは根本的な解決策ではないと判断した。

## 評価の整理

| 評価対象 | 結果 |
| --- | --- |
| TMOSとのY字GROVE I²C共存・通信安定性 | 確認した範囲で良好 |
| polling負荷・M5GOへの統合 | 良好。明確なUI応答性低下は未確認 |
| SD WAVによる分類結果の音声診断 | 正常に機能 |
| cooldownによる連続操作抑制 | 有効 |
| Gesture分類品質 | 誤認識が多い |
| 操作意図のない動作の排除 | 不十分。意図しない照明toggleが発生 |
| シーリングライト操作入力としての採用 | 不採用 |

センサー通信・統合の成立と、用途に必要な入力信頼性は別の評価である。PAJ7620U2が他の用途にも不適切であるという一般的な結論は示さない。

## 今後

- シーリングライトの確実な操作は、物理Cボタン長押しを維持する方向とする。
- Gesture UNITは本番EnvMonitorから外す予定とする。本書の記録時点では実装コードの削除は行っていない。
- DLight UNIT（照度センサー）を代替候補として検討する。Gestureのような操作入力ではなく、温度・湿度・TMOS presenceと並ぶ環境情報として照度を扱う方向とする。詳細設計は今回の評価範囲外である。
