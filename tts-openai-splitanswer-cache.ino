#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>     // NVS for persistent cache
#include "Audio.h"

// =======================
// Wi-Fi + API Credentials
// =======================
const char* WIFI_SSID = "Raline";
const char* WIFI_PASS = "Hachi1234!";

// ------------------------------------------------------------------------------------
// DANGER: Do not hardcode your API key in production.
// ------------------------------------------------------------------------------------
const char* OPENAI_API_KEY = "sk-"; // ⚠️ Replace with your OpenAI key

// =======================
// I2S Speaker Pin Setup
// =======================
#define SPK_BCLK 7
#define SPK_LRC  8
#define SPK_DIN  9

Audio audio;

// =======================
// Persistent Cache (NVS)
// =======================
Preferences kv;

#define CACHE_SIZE 20          // keep latest 20 Q&As
int cacheIndex = 0;            // rotating index

// djb2 hash (stable, simple)
uint32_t djb2(const String& s) {
  uint32_t h = 5381;
  for (size_t i = 0; i < s.length(); i++) h = ((h << 5) + h) + (uint8_t)s[i];
  return h;
}

// normalize question: trim, lowercase, collapse multiple spaces
String normalizeQuestion(String in) {
  in.trim();
  in.toLowerCase();
  String out; out.reserve(in.length());
  bool prevSpace = false;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (!prevSpace) { out += ' '; prevSpace = true; }
    } else {
      out += c; prevSpace = false;
    }
  }
  return out;
}

void cacheInit() {
  kv.begin("qa", false);
  cacheIndex = kv.getInt("idx", 0); // restore rotating pointer
}

String cacheKeyFor(const String& qNorm) {
  return String(djb2(qNorm), HEX);  // short hex key
}

String cacheGet(const String& qNorm) {
  String key = cacheKeyFor(qNorm);
  return kv.getString(key.c_str(), "");
}

void cachePut(const String& qNorm, const String& answer) {
  String key = cacheKeyFor(qNorm);
  kv.putString(key.c_str(), answer);

  // keep a small rotating list of keys so you can manage/inspect later
  String slot = "k" + String(cacheIndex);
  kv.putString(slot.c_str(), key);
  cacheIndex = (cacheIndex + 1) % CACHE_SIZE;
  kv.putInt("idx", cacheIndex);
}

// Clear all cached entries and reset index
void cacheClear() {
  kv.clear();            // wipe the whole "qa" namespace
  cacheIndex = 0;
  kv.putInt("idx", 0);   // re-create the index key to keep code paths simple
}

// =======================
// Speak in chunks
// =======================
void speakAnswer(String text) {
  text.trim();
  if (text.length() == 0) return;

  int start = 0;
  for (int i = 0; i < text.length(); i++) {
    if (text.charAt(i) == '.' || text.charAt(i) == '!' || text.charAt(i) == '?') {
      String sentence = text.substring(start, i + 1);
      sentence.trim();
      if (sentence.length() > 0) {
        Serial.print("🔊 Speaking chunk: ");
        Serial.println(sentence);
        audio.stopSong(); // reset pipeline between chunks
        audio.connecttospeech(sentence.c_str(), "en-US");
        while (audio.isRunning()) audio.loop();
      }
      start = i + 1;
    }
  }
  if (start < text.length()) {
    String sentence = text.substring(start);
    sentence.trim();
    if (sentence.length() > 0) {
      Serial.print("🔊 Speaking last chunk: ");
      Serial.println(sentence);
      audio.stopSong();
      audio.connecttospeech(sentence.c_str(), "en-US");
      while (audio.isRunning()) audio.loop();
    }
  }
}

// =======================
// Commands
// =======================
bool handleCommand(const String& raw) {
  String cmd = raw; cmd.trim();
  if (cmd.equalsIgnoreCase("/clear")) {
    cacheClear();
    Serial.println("🧹 Cache cleared.");
    speakAnswer("Cache cleared.");
    return true;
  }
  return false; // not a command
}

// =======================
// Setup / Loop
// =======================
void setup() {
  Serial.begin(115200);

  // Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi...");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\n✅ WiFi connected!");
  Serial.print("IP Address: "); Serial.println(WiFi.localIP());

  // Time for TLS
  Serial.print("Syncing time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) { delay(500); Serial.print("."); now = time(nullptr); }
  Serial.println("\n✅ Time synchronized.");

  // Audio
  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
  audio.setVolume(18);

  // Cache
  cacheInit();
  Serial.println("✅ Persistent cache ready. Type /clear to wipe.");
}

void loop() {
  Serial.println("\nType your question (or /clear) and press Enter:");
  while (!Serial.available()) { audio.loop(); delay(50); }

  String question = Serial.readStringUntil('\n');
  question.trim();

  // handle commands first
  if (handleCommand(question)) {
    while (Serial.available()) Serial.read();
    return; // command handled
  }

  String qNorm = normalizeQuestion(question);
  Serial.print("❓ Question: "); Serial.println(question);

  // 1) Try cache
  String answer = cacheGet(qNorm);
  if (answer.length()) {
    Serial.println("⚡ Served from cache.");
  } else {
    // 2) Call API and then cache it
    answer = askChatGPT(question);
    if (answer.length()) {
      cachePut(qNorm, answer);
      Serial.println("💾 Cached this Q&A.");
    }
  }

  if (answer.length()) {
    Serial.print("🤖 Answer: "); Serial.println(answer);
    speakAnswer(answer);
  } else {
    Serial.println("❌ Sorry, could not get an answer.");
  }

  while (Serial.available()) Serial.read(); // clear buffer
}

// =======================
// OpenAI helper
// =======================
String askChatGPT(String question) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected.");
    return "";
  }

  WiFiClientSecure client;
  client.setInsecure(); // demo only; consider proper root CA

  HTTPClient https;
  if (!https.begin(client, "https://api.openai.com/v1/chat/completions")) {
    Serial.println("❌ Unable to connect to OpenAI API");
    return "";
  }
  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", "Bearer " + String(OPENAI_API_KEY));

  DynamicJsonDocument doc(1024);
  doc["model"] = "gpt-4o-mini";
  JsonArray messages = doc.createNestedArray("messages");
  JsonObject userMessage = messages.createNestedObject();
  userMessage["role"] = "user";
  userMessage["content"] = question;

  String body; serializeJson(doc, body);
  int httpCode = https.POST(body);

  if (httpCode <= 0) {
    Serial.printf("❌ HTTP POST failed, error: %s\n", https.errorToString(httpCode).c_str());
    https.end();
    return "";
  }
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("❌ HTTP Error %d\n", httpCode);
    Serial.println("Server response: " + https.getString());
    https.end();
    return "";
  }

  String payload = https.getString();
  https.end();

  DynamicJsonDocument response(4096);
  if (deserializeJson(response, payload)) {
    Serial.println("❌ JSON parsing failed!");
    return "";
  }
  const char* content = response["choices"][0]["message"]["content"];
  return content ? String(content) : "";
}
