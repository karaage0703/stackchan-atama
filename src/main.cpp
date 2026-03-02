/**
 * stackchan-atama - アタマだけスタックチャン
 *
 * M5Stack単体で動く顔+音声スタックチャン（サーボ不要）
 * USB シリアル or WiFi HTTP API で外部から制御可能
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

// Camera support (CoreS3 only)
#if defined(ENABLE_CAMERA)
#include <esp_camera.h>
#include "mbedtls/base64.h"
static bool camera_initialized = false;
// Forward declarations
bool setupCamera();
void handleCapture();
#endif

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
bool wifi_connected = false;

// WAV playback queue (ring buffer with 4 slots)
static constexpr int WAV_QUEUE_SIZE = 4;
struct WavSlot {
  uint8_t* data;
  size_t len;
};
WavSlot wav_queue[WAV_QUEUE_SIZE] = {};
volatile int wav_queue_head = 0;  // Next slot to play
volatile int wav_queue_tail = 0;  // Next slot to write
volatile bool wav_playing = false;

// Serial command buffer
String serial_cmd_buf = "";

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

// WAV queue helpers
int wavQueueCount() {
  int count = wav_queue_tail - wav_queue_head;
  if (count < 0) count += WAV_QUEUE_SIZE;
  return count;
}

bool wavQueueFull() {
  return wavQueueCount() >= (WAV_QUEUE_SIZE - 1);
}

bool wavQueueEmpty() {
  return wav_queue_head == wav_queue_tail;
}

// Enqueue WAV data (returns false if queue full)
bool wavQueuePush(uint8_t* data, size_t len) {
  if (wavQueueFull()) return false;
  wav_queue[wav_queue_tail].data = data;
  wav_queue[wav_queue_tail].len = len;
  wav_queue_tail = (wav_queue_tail + 1) % WAV_QUEUE_SIZE;
  return true;
}

// ---- WAV playback task ----
void wavPlayTask(void* param) {
  while (true) {
    if (!wavQueueEmpty() && !wav_playing) {
      wav_playing = true;

      // Dequeue
      WavSlot& slot = wav_queue[wav_queue_head];
      uint8_t* data = slot.data;
      size_t len = slot.len;
      slot.data = nullptr;
      slot.len = 0;
      wav_queue_head = (wav_queue_head + 1) % WAV_QUEUE_SIZE;

      AudioFileSourceMemory* src = new AudioFileSourceMemory(data, len);
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
      free(data);

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
  if (wavQueueFull()) {
    server.send(409, "application/json", "{\"error\":\"queue full\"}");
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

  // Allocate memory for WAV data (PSRAM if available, else heap)
  uint8_t* buf = (uint8_t*)(psramFound() ? ps_malloc(len) : malloc(len));
  if (buf == nullptr) {
    server.send(500, "application/json", "{\"error\":\"memory allocation failed\"}");
    return;
  }

  memcpy(buf, body.c_str(), len);
  wavQueuePush(buf, len);

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

// ---- Serial Command Handler ----
// Commands:
//   FACE:happy       - Change expression
//   WAV:12345        - Receive WAV binary (next 12345 bytes)
//   VOLUME:180       - Set volume (0-255)
//   STATUS           - Print status JSON
//   WIFI:ssid:pass   - Connect to WiFi
void handleSerialCommand(const String& cmd) {
  if (cmd.startsWith("FACE:")) {
    String expr_str = cmd.substring(5);
    expr_str.trim();
    Expression expr = getExpression(expr_str);
    avatar.setExpression(expr);
    Serial.printf("{\"status\":\"ok\",\"expression\":\"%s\"}\n", getExpressionName(expr).c_str());

  } else if (cmd.startsWith("WAV:")) {
    if (wavQueueFull()) {
      Serial.println("{\"status\":\"error\",\"error\":\"queue full\"}");
      return;
    }
    size_t len = cmd.substring(4).toInt();
    size_t max_wav = psramFound() ? 1000000 : 100000;  // 1MB with PSRAM, 100KB without
    if (len < 44 || len > max_wav) {
      Serial.println("{\"status\":\"error\",\"error\":\"invalid size\"}");
      return;
    }

    // Allocate memory (PSRAM if available, else heap)
    uint8_t* buf = (uint8_t*)(psramFound() ? ps_malloc(len) : malloc(len));
    if (buf == nullptr) {
      Serial.println("{\"status\":\"error\",\"error\":\"memory allocation failed\"}");
      return;
    }

    // Drain any leftover data in serial buffer before signaling READY
    while (Serial.available()) Serial.read();

    Serial.println("READY");  // Signal PC to start sending binary
    Serial.flush();  // Ensure READY is sent before reading

    // Read binary data from serial
    size_t received = 0;
    unsigned long start = millis();
    while (received < len && (millis() - start) < 30000) {
      int avail = Serial.available();
      if (avail > 0) {
        while (avail > 0 && received < len) {
          buf[received++] = Serial.read();
          avail--;
        }
        start = millis();
      } else {
        delay(1);
      }
    }
    if (received == len) {
      wavQueuePush(buf, len);
      Serial.printf("{\"status\":\"ok\",\"size\":%d,\"queued\":%d}\n", len, wavQueueCount());
    } else {
      free(buf);
      Serial.printf("{\"status\":\"error\",\"error\":\"incomplete\",\"received\":%d,\"expected\":%d}\n", received, len);
    }

  } else if (cmd.startsWith("VOLUME:")) {
    int vol = cmd.substring(7).toInt();
    if (vol >= 0 && vol <= 255) {
      M5.Speaker.setChannelVolume(m5spk_virtual_channel, vol);
    }
    Serial.printf("{\"status\":\"ok\",\"volume\":%d}\n", M5.Speaker.getChannelVolume(m5spk_virtual_channel));

  } else if (cmd == "STATUS") {
#if defined(ENABLE_CAMERA)
    Serial.printf("{\"status\":\"online\",\"wifi\":%s,\"ip\":\"%s\",\"playing\":%s,\"queued\":%d,\"camera\":%s}\n",
      wifi_connected ? "true" : "false",
      wifi_connected ? WiFi.localIP().toString().c_str() : "none",
      wav_playing ? "true" : "false",
      wavQueueCount(),
      camera_initialized ? "true" : "false");
#else
    Serial.printf("{\"status\":\"online\",\"wifi\":%s,\"ip\":\"%s\",\"playing\":%s,\"queued\":%d,\"camera\":false}\n",
      wifi_connected ? "true" : "false",
      wifi_connected ? WiFi.localIP().toString().c_str() : "none",
      wav_playing ? "true" : "false",
      wavQueueCount());
#endif

  } else if (cmd.startsWith("WIFI:")) {
    // WIFI:ssid:password
    int first_colon = cmd.indexOf(':', 5);
    if (first_colon > 0) {
      wifi_ssid = cmd.substring(5, first_colon);
      wifi_pass = cmd.substring(first_colon + 1);
      wifi_ssid.trim();
      wifi_pass.trim();
      Serial.println("{\"status\":\"connecting\"}");
      avatar.setSpeechText("WiFi接続中...");
      WiFi.mode(WIFI_STA);
      WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
      int retry = 0;
      while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        retry++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        wifi_connected = true;
        server.begin();
        Serial.printf("{\"status\":\"ok\",\"ip\":\"%s\"}\n", WiFi.localIP().toString().c_str());
        avatar.setSpeechText(WiFi.localIP().toString().c_str());
        delay(2000);
        avatar.setSpeechText("");
      } else {
        Serial.println("{\"status\":\"error\",\"error\":\"wifi connection failed\"}");
        avatar.setSpeechText("WiFi失敗");
        delay(1000);
        avatar.setSpeechText("");
      }
    }

  } else if (cmd == "CAPTURE") {
#if defined(ENABLE_CAMERA)
    handleCapture();
#else
    Serial.println("{\"status\":\"error\",\"error\":\"camera not supported on this device\"}");
#endif

  } else {
    Serial.printf("{\"status\":\"error\",\"error\":\"unknown command: %s\"}\n", cmd.c_str());
  }
}

// ---- Camera Setup (CoreS3 only) ----
#if defined(ENABLE_CAMERA)
bool setupCamera() {
  // Release M5Unified's internal I2C so esp_camera can use GPIO 11/12
  M5.In_I2C.release();

  camera_config_t config = {};
  config.pin_pwdn     = -1;
  config.pin_reset    = -1;
  config.pin_xclk     = 2;
  config.pin_sccb_sda = 12;
  config.pin_sccb_scl = 11;
  config.pin_d7       = 47;
  config.pin_d6       = 48;
  config.pin_d5       = 16;
  config.pin_d4       = 15;
  config.pin_d3       = 42;
  config.pin_d2       = 41;
  config.pin_d1       = 40;
  config.pin_d0       = 39;
  config.pin_vsync    = 46;
  config.pin_href     = 38;
  config.pin_pclk     = 45;

  config.xclk_freq_hz = 20000000;
  config.ledc_timer   = LEDC_TIMER_1;    // Avoid conflict with M5Speaker
  config.ledc_channel = LEDC_CHANNEL_1;  // Avoid conflict with M5Speaker
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_QVGA;  // 320x240
  config.jpeg_quality = 12;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.sccb_i2c_port = -1;  // Let esp_camera manage I2C

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] init failed: 0x%x\n", err);
    return false;
  }
  Serial.println("[CAM] initialized");
  return true;
}

void handleCapture() {
  if (!camera_initialized) {
    Serial.println("{\"status\":\"error\",\"error\":\"camera not available\"}");
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("{\"status\":\"error\",\"error\":\"capture failed\"}");
    return;
  }

  // Convert RGB565 to JPEG
  uint8_t* jpg_buf = NULL;
  size_t jpg_len = 0;
  bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);

  if (!ok || !jpg_buf) {
    Serial.println("{\"status\":\"error\",\"error\":\"jpeg conversion failed\"}");
    return;
  }

  // Base64 encode
  size_t b64_len = 0;
  mbedtls_base64_encode(NULL, 0, &b64_len, jpg_buf, jpg_len);
  uint8_t* b64_buf = (uint8_t*)ps_malloc(b64_len + 1);
  if (!b64_buf) {
    free(jpg_buf);
    Serial.println("{\"status\":\"error\",\"error\":\"base64 alloc failed\"}");
    return;
  }

  mbedtls_base64_encode(b64_buf, b64_len + 1, &b64_len, jpg_buf, jpg_len);
  b64_buf[b64_len] = '\0';
  free(jpg_buf);

  // Send: header line + base64 data + END marker
  Serial.printf("{\"status\":\"ok\",\"format\":\"jpeg\",\"size\":%d,\"b64_size\":%d}\n", jpg_len, b64_len);
  Serial.flush();

  // Send base64 in chunks to avoid serial buffer overflow
  size_t sent = 0;
  while (sent < b64_len) {
    size_t chunk = (b64_len - sent > 1024) ? 1024 : (b64_len - sent);
    Serial.write(b64_buf + sent, chunk);
    sent += chunk;
    Serial.flush();
  }
  Serial.println();  // newline after base64
  Serial.println("END_CAPTURE");
  Serial.flush();

  free(b64_buf);
}
#endif

// ---- WiFi Setup ----
void setupWiFi() {
  // Try to read from SD card first (only if SD is available)
  // Note: On CoreS3 without SD card, SD.begin can hang or cause GPIO errors
  // So we skip SD entirely and rely on serial WIFI command
  #if 0
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
  #endif

  // If no SD card credentials, skip WiFi (use serial instead)
  if (wifi_ssid.isEmpty()) {
    Serial.println("No WiFi credentials. Use serial command WIFI:ssid:password to connect.");
    Serial.println("Or use USB serial commands: FACE:happy, WAV:size, STATUS, VOLUME:180");
    avatar.setSpeechText("USB Ready");
    delay(1500);
    avatar.setSpeechText("");
    return;
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
    wifi_connected = true;
    Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
    avatar.setSpeechText(WiFi.localIP().toString().c_str());
    delay(2000);
    avatar.setSpeechText("");
  } else {
    Serial.println("\nWiFi connection failed. Use serial commands instead.");
    avatar.setSpeechText("USB Ready");
    delay(1500);
    avatar.setSpeechText("");
  }
}

// ---- Setup ----
void setup() {
  auto cfg = M5.config();
  cfg.external_spk = true;
  M5.begin(cfg);

  // Re-init Serial after M5.begin() for USB CDC on ESP32-S3
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  Serial.setRxBufferSize(32768);  // 32KB receive buffer (ESP32-S3 has more RAM)
#else
  Serial.setRxBufferSize(4096);   // 4KB receive buffer (ESP32 classic)
#endif
  Serial.begin(921600);
  delay(500);

  // Speaker config
  {
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = 96000;
    spk_cfg.dma_buf_count = 20;
    spk_cfg.dma_buf_len = 512;
    M5.Speaker.config(spk_cfg);
    M5.Speaker.setVolume(255);  // Master volume max
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, 255);
  }

  // Start avatar
  avatar.init();
  avatar.setExpression(Expression::Neutral);

  // Audio output
  audioOut = new AudioOutputM5Speaker(&M5.Speaker, m5spk_virtual_channel);
  audioOut->SetRate(24000);  // VOICEVOX default sample rate

  // Start WAV playback task on core 1 (same as Arduino loop, needed for M5Stack Core speaker)
  xTaskCreatePinnedToCore(wavPlayTask, "wavPlay", 8192, nullptr, 1, nullptr, APP_CPU_NUM);

  // WiFi (optional - will skip if no SD card credentials)
  setupWiFi();

  // HTTP server (register endpoints, start only if WiFi connected)
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/face", HTTP_GET, handleFace);
  server.on("/play_wav", HTTP_POST, handlePlayWav);
  server.on("/setting", HTTP_GET, handleSetting);
  server.onNotFound(handleNotFound);
  if (wifi_connected) {
    server.begin();
    Serial.println("HTTP server started on port 80");
  }

  // Camera init (CoreS3 only)
#if defined(ENABLE_CAMERA)
  camera_initialized = setupCamera();
#endif

  Serial.println("Serial commands ready: FACE:expr WAV:size VOLUME:vol STATUS WIFI:ssid:pass CAPTURE");
}

// ---- Loop ----
void loop() {
  M5.update();

  // Handle HTTP clients (if WiFi connected)
  if (wifi_connected) {
    server.handleClient();
  }

  // Handle serial commands
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serial_cmd_buf.length() > 0) {
        handleSerialCommand(serial_cmd_buf);
        serial_cmd_buf = "";
      }
    } else {
      serial_cmd_buf += c;
    }
  }

  delay(1);
}
