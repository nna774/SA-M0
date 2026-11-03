#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <cstring>
#include <cstdlib>
#include <vector>

// 2枚のBP35C0(モジュール1=Serial1/UART1, モジュール2=Serial0/UART0)にそれぞれ独立に
// BルートJOINし、JOIN後は固定情報(係数/積算電力量単位/有効桁数/メーカコード)を1回だけ、
// 以後は瞬時電力/瞬時電流/積算電力量を一定間隔でECHONET LiteのGETで取り続ける常駐ファーム。
//
// 認証情報はソースに焼き込まずNVS(Preferences)に持たせる。起動時に未設定なら
// `!setcred <1|2> <name> <id> <password>` をシリアルモニタに打ち込んで書き込む。
//
// 配線・ピン選定理由はbp35c0-dual-probeと共通。README.md参照。

namespace {

constexpr uint32_t kBaud = 115200;
constexpr uint32_t kCommandTimeoutMs = 6000;
constexpr uint32_t kJoinTimeoutMs = 20000;
constexpr uint32_t kGetResponseTimeoutMs = 10000;
constexpr uint32_t kPollIntervalMs = 30000;
constexpr int kMinScanDuration = 5;
constexpr int kMaxScanDuration = 7;
constexpr int kMaxGetFailuresBeforeReset = 3;

Preferences prefs;

enum class State {
  kNotProvisioned,
  kSetRbid,
  kSetPwd,
  kScanAck,
  kScanEvents,
  kSetChannel,
  kSetPanId,
  kLl64,
  kJoinAck,
  kJoinEvents,
  kFixedInfoWait,
  kReady,
  kWaitGetResponse,
};

struct Credential {
  String name;
  String id;
  String password;
  bool valid = false;
};

struct ScanResult {
  String channel;
  String panId;
  String addr;
};

// SKモジュールのUARTを読み、テキスト行 or ERXUDPの生バイトフレームを1件ずつ切り出す。
// ERXUDPのデータ部は生バイトで来て0x0Aや0x0Dが平然と混ざる(BP35C0.md参照)ので、
// 行として読まずDATALENぶんをそのままバイト単位で読み切る。
class SkReader {
 public:
  explicit SkReader(HardwareSerial &serial) : serial_(serial) {}

  bool poll() {
    while (serial_.available()) {
      const uint8_t b = serial_.read();
      if (mode_ == Mode::kData) {
        data_.push_back(b);
        if (data_.size() >= dataLen_) {
          finish(true);
          return true;
        }
        continue;
      }
      if (b == '\r') {
        continue;
      }
      if (b == '\n') {
        finish(false);
        return true;
      }
      header_ += char(b);
      if (b == ' ') {
        spaceCount_++;
        // ERXUDP <SENDER> <DEST> <RPORT> <LPORT> <SENDERLLA> <SECURED> <SIDE> <DATALEN> <生バイト>
        // 9個目の空白の直後から生バイトが始まる(BP35C0はside引数が挟まるぶんBP35A1より1個多い)。
        if (spaceCount_ == 9 && header_.startsWith("ERXUDP ")) {
          const String withoutTrailingSpace = header_.substring(0, header_.length() - 1);
          const int lastSpace = withoutTrailingSpace.lastIndexOf(' ');
          dataLen_ = strtoul(withoutTrailingSpace.substring(lastSpace + 1).c_str(), nullptr, 16);
          data_.clear();
          data_.reserve(dataLen_);
          mode_ = Mode::kData;
        }
      }
    }
    return false;
  }

  const String &line() const { return line_; }
  bool isErxudp() const { return isErxudp_; }
  const std::vector<uint8_t> &data() const { return data_; }

 private:
  enum class Mode { kLine, kData };

  void finish(bool erxudp) {
    line_ = header_;
    isErxudp_ = erxudp;
    header_ = "";
    spaceCount_ = 0;
    mode_ = Mode::kLine;
  }

  HardwareSerial &serial_;
  Mode mode_ = Mode::kLine;
  String header_;
  int spaceCount_ = 0;
  size_t dataLen_ = 0;
  std::vector<uint8_t> data_;
  String line_;
  bool isErxudp_ = false;
};

struct Module {
  HardwareSerial &serial;
  const int rxPin;
  const int txPin;
  const int resetPin;
  const char tag;
  SkReader reader;

  Credential credential;
  State state = State::kNotProvisioned;
  uint32_t deadline = 0;
  uint32_t nextPollAt = 0;
  int scanDuration = kMinScanDuration;
  ScanResult scan;
  String ipv6;
  bool collectingEpandesc = false;
  int consecutiveGetFailures = 0;
  double coefficient = 1.0;
  double unit = 0.001;
  int digits = 8;
  uint8_t makerCode[3] = {0, 0, 0};

  // /metrics用に最後に取れた値を保持する。
  bool hasReading = false;
  int32_t lastPowerW = 0;
  double lastCurrentR = 0;
  bool hasCurrentT = false;
  double lastCurrentT = 0;
  double lastEnergyKwh = 0;
  uint32_t lastReadingAtMs = 0;
};

Module modules[] = {
    {Serial1, BP35C0_1_RX_PIN, BP35C0_1_TX_PIN, BP35C0_1_RESET_PIN, '1', SkReader(Serial1)},
    {Serial0, BP35C0_2_RX_PIN, BP35C0_2_TX_PIN, BP35C0_2_RESET_PIN, '2', SkReader(Serial0)},
};

String hostLine;

// Wi-Fi認証情報もNVS(Preferences)に持たせる。ソースには焼き込まない(認証情報の方針参照)。
WebServer server(80);
String wifiSsid, wifiPassword;
bool wifiConfigured = false;
bool wifiWasConnected = false;
uint32_t nextWifiAttemptAt = 0;
constexpr uint32_t kWifiRetryIntervalMs = 10000;

Module *findModule(char tag) {
  for (auto &m : modules) {
    if (m.tag == tag) {
      return &m;
    }
  }
  return nullptr;
}

void log(char tag, const String &msg) {
  Serial.printf("[%c] %s\r\n", tag, msg.c_str());
}

// RESETNをオープンドレイン相当で叩く。Highへは駆動せず、基板上の100kΩプルアップに戻させる。
void pulseReset(Module &m) {
  pinMode(m.resetPin, OUTPUT);
  digitalWrite(m.resetPin, LOW);
  delay(10);
  pinMode(m.resetPin, INPUT);
  log(m.tag, "[reset]");
  delay(500);
}

void sendCommand(Module &m, const String &command) {
  Serial.printf("[%c] --> %s\r\n", m.tag, command.c_str());
  m.serial.printf("%s\r\n", command.c_str());
}

size_t buildGetFrame(uint8_t *out, uint8_t tid, const uint8_t *epcs, size_t count) {
  size_t i = 0;
  out[i++] = 0x10;
  out[i++] = 0x81;
  out[i++] = 0x00;
  out[i++] = tid;
  out[i++] = 0x05;
  out[i++] = 0xFF;
  out[i++] = 0x01;  // SEOJ: コントローラ
  out[i++] = 0x02;
  out[i++] = 0x88;
  out[i++] = 0x01;  // DEOJ: 低圧スマート電力量メータ
  out[i++] = 0x62;  // ESV: Get
  out[i++] = uint8_t(count);  // OPC
  for (size_t k = 0; k < count; k++) {
    out[i++] = epcs[k];
    out[i++] = 0x00;
  }
  return i;
}

// SKSENDTOはASCIIヘッダ+生バイトを継ぎ目なく送る必要がある。間にCRLFを挟むとデータ長の
// 境目がずれ、モジュールがエラーも返さず固まる(BP35C0.md参照)ので、ヘッダ文字列と
// バイナリフレームを別々のwrite()で続けて流す(ホスト側の行バッファリングを経由しない)。
void sendGetRequest(Module &m, const uint8_t *epcs, size_t count) {
  uint8_t frame[32];
  const size_t len = buildGetFrame(frame, uint8_t(m.tag - '0'), epcs, count);
  // ヘッダ文字列は "SKSENDTO 1 " + IPv6(最大39文字) + " 0E1A 1 0 XXXX " で最大65文字要る。
  // 64バイトだとsnprintfに切り詰められてSKSENDTOが壊れる(実機で踏んだ)。
  char header[96];
  snprintf(header, sizeof(header), "SKSENDTO 1 %s 0E1A 1 0 %04X ", m.ipv6.c_str(), unsigned(len));
  Serial.printf("[%c] --> SKSENDTO (GET %u props)\r\n", m.tag, unsigned(count));
  m.serial.write(reinterpret_cast<const uint8_t *>(header), strlen(header));
  m.serial.write(frame, len);
}

void beginPolling(Module &m) {
  m.state = State::kReady;
  m.nextPollAt = millis();  // 起動直後の1回はすぐ取りに行く
}

void applyFixedInfo(Module &m, const std::vector<uint8_t> &data) {
  const uint8_t opc = data[11];
  size_t i = 12;
  for (uint8_t p = 0; p < opc && i + 1 < data.size(); p++) {
    const uint8_t epc = data[i];
    const uint8_t pdc = data[i + 1];
    const uint8_t *edt = data.data() + i + 2;
    if (epc == 0xD3 && pdc == 4) {
      // 係数。0(未設定)は「係数なし」として扱う
      const uint32_t v = (uint32_t(edt[0]) << 24) | (uint32_t(edt[1]) << 16) | (uint32_t(edt[2]) << 8) | edt[3];
      m.coefficient = (v == 0) ? 1.0 : double(v);
    } else if (epc == 0xD7 && pdc == 1) {
      m.digits = edt[0];
    } else if (epc == 0xE1 && pdc == 1) {
      static const double kUnitTable[] = {1, 0.1, 0.01, 0.001, 0.0001, 0, 0, 0, 0, 0, 10, 100, 1000, 10000};
      if (edt[0] < sizeof(kUnitTable) / sizeof(kUnitTable[0]) && kUnitTable[edt[0]] > 0) {
        m.unit = kUnitTable[edt[0]];
      }
    } else if (epc == 0x8A && pdc == 3) {
      memcpy(m.makerCode, edt, 3);
    }
    i += 2 + pdc;
  }
  Serial.printf("[%c] %s 固定情報: maker=%02X%02X%02X coefficient=%g digits=%d unit=%gkWh\r\n",
                m.tag, m.credential.name.c_str(), m.makerCode[0], m.makerCode[1], m.makerCode[2],
                m.coefficient, m.digits, m.unit);
}

void reportReading(Module &m, const std::vector<uint8_t> &data) {
  const uint8_t opc = data[11];
  size_t i = 12;
  String powerStr = "?", currentStr = "?", energyStr = "?";
  for (uint8_t p = 0; p < opc && i + 1 < data.size(); p++) {
    const uint8_t epc = data[i];
    const uint8_t pdc = data[i + 1];
    const uint8_t *edt = data.data() + i + 2;
    if (epc == 0xE7 && pdc == 4) {
      const int32_t v = int32_t((uint32_t(edt[0]) << 24) | (uint32_t(edt[1]) << 16) | (uint32_t(edt[2]) << 8) | edt[3]);
      powerStr = String(v) + "W";
      m.lastPowerW = v;
    } else if (epc == 0xE8 && pdc == 4) {
      const int16_t r = int16_t((edt[0] << 8) | edt[1]);
      const int16_t t = int16_t((edt[2] << 8) | edt[3]);
      // T相 0x7FFEは「該当なし」(単相2線式)。BP35C0.mdの実測記録を参照。
      const bool tValid = uint16_t(t) != 0x7FFE;
      const String tStr = tValid ? String(t * 0.1, 1) + "A" : "N/A";
      currentStr = "R=" + String(r * 0.1, 1) + "A T=" + tStr;
      m.lastCurrentR = r * 0.1;
      m.hasCurrentT = tValid;
      m.lastCurrentT = tValid ? t * 0.1 : 0;
    } else if (epc == 0xE0 && pdc == 4) {
      const uint32_t v = (uint32_t(edt[0]) << 24) | (uint32_t(edt[1]) << 16) | (uint32_t(edt[2]) << 8) | edt[3];
      const double kwh = v * m.coefficient * m.unit;
      energyStr = String(kwh, 3) + "kWh";
      m.lastEnergyKwh = kwh;
    }
    i += 2 + pdc;
  }
  m.hasReading = true;
  m.lastReadingAtMs = millis();
  Serial.printf("[%c] %s power=%s current=%s energy=%s\r\n",
                m.tag, m.credential.name.c_str(), powerStr.c_str(), currentStr.c_str(), energyStr.c_str());
}

void resetAndRestart(Module &m) {
  pulseReset(m);
  m.scanDuration = kMinScanDuration;
  m.consecutiveGetFailures = 0;
  sendCommand(m, "SKSETRBID " + m.credential.id);
  m.state = State::kSetRbid;
  m.deadline = millis() + kCommandTimeoutMs;
}

void startScan(Module &m) {
  sendCommand(m, "SKSCAN 2 FFFFFFFF " + String(m.scanDuration) + " 0");
  m.scan = ScanResult();
  m.collectingEpandesc = false;
  m.state = State::kScanAck;
  m.deadline = millis() + kCommandTimeoutMs;
}

void finishPoll(Module &m, bool success) {
  if (success) {
    m.consecutiveGetFailures = 0;
  } else {
    m.consecutiveGetFailures++;
    if (m.consecutiveGetFailures >= kMaxGetFailuresBeforeReset) {
      log(m.tag, "GET失敗が連続、再JOINする");
      resetAndRestart(m);
      return;
    }
  }
  m.state = State::kReady;
  m.nextPollAt = millis() + kPollIntervalMs;
}

void handleEvent(Module &m) {
  const bool isFrame = m.reader.isErxudp();
  const String &line = m.reader.line();

  if (m.state == State::kWaitGetResponse) {
    if (isFrame) {
      const std::vector<uint8_t> &data = m.reader.data();
      if (data.size() >= 11 && data[4] == 0x02 && data[5] == 0x88 && data[6] == 0x01) {
        const uint8_t esv = data[10];
        if (esv == 0x72) {
          reportReading(m, data);
          finishPoll(m, true);
        } else if (esv == 0x52) {
          log(m.tag, "Get_SNA(取得失敗)");
          finishPoll(m, false);
        }
        // それ以外のESVは無視して待ち続ける
      }
      // ノードプロファイル(0EF001)のINF自発通知等はSEOJが合わず無視して待ち続ける
      return;
    }
    if (line.startsWith("FAIL")) {
      log(m.tag, "SKSENDTO failed: " + line);
      finishPoll(m, false);
    }
    return;
  }

  if (m.state == State::kFixedInfoWait) {
    if (isFrame) {
      const std::vector<uint8_t> &data = m.reader.data();
      if (data.size() >= 11 && data[4] == 0x02 && data[5] == 0x88 && data[6] == 0x01 &&
          (data[10] == 0x72 || data[10] == 0x52)) {
        applyFixedInfo(m, data);  // Get_SNAでも来たぶんだけベストエフォートで拾う
        beginPolling(m);
      }
      return;
    }
    if (line.startsWith("FAIL")) {
      log(m.tag, "固定情報の取得に失敗、デフォルト値のまま進める: " + line);
      beginPolling(m);
    }
    return;
  }

  if (isFrame) {
    return;  // 待ってない時のERXUDPは無視する
  }

  switch (m.state) {
    case State::kNotProvisioned:
    case State::kReady:
      break;

    case State::kSetRbid:
      if (line == "OK") {
        sendCommand(m, "SKSETPWD C " + m.credential.password);
        m.state = State::kSetPwd;
        m.deadline = millis() + kCommandTimeoutMs;
      } else if (line.startsWith("FAIL")) {
        log(m.tag, "SKSETRBID failed: " + line);
        resetAndRestart(m);
      }
      break;

    case State::kSetPwd:
      if (line == "OK") {
        startScan(m);
      } else if (line.startsWith("FAIL")) {
        log(m.tag, "SKSETPWD failed: " + line);
        resetAndRestart(m);
      }
      break;

    case State::kScanAck:
      if (line == "OK") {
        m.state = State::kScanEvents;
        m.deadline = millis() + (uint32_t(m.scanDuration) + 1) * 2000UL + 8000UL;
      } else if (line.startsWith("FAIL")) {
        log(m.tag, "SKSCAN failed: " + line);
        resetAndRestart(m);
      }
      break;

    case State::kScanEvents:
      if (line.startsWith("EVENT 22")) {
        if (m.scan.panId.isEmpty()) {
          log(m.tag, "見つからず、durationを上げて再試行");
          if (m.scanDuration < kMaxScanDuration) {
            m.scanDuration++;
          }
          startScan(m);
        } else {
          log(m.tag, "発見: channel=" + m.scan.channel + " panid=" + m.scan.panId);
          sendCommand(m, "SKSREG S2 " + m.scan.channel);
          m.state = State::kSetChannel;
          m.deadline = millis() + kCommandTimeoutMs;
        }
      } else if (line == "EPANDESC") {
        m.collectingEpandesc = true;
      } else if (m.collectingEpandesc) {
        const int colon = line.indexOf(':');
        if (colon > 0) {
          String key = line.substring(0, colon);
          String value = line.substring(colon + 1);
          key.trim();
          value.trim();
          if (key == "Channel") {
            m.scan.channel = value;
          } else if (key == "Pan ID") {
            m.scan.panId = value;
          } else if (key == "Addr") {
            m.scan.addr = value;
          }
        }
      }
      break;

    case State::kSetChannel:
      if (line == "OK") {
        sendCommand(m, "SKSREG S3 " + m.scan.panId);
        m.state = State::kSetPanId;
        m.deadline = millis() + kCommandTimeoutMs;
      } else if (line.startsWith("FAIL")) {
        log(m.tag, "SKSREG S2 failed: " + line);
        resetAndRestart(m);
      }
      break;

    case State::kSetPanId:
      if (line == "OK") {
        sendCommand(m, "SKLL64 " + m.scan.addr);
        m.state = State::kLl64;
        m.deadline = millis() + kCommandTimeoutMs;
      } else if (line.startsWith("FAIL")) {
        log(m.tag, "SKSREG S3 failed: " + line);
        resetAndRestart(m);
      }
      break;

    case State::kLl64:
      if (line.startsWith("SKLL64") || line.isEmpty()) {
        break;  // 自分が送ったコマンドのエコーバック
      }
      m.ipv6 = line;
      log(m.tag, "IPv6=" + m.ipv6);
      sendCommand(m, "SKJOIN " + m.ipv6);
      m.state = State::kJoinAck;
      m.deadline = millis() + kCommandTimeoutMs;
      break;

    case State::kJoinAck:
      if (line == "OK") {
        m.state = State::kJoinEvents;
        m.deadline = millis() + kJoinTimeoutMs;
      } else if (line.startsWith("FAIL")) {
        log(m.tag, "SKJOIN failed: " + line);
        resetAndRestart(m);
      }
      break;

    case State::kJoinEvents:
      if (line.startsWith("EVENT 25")) {
        log(m.tag, "JOIN成功");
        static const uint8_t kFixedEpcs[] = {0xD3, 0xD7, 0xE1, 0x8A};
        sendGetRequest(m, kFixedEpcs, 4);
        m.state = State::kFixedInfoWait;
        m.deadline = millis() + kGetResponseTimeoutMs;
      } else if (line.startsWith("EVENT 24")) {
        log(m.tag, "JOIN失敗(EVENT 24)");
        resetAndRestart(m);
      }
      break;

    default:
      break;
  }
}

// sam0_* の命名はbin/exporter(BP35A1向けGo実装)と揃える。existing Prometheusがそのまま
// scrapeできることを優先し、認証・TLSは置かない(家庭内LAN限定での利用を前提とする)。
void handleMetrics() {
  String out;
  out.reserve(1024);
  const uint32_t now = millis();

  out += "# HELP sam0_instantaneous_power_watt instantaneous power (W)\n";
  out += "# TYPE sam0_instantaneous_power_watt gauge\n";
  for (auto &m : modules) {
    if (!m.hasReading) continue;
    out += "sam0_instantaneous_power_watt{meter=\"" + m.credential.name + "\"} " + String(m.lastPowerW) + "\n";
  }

  out += "# HELP sam0_instantaneous_current_ampere instantaneous current (A)\n";
  out += "# TYPE sam0_instantaneous_current_ampere gauge\n";
  for (auto &m : modules) {
    if (!m.hasReading) continue;
    out += "sam0_instantaneous_current_ampere{meter=\"" + m.credential.name + "\",phase=\"r\"} " +
           String(m.lastCurrentR, 1) + "\n";
    if (m.hasCurrentT) {
      // T相「該当なし」(単相2線式)のメーターは出力しない。欠落自体が判別情報(BP35C0.md参照)。
      out += "sam0_instantaneous_current_ampere{meter=\"" + m.credential.name + "\",phase=\"t\"} " +
             String(m.lastCurrentT, 1) + "\n";
    }
  }

  // E0は単調増加カウンタなのでrate()/increase()が効く(tsdb.md参照)。
  out += "# HELP sam0_cumulative_energy_kwh_total cumulative energy (kWh)\n";
  out += "# TYPE sam0_cumulative_energy_kwh_total counter\n";
  for (auto &m : modules) {
    if (!m.hasReading) continue;
    out += "sam0_cumulative_energy_kwh_total{meter=\"" + m.credential.name + "\"} " +
           String(m.lastEnergyKwh, 3) + "\n";
  }

  out += "# HELP sam0_last_reading_seconds seconds since the last successful reading\n";
  out += "# TYPE sam0_last_reading_seconds gauge\n";
  for (auto &m : modules) {
    if (!m.hasReading) continue;
    out += "sam0_last_reading_seconds{meter=\"" + m.credential.name + "\"} " +
           String((now - m.lastReadingAtMs) / 1000.0, 1) + "\n";
  }

  server.send(200, "text/plain; version=0.0.4", out);
}

void loadWifiCredential() {
  wifiSsid = prefs.getString("wifiSsid", "");
  wifiPassword = prefs.getString("wifiPass", "");
  wifiConfigured = !wifiSsid.isEmpty();
}

void startWifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  nextWifiAttemptAt = millis() + kWifiRetryIntervalMs;
  Serial.printf("Wi-Fi接続開始: %s\r\n", wifiSsid.c_str());
}

// Wi-Fi接続状況を監視し、切断時は一定間隔で再接続を試みる。
void pollWifi() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    Serial.printf("Wi-Fi接続成功: IP=%s (http://%s/metrics)\r\n",
                  WiFi.localIP().toString().c_str(), WiFi.localIP().toString().c_str());
  } else if (!connected && wifiWasConnected) {
    Serial.println("Wi-Fi切断、再接続を試みる");
  }
  wifiWasConnected = connected;

  if (!connected && wifiConfigured && millis() >= nextWifiAttemptAt) {
    startWifiConnect();
  }
}

void loadCredential(Module &m) {
  const String prefix = String(m.tag);
  m.credential.name = prefs.getString((prefix + "name").c_str(), "");
  m.credential.id = prefs.getString((prefix + "id").c_str(), "");
  m.credential.password = prefs.getString((prefix + "pw").c_str(), "");
  m.credential.valid = !m.credential.id.isEmpty() && !m.credential.password.isEmpty();
}

void saveCredential(Module &m, const String &name, const String &id, const String &password) {
  const String prefix = String(m.tag);
  prefs.putString((prefix + "name").c_str(), name);
  prefs.putString((prefix + "id").c_str(), id);
  prefs.putString((prefix + "pw").c_str(), password);
  m.credential = {name, id, password, true};
}

void clearCredential(Module &m) {
  const String prefix = String(m.tag);
  prefs.remove((prefix + "name").c_str());
  prefs.remove((prefix + "id").c_str());
  prefs.remove((prefix + "pw").c_str());
  m.credential = Credential();
  m.state = State::kNotProvisioned;
}

void printProvisionStatus(Module &m) {
  if (m.credential.valid) {
    Serial.printf("[%c] NVSから認証情報を読み込み済み(%s)\r\n", m.tag, m.credential.name.c_str());
  } else {
    Serial.printf("[%c] 未設定。`!setcred %c <name> <id> <password>` で書き込め\r\n", m.tag, m.tag);
  }
}

void handleHostLine(const String &line) {
  static const char kSetCred[] = "!setcred ";
  static const char kClearCred[] = "!clearcred ";
  static const char kSetWifi[] = "!setwifi ";
  static const char kClearWifi[] = "!clearwifi";

  if (line.startsWith(kSetWifi)) {
    const String rest = line.substring(strlen(kSetWifi));
    const int sp = rest.indexOf(' ');
    if (sp < 0) {
      Serial.println("usage: !setwifi <ssid> <password>");
      return;
    }
    wifiSsid = rest.substring(0, sp);
    wifiPassword = rest.substring(sp + 1);
    prefs.putString("wifiSsid", wifiSsid);
    prefs.putString("wifiPass", wifiPassword);
    wifiConfigured = true;
    Serial.println("Wi-Fi設定をNVSに保存した。接続を開始する");
    startWifiConnect();
    return;
  }

  if (line == kClearWifi) {
    prefs.remove("wifiSsid");
    prefs.remove("wifiPass");
    wifiSsid = "";
    wifiPassword = "";
    wifiConfigured = false;
    WiFi.disconnect(true, true);
    Serial.println("Wi-Fi設定をNVSから消去した");
    return;
  }

  if (line.startsWith(kSetCred)) {
    const String rest = line.substring(strlen(kSetCred));
    const int sp1 = rest.indexOf(' ');
    const int sp2 = sp1 < 0 ? -1 : rest.indexOf(' ', sp1 + 1);
    const int sp3 = sp2 < 0 ? -1 : rest.indexOf(' ', sp2 + 1);
    if (sp1 < 0 || sp2 < 0 || sp3 < 0) {
      Serial.println("usage: !setcred <1|2> <name> <id> <password>");
      return;
    }
    Module *m = findModule(rest.charAt(0));
    if (m == nullptr) {
      Serial.println("unknown module");
      return;
    }
    const String name = rest.substring(sp1 + 1, sp2);
    String id = rest.substring(sp2 + 1, sp3);
    id.toUpperCase();  // Bルートは大文字でなければメーターが応答しない(BP35C0.md参照)
    const String password = rest.substring(sp3 + 1);
    saveCredential(*m, name, id, password);
    Serial.printf("[%c] 認証情報をNVSに保存した。JOINを開始する\r\n", m->tag);
    resetAndRestart(*m);
    return;
  }

  if (line.startsWith(kClearCred)) {
    Module *m = findModule(line.charAt(strlen(kClearCred)));
    if (m == nullptr) {
      Serial.println("unknown module");
      return;
    }
    clearCredential(*m);
    Serial.printf("[%c] 認証情報をNVSから消去した\r\n", m->tag);
    return;
  }

  Serial.println("unknown command. `!setcred <1|2> <name> <id> <password>` / `!clearcred <1|2>` / "
                  "`!setwifi <ssid> <password>` / `!clearwifi`");
}

}  // namespace

void setup() {
  Serial.begin(kBaud);
  const uint32_t startedAt = millis();
  while (!Serial && millis() - startedAt < 3000) {
    delay(10);
  }

  prefs.begin("bp35c0", false);

  for (auto &m : modules) {
    m.serial.begin(kBaud, SERIAL_8N1, m.rxPin, m.txPin);
    loadCredential(m);
  }
  loadWifiCredential();

  Serial.println("bp35c0-dual-reader:");
  for (auto &m : modules) {
    Serial.printf("  [%c] RX=GPIO%d TX=GPIO%d RESETN=GPIO%d\r\n", m.tag, m.rxPin, m.txPin, m.resetPin);
    printProvisionStatus(m);
  }
  Serial.println("`!setcred <1|2> <name> <id> <password>` で認証情報をNVSに書き込む(idは自動で大文字化)。");
  Serial.println("`!clearcred <1|2>` で消去。");
  if (wifiConfigured) {
    Serial.printf("Wi-Fi: NVSから読み込み済み(SSID=%s)\r\n", wifiSsid.c_str());
  } else {
    Serial.println("Wi-Fi: 未設定。`!setwifi <ssid> <password>` で書き込め");
  }

  for (auto &m : modules) {
    if (m.credential.valid) {
      resetAndRestart(m);
    }
  }

  // Wi-Fi未接続でもserver.begin()自体は害がない。接続後にクライアントから届くようになる。
  server.on("/metrics", handleMetrics);
  server.begin();
  if (wifiConfigured) {
    startWifiConnect();
  }
}

void loop() {
  pollWifi();
  server.handleClient();

  while (Serial.available()) {
    const char c = Serial.read();
    if (c != '\r' && c != '\n') {
      hostLine += c;
      continue;
    }
    if (!hostLine.isEmpty()) {
      handleHostLine(hostLine);
      hostLine = "";
    }
  }

  const uint32_t now = millis();
  for (auto &m : modules) {
    while (m.reader.poll()) {
      handleEvent(m);
    }

    if (m.state == State::kNotProvisioned) {
      continue;
    }
    if (m.state == State::kReady) {
      if (now >= m.nextPollAt) {
        static const uint8_t kPollEpcs[] = {0xE7, 0xE8, 0xE0};
        sendGetRequest(m, kPollEpcs, 3);
        m.state = State::kWaitGetResponse;
        m.deadline = now + kGetResponseTimeoutMs;
      }
      continue;
    }
    if (now > m.deadline) {
      switch (m.state) {
        case State::kFixedInfoWait:
          log(m.tag, "固定情報の取得がタイムアウト、デフォルト値のまま進める");
          beginPolling(m);
          break;
        case State::kWaitGetResponse:
          log(m.tag, "GET応答がタイムアウト");
          finishPoll(m, false);
          break;
        default:
          log(m.tag, "タイムアウト、リセットしてやり直す");
          resetAndRestart(m);
          break;
      }
    }
  }
}
