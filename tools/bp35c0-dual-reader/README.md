# bp35c0-dual-reader

2枚のBP35C0(Cube J1から取り外した基板)を1個のESP32-C3に接続し、両方に自動でBルート
JOINして瞬時電力・瞬時電流・積算電力量を定期的に取り続ける常駐ファーム。配線は
[bp35c0-dual-probe](../bp35c0-dual-probe/)と共通。ピン選定理由・配線図はそちらの
README.mdを参照。

インタラクティブに叩きたいだけならbp35c0-dual-probe、放っておいて周期的にデータが欲しいなら
このbp35c0-dual-reader、という使い分け。

## 認証情報はNVSに持たせる(ソースには書かない)

BルートID・パスワードはファームのソースコードに焼き込まず、ESP32のNVS(`Preferences`)に
実機上で書き込む。ビルド成果物やリポジトリに認証情報が残らない。

初回起動時（またはNVSが空の状態）はシリアルモニタから以下を打つ:

```
!setcred 1 電灯 00112233445566778899AABBCCDDEEFF 123456789012
!setcred 2 電力 0123456789ABCDEF0123456789ABCDEF 210987654321
```

`!setcred <1|2> <name> <id> <password>` — `id`は自動で大文字化される(Bルートは大文字
でなければメーターが応答しない。BP35C0.md参照)。書き込むと同時にそのモジュールの
JOINシーケンスが即座に始まる。`name`はログ・`/metrics`のラベルに使う(スペース不可)。
`電灯`/`電力`は低圧契約の種別名(一般家庭用の従量電灯契約、水道給湯器等の低圧電力契約)。

消去は `!clearcred <1|2>`。書き直したい時はこれで一旦消してから`!setcred`し直す。

`name`だけ変えたい場合はid/passwordを打ち直す必要はなく `!rename <1|2> <name>` でよい
(JOIN済みのセッションはそのまま、ログ・`/metrics`のラベルだけ変わる)。

NVSは電源を切っても保持されるので、一度設定すれば以降は自動でJOINまで進む。

## 起動後の流れ

1. 電源投入・リセット時、NVSに認証情報があるモジュールだけ自動でRESETNパルス→
   `SKSETRBID`→`SKSETPWD`→`SKSCAN`(見つからなければduration 5→7で再試行)→
   `SKSREG`→`SKLL64`→`SKJOIN`と進む
2. JOIN成功後、**固定情報を1回だけ**取得する: 係数(0xD3)・積算電力量単位(0xE1)・
   積算電力量有効桁数(0xD7)・メーカコード(0x8A)。これらは変化しないので毎回は取らない
3. 以後30秒間隔で**瞬時電力(0xE7)・瞬時電流(0xE8)・積算電力量(0xE0)**をECHONET Lite
   のGETで取得し、シリアルに1行で出す。積算電力量は固定情報の係数×単位で正しくkWhに
   換算する
4. コマンドがタイムアウトしたりJOINが切れたりしたら、そのモジュールだけをリセットして
   最初からやり直す(もう一方のモジュールには影響しない)

## 出力例

```
bp35c0-dual-reader:
  [1] RX=GPIO4 TX=GPIO5 RESETN=GPIO6
  [1] NVSから認証情報を読み込み済み(denryoku)
  [2] RX=GPIO7 TX=GPIO10 RESETN=GPIO3
  [2] NVSから認証情報を読み込み済み(kyuutouki)
`!setcred <1|2> <name> <id> <password>` で認証情報をNVSに書き込む(idは自動で大文字化)。
`!clearcred <1|2>` で消去。
[1] [reset]
[2] [reset]
[1] 発見: channel=3B panid=8A2F
[2] 発見: channel=3B panid=A7B8
[1] JOIN成功
[2] JOIN成功
[1] denryoku 固定情報: maker=000000 coefficient=1 digits=8 unit=0.001kWh
[2] kyuutouki 固定情報: maker=000000 coefficient=1 digits=6 unit=0.01kWh
[1] denryoku power=330W current=R=0.9A T=4.3A energy=711.282kWh
[2] kyuutouki power=0W current=R=0.0A T=N/A energy=1976.640kWh
```

T相`N/A`は単相2線式のメーター(200V専用の深夜電力契約)であることを示す(BP35C0.md参照)。

## Wi-FiとPrometheus `/metrics`

Wi-Fi認証情報もBルート認証情報と同様、ソースには焼き込まずNVSに持たせる。シリアルモニタから:

```
!setwifi <ssid> <password>
```

で書き込むと即座に接続を開始する。消去は`!clearwifi`。接続に成功するとシリアルに
`Wi-Fi接続成功: IP=... (http://.../metrics)`が出るので、そのIPを家のPrometheusの
scrape対象に加える。SSID・パスワードにスペースは使えない(最初の空白で区切る実装のため)。

ネイティブUSB CDCコンソールはローカルエコーを返さないので、打った文字が見えないように
思えることがある(ファーム側で明示的にエコーバックしているので、実際には反映されている)。

ポート80で`/metrics`をPrometheusテキスト形式で常時提供する(Wi-Fi接続前でも
`server.begin()`自体は実行されるので、繋がった瞬間から応答できる)。認証・TLSは
置いていない。**家庭内LAN限定での利用を前提とする**(tsdb.md参照。VictoriaMetricsへの
プッシュ等、外向きの永続化はまだ未実装で別途検討中)。

メトリクス名はBP35A1向けの既存Go実装(`bin/exporter`)と揃えて`sam0_`始まりにしている。

```
sam0_instantaneous_power_watt{meter="電灯"} 330
sam0_instantaneous_current_ampere{meter="電灯",phase="r"} 0.9
sam0_instantaneous_current_ampere{meter="電灯",phase="t"} 4.3
sam0_cumulative_energy_kwh_total{meter="電灯"} 711.282
sam0_last_reading_seconds{meter="電灯"} 12.3
```

- `phase="t"`は単相2線式のメーター(T相「該当なし」)では出力されない。欠落自体が判別情報になる
- `sam0_cumulative_energy_kwh_total`は積算電力量(E0)そのものなので単調増加カウンタ。
  `rate()`/`increase()`がそのまま効く
- `sam0_last_reading_seconds`は最後に取得できてからの経過秒数。これが伸び続けていたら
  そのモジュールがJOINからやり直している最中、または詰まっている

## 定期取得の間隔・タイムアウトを変えたい場合

`src/main.cpp`冒頭の定数を書き換える。

| 定数 | 意味 | 既定値 |
| --- | --- | --- |
| `kPollIntervalMs` | 定期取得の間隔 | 30秒 |
| `kGetResponseTimeoutMs` | GET応答を待つ上限 | 10秒 |
| `kMaxGetFailuresBeforeReset` | 連続失敗何回でJOINからやり直すか | 3回 |
| `kMinScanDuration`/`kMaxScanDuration` | SKSCANのduration探索範囲 | 5〜7 |

## 使い方

```sh
source ~/.platformio/penv/bin/activate
pio run -t upload
pio device monitor
```

## 未実装(今のところ)

Wi-Fi接続と`/metrics`でのPrometheus pull対応は実装済み(上記参照)。ただし**家に既にある
Prometheusからscrapeしてもらう前提**で認証・TLSは置いていないので、家庭内LAN限定で使うこと。

長期保存・外部可視化(VictoriaMetricsへのpush、保持期間を伸ばす等)はまだ方針段階
(`tsdb.md`参照)。今のscrape方式はあくまで「一旦家に入れる」ための最短経路で、
年単位保存が要るなら既存Prometheusからのremote_write等を別途検討する。
