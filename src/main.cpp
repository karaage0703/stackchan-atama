/**
 * stackchan-atama - アタマだけスタックチャン
 *
 * M5Stack単体で動く顔+音声スタックチャン（サーボ不要）
 * WiFi HTTP API で外部から制御可能
 *
 * Based on the work of:
 *   - robo8080 (AI_StackChan2, M5Unified_StackChan)
 *   - meganetaaan / lovyan03 (m5stack-avatar, M5Unified)
 *   - stack-chan community
 */

#include <M5Unified.h>
#include <Avatar.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <AudioGeneratorWAV.h>
#include <AudioFileSourceBuffer.h>
#include <AudioOutputI2S.h>
#include <SD.h>

// ---- Configuration ----
// WiFi credentials: set via Serial or SD card
String wifi_ssid = "";
String wifi_pass = "";

// ---- Audio ----
static constexpr uint8_t m5spk_virtual_channel = 0;

// Custom AudioOutput for M5Speaker (based on robo8080's implementation)
class AudioOutputM5Speaker : public AudioOutput {
public:
  AudioOutputM5Speaker(m5::Speaker_Class* speaker, uint8_t virtual_ch = 0) {
    _speaker = speaker;
    _virtual_ch = virtual_ch;
    _tri_index = 0;
    _tri_filled = 0;
  }

  bool begin() override {
    _tri_index = 0;
    _tri_filled = 0;
    return true;
  }

  bool ConsumeSample(int16_t sample[2]) override {
    // Mono mix
    int16_t mono = (sample[0] + sample[1]) / 2;
    _tri_buffer[_tri_index][_tri_filled++] = mono;

    if (_tri_filled >= TRI_BUF_SIZE) {
      flush();
    }
    return true;
  }

  void flush() {
    if (_tri_filled > 0) {
      _speaker->playRaw(_tri_buffer[_tri_index], _tri_filled, hertz, false, 1, _virtual_ch);
      _tri_index = (_tri_index + 1) % 3;
      _tri_filled = 0;
    }
  }

  bool stop() override {
    flush();
    return true;
  }

  // Get current audio level for lip sync
  int16_t getLevel() {
    int idx = (_tri_index + 2) % 3;  // previous buffer
    int32_t sum = 0;
    for (int i = 0; i < TRI_BUF_SIZE; i++) {
      sum += abs(_tri_buffer[idx][i]);
    }
    return sum / TRI_BUF_SIZE;
  }

private:
  static constexpr int TRI_BUF_SIZE = 640;
  m5::Speaker_Class* _speaker;
  uint8_t _virtual_ch;
  int16_t _tri_buffer[3][TRI_BUF_SIZE] = {};
  uint8_t _tri_index;
  size_t _tri_filled;
};

// ---- Globals ----
using namespace m5avatar;
Avatar avatar;
WebServer server(80);
AudioOutputM5Speaker* audioOut = nullptr;

// WAV playback state
volatile bool wav_playing = false;
uint8_t* wav_data = nullptr;
size_t wav_data_len = 0;

// ---- Expression map ----
Expression getExpression(const String& name) {
  if (name == "happy" || name == "1") return Expression::Happy;
  if (name == "sleepy" || name == "2") return Expression::Sleepy;
  if (name == "doubt" || name == "3") return Expression::Doubt;
  if (name == "sad" || name == "4") return Expression::Sad;
  if (name == "angry" || name == "5") return Expression::Angry;
  return Expression::Neutral;
}

String getExpressionName(Expression expr) {
  switch (expr) {
    case Expression::Happy: return "happy";
    case Expression::Sleepy: return "sleepy";
    case Expression::Doubt: return "doubt";
    case Expression::Sad: return "sad";
    case Expression::Angry: return "angry";
    default: return "neutral";
  }
}

// ---- AudioFileSource from memory buffer ----
class AudioFileSourceMemory : public AudioFileSource {
public:
  AudioFileSourceMemory(const uint8_t* data, size_t len) {
    _data = data;
    _len = len;
    _pos = 0;
  }

  bool isOpen() override { return _data != nullptr; }
  bool open(const char* url) override { return false; }
  bool close() override { _data = nullptr; return true; }
  uint32_t getSize() override { return _len; }
  uint32_t getPos() override { return _pos; }
  bool seek(int32_t pos, int dir) override {
    if (dir == SEEK_SET) _pos = pos;
    else if (dir == SEEK_CUR) _pos += pos;
    else if (dir == SEEK_END) _pos = _len + pos;
    if (_pos > _len) _pos = _len;
    return true;
  }
  uint32_t read(void* data, uint32_t len) override {
    uint32_t remain = _len - _pos;
    uint32_t toRead = (len < remain) ? len : remain;
    memcpy(data, _data + _pos, toRead);
    _pos += toRead;
    return toRead;
  }

private:
  const uint8_t* _data;
  size_t _len;
  size_t _pos;
};

// ---- WAV playback task ----
void wavPlayTask(void* param) {
  while (true) {
    if (wav_data != nullptr && wav_data_len > 0 && !wav_playing) {
      wav_playing = true;

      AudioFileSourceMemory* src = new AudioFileSourceMemory(wav_data, wav_data_len);
      AudioGeneratorWAV* wav = new AudioGeneratorWAV();

      if (wav->begin(src, audioOut)) {
        while (wav->isRunning()) {
          if (!wav->loop()) break;

          // Lip sync
          float level = (float)audioOut->getLevel() / 5000.0f;
          if (level > 1.0f) level = 1.0f;
          avatar.setMouthOpenRatio(level);

          vTaskDelay(1);
        }
        wav->stop();
      }

      avatar.setMouthOpenRatio(0.0f);
      delete wav;
      delete src;

      // Free WAV data
      free(wav_data);
      wav_data = nullptr;
      wav_data_len = 0;
      wav_playing = false;
    }
    vTaskDelay(10);
  }
}

// ---- HTTP Handlers ----
void handleRoot() {
  server.send(200, "text/plain", "hello from stackchan-atama!");
}

void handleStatus() {
  JsonDocument doc;
  doc["status"] = "online";
  doc["ip"] = WiFi.localIP().toString();
  doc["playing"] = wav_playing;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleFace() {
  String expr_str = server.arg("expression");
  if (expr_str.isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"expression parameter required\"}");
    return;
  }

  Expression expr = getExpression(expr_str);
  avatar.setExpression(expr);

  JsonDocument doc;
  doc["status"] = "ok";
  doc["expression"] = getExpressionName(expr);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handlePlayWav() {
  if (wav_playing) {
    server.send(409, "application/json", "{\"error\":\"already playing\"}");
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no wav data in body\"}");
    return;
  }

  String body = server.arg("plain");
  size_t len = body.length();

  if (len < 44) {  // WAV header minimum
    server.send(400, "application/json", "{\"error\":\"data too small for WAV\"}");
    return;
  }

  // Allocate PSRAM for WAV data
  wav_data = (uint8_t*)ps_malloc(len);
  if (wav_data == nullptr) {
    server.send(500, "application/json", "{\"error\":\"memory allocation failed\"}");
    return;
  }

  memcpy(wav_data, body.c_str(), len);
  wav_data_len = len;

  JsonDocument doc;
  doc["status"] = "ok";
  doc["size"] = len;
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleSetting() {
  String vol_str = server.arg("volume");
  if (!vol_str.isEmpty()) {
    int vol = vol_str.toInt();
    if (vol >= 0 && vol <= 255) {
      M5.Speaker.setChannelVolume(m5spk_virtual_channel, vol);
    }
  }

  JsonDocument doc;
  doc["status"] = "ok";
  doc["volume"] = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleNotFound() {
  server.send(404, "application/json", "{\"error\":\"not found\"}");
}

// ---- WiFi Setup ----
void setupWiFi() {
  // Try to read from SD card first
  if (SD.begin(GPIO_NUM_NC, SPI, 25000000)) {
    File f = SD.open("/wifi.txt");
    if (f) {
      wifi_ssid = f.readStringUntil('\n');
      wifi_pass = f.readStringUntil('\n');
      wifi_ssid.trim();
      wifi_pass.trim();
      f.close();
      Serial.println("WiFi credentials loaded from SD card");
    }
  }

  // If no SD card credentials, try SmartConfig
  if (wifi_ssid.isEmpty()) {
    Serial.println("No WiFi credentials found. Starting SmartConfig...");
    Serial.println("Use ESP-Touch app to configure WiFi.");
    avatar.setSpeechText("WiFi設定待ち...");

    WiFi.mode(WIFI_STA);
    WiFi.beginSmartConfig();

    while (!WiFi.smartConfigDone()) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nSmartConfig done!");
  } else {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
  }

  // Wait for connection
  avatar.setSpeechText("WiFi接続中...");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 30) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    avatar.setSpeechText(WiFi.localIP().toString().c_str());
    delay(2000);
    avatar.setSpeechText("");
  } else {
    Serial.println("\nWiFi connection failed!");
    avatar.setSpeechText("WiFi接続失敗");
  }
}

// ---- Setup ----
void setup() {
  auto cfg = M5.config();
  cfg.external_spk = true;
  M5.begin(cfg);

  // Speaker config
  {
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = 96000;
    spk_cfg.dma_buf_count = 20;
    spk_cfg.dma_buf_len = 512;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, 180);
  }

  // Start avatar
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // Audio output
  audioOut = new AudioOutputM5Speaker(&M5.Speaker, m5spk_virtual_channel);
  audioOut->SetRate(24000);  // VOICEVOX default sample rate

  // Start WAV playback task on core 0
  xTaskCreatePinnedToCore(wavPlayTask, "wavPlay", 8192, nullptr, 1, nullptr, PRO_CPU_NUM);

  // WiFi
  setupWiFi();

  // HTTP server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/face", HTTP_GET, handleFace);
  server.on("/play_wav", HTTP_POST, handlePlayWav);
  server.on("/setting", HTTP_GET, handleSetting);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("HTTP server started on port 80");
  Serial.printf("Endpoints: / /status /face /play_wav /setting\n");
}

// ---- Loop ----
void loop() {
  M5.update();
  server.handleClient();
  delay(1);
}
