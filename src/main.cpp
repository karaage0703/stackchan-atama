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
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Preferences.h>

Preferences preferences;

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
// WiFi credentials: set via Serial, NVS, or SD card
String wifi_ssid = "";
String wifi_pass = "";

// Forward declarations for WiFi credential management
void saveWiFiCredentials(const String& ssid, const String& pass);
void clearWiFiCredentials();

// ---- Audio ----
static constexpr uint8_t m5spk_virtual_channel = 0;

// Simple WAV player using M5.Speaker.playRaw directly (no ESP8266Audio dependency)
class SimpleWavPlayer {
public:
  SimpleWavPlayer(m5::Speaker_Class* speaker, uint8_t virtual_ch = 0)
    : _speaker(speaker), _virtual_ch(virtual_ch), _tri_index(0), _tri_filled(0), _sample_rate(24000) {}

  void setSampleRate(uint32_t rate) { _sample_rate = rate; }
  uint32_t getSampleRate() const { return _sample_rate; }

  // Feed a mono 16-bit sample
  void feedSample(int16_t sample) {
    _tri_buffer[_tri_index][_tri_filled++] = sample;
    if (_tri_filled >= TRI_BUF_SIZE) {
      flush();
    }
  }

  void flush() {
    if (_tri_filled > 0) {
      _speaker->playRaw(_tri_buffer[_tri_index], _tri_filled, _sample_rate, false, 1, _virtual_ch);
      _tri_index = (_tri_index + 1) % 3;
      _tri_filled = 0;
    }
  }

  void reset() {
    _tri_index = 0;
    _tri_filled = 0;
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
  uint32_t _sample_rate;
};

// WAV header parser
struct WavHeader {
  uint16_t channels;
  uint32_t sample_rate;
  uint16_t bits_per_sample;
  uint32_t data_offset;
  uint32_t data_size;
  bool valid;
};

WavHeader parseWavHeader(const uint8_t* data, size_t len) {
  WavHeader h = {0, 0, 0, 0, 0, false};
  if (len < 44) return h;
  // RIFF header
  if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return h;

  // Find fmt chunk
  size_t pos = 12;
  while (pos + 8 <= len) {
    uint32_t chunk_size = data[pos+4] | (data[pos+5]<<8) | (data[pos+6]<<16) | (data[pos+7]<<24);
    if (memcmp(data + pos, "fmt ", 4) == 0) {
      if (pos + 8 + chunk_size > len || chunk_size < 16) return h;
      h.channels = data[pos+10] | (data[pos+11]<<8);
      h.sample_rate = data[pos+12] | (data[pos+13]<<8) | (data[pos+14]<<16) | (data[pos+15]<<24);
      h.bits_per_sample = data[pos+22] | (data[pos+23]<<8);
    } else if (memcmp(data + pos, "data", 4) == 0) {
      h.data_offset = pos + 8;
      h.data_size = chunk_size;
      h.valid = (h.sample_rate > 0 && h.bits_per_sample > 0 && h.channels > 0);
      return h;
    }
    pos += 8 + chunk_size;
    if (chunk_size & 1) pos++;  // Pad byte
  }
  return h;
}

// ---- Globals ----
using namespace m5avatar;
Avatar avatar;
AsyncWebServer server(80);
SimpleWavPlayer* wavPlayer = nullptr;
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

      // Parse WAV header
      WavHeader hdr = parseWavHeader(data, len);
      if (hdr.valid && hdr.bits_per_sample == 16) {
        wavPlayer->setSampleRate(hdr.sample_rate);
        wavPlayer->reset();

        const int16_t* pcm = (const int16_t*)(data + hdr.data_offset);
        size_t total_samples = hdr.data_size / (hdr.bits_per_sample / 8);
        size_t step = hdr.channels;  // Skip interleaved channels (take first channel only)

        for (size_t i = 0; i < total_samples; i += step) {
          // Mono mix if stereo
          if (hdr.channels == 2 && i + 1 < total_samples) {
            int32_t mix = ((int32_t)pcm[i] + (int32_t)pcm[i+1]) / 2;
            wavPlayer->feedSample((int16_t)mix);
          } else {
            wavPlayer->feedSample(pcm[i]);
          }

          // Lip sync (every 640 samples)
          if ((i / step) % 640 == 639) {
            float level = (float)wavPlayer->getLevel() / 5000.0f;
            if (level > 1.0f) level = 1.0f;
            avatar.setMouthOpenRatio(level);
            vTaskDelay(1);
          }
        }
        wavPlayer->flush();
      }

      avatar.setMouthOpenRatio(0.0f);
      free(data);

      wav_playing = false;
    }
    vTaskDelay(10);
  }
}

// ---- HTTP Handlers (ESPAsyncWebServer) ----
void handleRoot(AsyncWebServerRequest *request) {
  request->send(200, "text/plain", "hello from stackchan-atama!");
}

void handleStatus(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["status"] = "online";
  doc["ip"] = WiFi.localIP().toString();
  doc["playing"] = wav_playing;
  doc["queued"] = wavQueueCount();
  doc["psram"] = psramFound();

  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void handleFace(AsyncWebServerRequest *request) {
  if (!request->hasParam("expression")) {
    request->send(400, "application/json", "{\"error\":\"expression parameter required\"}");
    return;
  }

  String expr_str = request->getParam("expression")->value();
  Expression expr = getExpression(expr_str);
  avatar.setExpression(expr);

  JsonDocument doc;
  doc["status"] = "ok";
  doc["expression"] = getExpressionName(expr);
  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void handleSetting(AsyncWebServerRequest *request) {
  if (request->hasParam("volume")) {
    int vol = request->getParam("volume")->value().toInt();
    if (vol >= 0 && vol <= 255) {
      M5.Speaker.setChannelVolume(m5spk_virtual_channel, vol);
    }
  }

  JsonDocument doc;
  doc["status"] = "ok";
  doc["volume"] = M5.Speaker.getChannelVolume(m5spk_virtual_channel);
  String json;
  serializeJson(doc, json);
  request->send(200, "application/json", json);
}

void handleNotFound(AsyncWebServerRequest *request) {
  request->send(404, "application/json", "{\"error\":\"not found\"}");
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
#ifdef ARDUINO_M5STACK_ATOMS3R
#if defined(ENABLE_CAMERA)
    Serial.printf("{\"status\":\"online\",\"board\":\"AtomS3R+Echo\",\"wifi\":%s,\"ip\":\"%s\",\"playing\":%s,\"queued\":%d,\"camera\":%s,\"psram\":%s}\n",
      wifi_connected ? "true" : "false",
      wifi_connected ? WiFi.localIP().toString().c_str() : "none",
      wav_playing ? "true" : "false",
      wavQueueCount(),
      camera_initialized ? "true" : "false",
      psramFound() ? "true" : "false");
#else
    Serial.printf("{\"status\":\"online\",\"board\":\"AtomS3R+Echo\",\"wifi\":%s,\"ip\":\"%s\",\"playing\":%s,\"queued\":%d,\"camera\":false,\"psram\":%s}\n",
      wifi_connected ? "true" : "false",
      wifi_connected ? WiFi.localIP().toString().c_str() : "none",
      wav_playing ? "true" : "false",
      wavQueueCount(),
      psramFound() ? "true" : "false");
#endif
#else
#if defined(ENABLE_CAMERA)
    Serial.printf("{\"status\":\"online\",\"wifi\":%s,\"ip\":\"%s\",\"playing\":%s,\"queued\":%d,\"camera\":%s,\"psram\":%s}\n",
      wifi_connected ? "true" : "false",
      wifi_connected ? WiFi.localIP().toString().c_str() : "none",
      wav_playing ? "true" : "false",
      wavQueueCount(),
      camera_initialized ? "true" : "false",
      psramFound() ? "true" : "false");
#else
    Serial.printf("{\"status\":\"online\",\"wifi\":%s,\"ip\":\"%s\",\"playing\":%s,\"queued\":%d,\"camera\":false,\"psram\":%s}\n",
      wifi_connected ? "true" : "false",
      wifi_connected ? WiFi.localIP().toString().c_str() : "none",
      wav_playing ? "true" : "false",
      wavQueueCount(),
      psramFound() ? "true" : "false");
#endif
#endif

  } else if (cmd == "WIFI:CLEAR") {
    clearWiFiCredentials();
    Serial.println("{\"status\":\"ok\",\"message\":\"wifi credentials cleared\"}");

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
        WiFi.setSleep(false);  // Disable WiFi power saving for lower latency
        saveWiFiCredentials(wifi_ssid, wifi_pass);
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

  // Discard first few frames to let AWB/AEC stabilize
  for (int i = 0; i < 4; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(50);
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

void handleCaptureHTTP(AsyncWebServerRequest *request) {
  if (!camera_initialized) {
    request->send(503, "application/json", "{\"error\":\"camera not available\"}");
    return;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    request->send(500, "application/json", "{\"error\":\"capture failed\"}");
    return;
  }

  // Convert RGB565 to JPEG
  uint8_t* jpg_buf = NULL;
  size_t jpg_len = 0;
  bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
  esp_camera_fb_return(fb);

  if (!ok || !jpg_buf) {
    request->send(500, "application/json", "{\"error\":\"jpeg conversion failed\"}");
    return;
  }

  // Use chunked response with captured buffer pointer and length
  uint8_t* capture_buf = jpg_buf;
  size_t capture_len = jpg_len;
  AsyncWebServerResponse *response = request->beginChunkedResponse(
    "image/jpeg",
    [capture_buf, capture_len](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
      if (index >= capture_len) {
        free(capture_buf);
        return 0;  // Done
      }
      size_t toSend = capture_len - index;
      if (toSend > maxLen) toSend = maxLen;
      memcpy(buffer, capture_buf + index, toSend);
      return toSend;
    }
  );
  response->addHeader("Content-Disposition", "inline; filename=\"capture.jpg\"");
  request->send(response);
}
#endif

// ---- WiFi Setup ----
void saveWiFiCredentials(const String& ssid, const String& pass) {
  preferences.begin("wifi", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.end();
  Serial.println("[NVS] WiFi credentials saved");
}

void clearWiFiCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
  wifi_ssid = "";
  wifi_pass = "";
  Serial.println("[NVS] WiFi credentials cleared");
}

void setupWiFi() {
  // Load credentials from NVS
  preferences.begin("wifi", true);  // read-only
  wifi_ssid = preferences.getString("ssid", "");
  wifi_pass = preferences.getString("pass", "");
  preferences.end();

  if (!wifi_ssid.isEmpty()) {
    Serial.println("[NVS] WiFi credentials loaded");
  }

  if (wifi_ssid.isEmpty()) {
    Serial.println("No WiFi credentials. Use serial command WIFI:ssid:password to connect.");
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
    WiFi.setSleep(false);  // Disable WiFi power saving for lower latency
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
#ifdef ARDUINO_M5STACK_ATOMS3R
  // AtomS3R with Atomic Echo Base configuration
  cfg.external_speaker.atomic_echo = true;
#else
  // Default configuration (CoreS3, Core2, etc.)
  cfg.external_spk = true;
#endif
  M5.begin(cfg);

  // Re-init Serial after M5.begin() for USB CDC on ESP32-S3
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  Serial.setRxBufferSize(32768);  // 32KB receive buffer (ESP32-S3 has more RAM)
#else
  Serial.setRxBufferSize(4096);   // 4KB receive buffer (ESP32 classic)
#endif

#ifdef ARDUINO_M5STACK_ATOMS3R
  Serial.begin(115200);  // Optimized for AtomS3R
#else
  Serial.begin(921600);  // Default for CoreS3/Core2
#endif
  delay(500);

  // Speaker config
  {
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = 96000;
    spk_cfg.dma_buf_count = 20;
    spk_cfg.dma_buf_len = 512;
    M5.Speaker.config(spk_cfg);
#ifdef ARDUINO_M5STACK_ATOMS3R
    // Atomic Echo Base: use lower volume to prevent distortion
    M5.Speaker.setVolume(192);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, 192);
#else
    // Default: maximum volume
    M5.Speaker.setVolume(255);
    M5.Speaker.setChannelVolume(m5spk_virtual_channel, 255);
#endif
  }

  // Start avatar
#ifdef ARDUINO_M5STACK_ATOMS3R
  // AtomS3R: Initialize avatar for headless operation (no display)
  avatar.setScale(0.5);
  avatar.setPosition(-56, -96);
  avatar.init();  // Avatar library auto-detects no display and uses appropriate size
#else
  // M5Stack devices: Use default initialization with display
  avatar.init();
#endif
  avatar.setExpression(Expression::Neutral);

  // Audio output (self-contained WAV player, no ESP8266Audio dependency)
  wavPlayer = new SimpleWavPlayer(&M5.Speaker, m5spk_virtual_channel);

  // Start WAV playback task on core 1 (same as Arduino loop, needed for M5Stack Core speaker)
  xTaskCreatePinnedToCore(wavPlayTask, "wavPlay", 8192, nullptr, 1, nullptr, APP_CPU_NUM);

  // WiFi (optional - will skip if no SD card credentials)
  setupWiFi();

  // HTTP server (ESPAsyncWebServer - register endpoints)
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/face", HTTP_GET, handleFace);
  server.on("/setting", HTTP_GET, handleSetting);
#if defined(ENABLE_CAMERA)
  server.on("/capture", HTTP_GET, handleCaptureHTTP);
#endif

  // play_wav: POST binary WAV data
  // Usage: curl -X POST -H "Content-Type: application/octet-stream" --data-binary @audio.wav http://<ip>/play_wav
  server.on("/play_wav", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      uint8_t* buf = (uint8_t*)request->_tempObject;
      if (!buf) {
        request->send(400, "application/json", "{\"error\":\"no data received\"}");
        return;
      }
      request->_tempObject = nullptr;
      size_t len = request->contentLength();
      if (wavQueueFull()) {
        free(buf);
        request->send(409, "application/json", "{\"error\":\"queue full\"}");
        return;
      }
      wavQueuePush(buf, len);
      JsonDocument doc;
      doc["status"] = "ok";
      doc["size"] = len;
      doc["queued"] = wavQueueCount();
      String json;
      serializeJson(doc, json);
      request->send(200, "application/json", json);
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        size_t max_wav = psramFound() ? 512000 : 80000;  // PSRAMなし: 80KB制限
        if (total < 44 || total > max_wav) return;
        uint8_t* buf = (uint8_t*)(psramFound() ? ps_malloc(total) : malloc(total));
        if (!buf) return;
        request->_tempObject = buf;
      }
      uint8_t* buf = (uint8_t*)request->_tempObject;
      if (buf) {
        memcpy(buf + index, data, len);
      }
    }
  );

  server.onNotFound(handleNotFound);
  if (wifi_connected) {
    server.begin();
    Serial.println("HTTP server started on port 80");
  }

  // Camera init (CoreS3 only)
#if defined(ENABLE_CAMERA)
  camera_initialized = setupCamera();
#endif

#ifdef ARDUINO_M5STACK_ATOMS3R
  Serial.println("[AtomS3R+Echo] Serial commands ready: FACE:expr WAV:size VOLUME:vol STATUS WIFI:ssid:pass CAPTURE");
#else
  Serial.println("Serial commands ready: FACE:expr WAV:size VOLUME:vol STATUS WIFI:ssid:pass CAPTURE");
#endif
}

// ---- Loop ----
void loop() {
  M5.update();

  // Note: ESPAsyncWebServer handles HTTP clients automatically (no handleClient() needed)

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
