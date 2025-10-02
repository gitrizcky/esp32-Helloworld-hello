#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "driver/i2s.h"

// 🔑 Your WiFi + API config
// ====== Wi-Fi ======
// Wi-Fi settings
const char* WIFI_SSID     = "Demo";
const char* WIFI_PASSWORD = "Demotksc2791";

// OpenAI API Key
const char* OPENAI_API_KEY = "sk-";
const char* OPENAI_TTS_URL = "https://api.openai.com/v1/audio/speech";

// // I2S pins (adjust to your wiring)
// #define I2S_BCLK  26   // BCLK
// #define I2S_LRC   25   // LRCLK
// #define I2S_DOUT  22   // DIN (data in to MAX98357A)

// I2S pins (adjust to your wiring)
#define I2S_BCLK  7   // BCLK
#define I2S_LRC   8   // LRCLK
#define I2S_DOUT  9   // DIN (data in to MAX98357A)



bool playedOnce = false;

void setupSpeaker() {
  // I2S config for 16kHz stereo
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, 
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_I2S | I2S_COMM_FORMAT_I2S_MSB),
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_set_pin(I2S_NUM_1, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_1);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected!");
  Serial.println(WiFi.localIP());

  setupSpeaker();
}

void loop() {
  if (playedOnce) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  if (!http.begin(client, OPENAI_TTS_URL)) {
    Serial.println("❌ HTTP begin failed");
    return;
  }

  http.addHeader("Authorization", String("Bearer ") + OPENAI_API_KEY);
  http.addHeader("Content-Type", "application/json");

  // Force WAV output
  String body = R"rawliteral(
  {
    "model": "gpt-4o-mini-tts",
    "voice": "alloy",
    "input": "Hello, I am your ESP32 speaking with clear voice!",
    "response_format": "wav",
    "sample_rate": 16000
  }
  )rawliteral";

  http.setTimeout(30000);
  int code = http.POST(body);

  if (code <= 0) {
    Serial.printf("❌ HTTP error: %s\n", http.errorToString(code).c_str());
    http.end();
    return;
  }

  Serial.printf("HTTP code: %d\n", code);
  WiFiClient* stream = http.getStreamPtr();

  // 🔎 Look for "RIFF" in stream (skip chunk headers)
  bool foundRIFF = false;
  uint8_t searchBuf[4] = {0};
  while (!foundRIFF && http.connected()) {
    if (stream->available()) {
      searchBuf[0] = searchBuf[1];
      searchBuf[1] = searchBuf[2];
      searchBuf[2] = searchBuf[3];
      searchBuf[3] = stream->read();
      if (searchBuf[0] == 'R' && searchBuf[1] == 'I' &&
          searchBuf[2] == 'F' && searchBuf[3] == 'F') {
        Serial.println("🎵 Found RIFF header, WAV detected!");
        foundRIFF = true;
      }
    }
  }

  if (!foundRIFF) {
    Serial.println("❌ Could not find RIFF header, aborting.");
    http.end();
    playedOnce = true;
    return;
  }

  // Skip the rest of 44-byte WAV header
  int skipped = 4;
  while (skipped < 44 && http.connected()) {
    if (stream->available()) {
      stream->read();
      skipped++;
    }
  }
  Serial.println("✅ WAV header skipped, starting audio playback...");

  const size_t chunk = 512;
  uint8_t buf[chunk];

    while (http.connected()) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, min(avail, (int)chunk));
      if (n > 0) {
        int16_t *samples = (int16_t*)buf;
        int sampleCount = n / 2;

        for (int i = 0; i < sampleCount; i++) {
          int16_t mono = samples[i];
          int16_t stereo[2] = {mono, mono};   // duplicate mono → left + right
          size_t written;
          i2s_write(I2S_NUM_1, stereo, sizeof(stereo), &written, portMAX_DELAY);
        }
      }
    } else {
      delay(1);
    }
  }


  Serial.println("✅ Done playing!");
  http.end();
  playedOnce = true;
}
