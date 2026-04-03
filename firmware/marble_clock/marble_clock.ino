/*
 * Marble Ring Clock — ESP32 Lolin32 Lite
 * WS2812 LED ring clock with NTP sync and web configuration
 *
 * Hardware:
 *   - ESP32 Lolin32 Lite
 *   - WS2812B LED strip (60 LED/m default, configurable)
 *   - External 5V power supply for LEDs
 *   - Data pin: GPIO 22 (configurable)
 *
 * Libraries required:
 *   - FastLED
 *   - ESPAsyncWebServer
 *   - AsyncTCP
 *   - NTPClient
 *   - ArduinoJson
 *   - LittleFS (built-in with ESP32 Arduino core)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <FastLED.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <time.h>

// ─── Default configuration ──────────────────────────────────────────────────
#define DATA_PIN        22
#define MAX_LEDS        200  // Absolute maximum, allocated buffer
#define DEFAULT_NUM_LEDS 70  // Approximate for 370mm diameter ring @ 60LED/m
#define DEFAULT_BRIGHTNESS 80
#define LED_TYPE        WS2812B
#define COLOR_ORDER     GRB

// ─── NTP ───────────────────────────────────────────────────────────────────
#define NTP_SERVER      "pool.ntp.org"
#define DEFAULT_UTC_OFFSET 3600  // UTC+1 (Italy CET)

// ─── Global objects ────────────────────────────────────────────────────────
CRGB leds[MAX_LEDS];
Preferences prefs;
AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER);

// ─── Config struct ─────────────────────────────────────────────────────────
struct Config {
  char ssid[64];
  char password[64];
  int  numLeds;           // Total physical LEDs on ring
  int  dataPin;
  int  brightness;
  uint8_t hourR, hourG, hourB;   // Hour hand color
  uint8_t minR, minG, minB;     // Minute hand color
  int  hourBrightness;    // 0-255
  int  minBrightness;     // 0-255
  int  utcOffsetSec;      // Timezone offset in seconds
  int  manualHour;        // -1 = use NTP
  int  manualMinute;
  bool ntpEnabled;
  int  ledDensity;        // LEDs per meter (for display only)
  // Mapping mode: 0 = distribute evenly, 1 = use first 60, 2 = use all
  int  mappingMode;
} cfg;

// ─── Runtime state ─────────────────────────────────────────────────────────
bool wifiConnected = false;
bool ntpSynced     = false;
unsigned long lastNTPSync = 0;
unsigned long lastUpdate  = 0;

// ─── Config persistence ────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("clock", false);
  strlcpy(cfg.ssid,     prefs.getString("ssid",     "").c_str(), sizeof(cfg.ssid));
  strlcpy(cfg.password, prefs.getString("pass",     "").c_str(), sizeof(cfg.password));
  cfg.numLeds       = prefs.getInt("numLeds",    DEFAULT_NUM_LEDS);
  cfg.dataPin       = prefs.getInt("dataPin",    DATA_PIN);
  cfg.brightness    = prefs.getInt("bri",        DEFAULT_BRIGHTNESS);
  cfg.hourR         = prefs.getUChar("hR",       255);
  cfg.hourG         = prefs.getUChar("hG",       80);
  cfg.hourB         = prefs.getUChar("hB",       0);
  cfg.minR          = prefs.getUChar("mR",       0);
  cfg.minG          = prefs.getUChar("mG",       180);
  cfg.minB          = prefs.getUChar("mB",       255);
  cfg.hourBrightness= prefs.getInt("hBri",       200);
  cfg.minBrightness = prefs.getInt("mBri",       255);
  cfg.utcOffsetSec  = prefs.getInt("utcOff",     DEFAULT_UTC_OFFSET);
  cfg.manualHour    = prefs.getInt("manHour",    -1);
  cfg.manualMinute  = prefs.getInt("manMin",     0);
  cfg.ntpEnabled    = prefs.getBool("ntpEn",     true);
  cfg.ledDensity    = prefs.getInt("density",    60);
  cfg.mappingMode   = prefs.getInt("mapMode",    0);
  prefs.end();
}

void saveConfig() {
  prefs.begin("clock", false);
  prefs.putString("ssid",    cfg.ssid);
  prefs.putString("pass",    cfg.password);
  prefs.putInt("numLeds",    cfg.numLeds);
  prefs.putInt("dataPin",    cfg.dataPin);
  prefs.putInt("bri",        cfg.brightness);
  prefs.putUChar("hR",       cfg.hourR);
  prefs.putUChar("hG",       cfg.hourG);
  prefs.putUChar("hB",       cfg.hourB);
  prefs.putUChar("mR",       cfg.minR);
  prefs.putUChar("mG",       cfg.minG);
  prefs.putUChar("mB",       cfg.minB);
  prefs.putInt("hBri",       cfg.hourBrightness);
  prefs.putInt("mBri",       cfg.minBrightness);
  prefs.putInt("utcOff",     cfg.utcOffsetSec);
  prefs.putInt("manHour",    cfg.manualHour);
  prefs.putInt("manMin",     cfg.manualMinute);
  prefs.putBool("ntpEn",     cfg.ntpEnabled);
  prefs.putInt("density",    cfg.ledDensity);
  prefs.putInt("mapMode",    cfg.mappingMode);
  prefs.end();
}

// ─── LED mapping ───────────────────────────────────────────────────────────
// Returns the physical LED index for a given logical position (0-59)
// given the total physical LED count and mapping mode
int logicalToPhysical(int logical, int totalLeds, int mode) {
  if (totalLeds <= 0) return 0;
  switch (mode) {
    case 0: // Distribute evenly across all physical LEDs
      return (int)round((float)logical * totalLeds / 60.0f) % totalLeds;
    case 1: // Use first min(totalLeds, 60) LEDs directly
      if (logical >= totalLeds) return -1; // -1 = skip
      return logical;
    case 2: // Use all LEDs, map 60 positions by spreading gaps uniformly
      return (int)round((float)logical * totalLeds / 60.0f) % totalLeds;
    default:
      return logical % totalLeds;
  }
}

// ─── Clock rendering ───────────────────────────────────────────────────────
void renderClock(int hour12, int minute) {
  // Clear all LEDs
  fill_solid(leds, cfg.numLeds, CRGB::Black);

  // Minute hand: exact minute position (0-59)
  int minPos = logicalToPhysical(minute % 60, cfg.numLeds, cfg.mappingMode);
  if (minPos >= 0 && minPos < cfg.numLeds) {
    CRGB minColor = CRGB(
      (cfg.minR * cfg.minBrightness) / 255,
      (cfg.minG * cfg.minBrightness) / 255,
      (cfg.minB * cfg.minBrightness) / 255
    );
    leds[minPos] = minColor;
  }

  // Hour hand: hour 1-12 mapped to 0-59 (each hour = 5 minute positions)
  // Plus fractional offset based on minutes
  float hourFrac   = (hour12 % 12) * 5.0f + (minute / 12.0f);
  int   hourLogical = (int)round(hourFrac) % 60;
  int   hourPos    = logicalToPhysical(hourLogical, cfg.numLeds, cfg.mappingMode);
  if (hourPos >= 0 && hourPos < cfg.numLeds) {
    CRGB hourColor = CRGB(
      (cfg.hourR * cfg.hourBrightness) / 255,
      (cfg.hourG * cfg.hourBrightness) / 255,
      (cfg.hourB * cfg.hourBrightness) / 255
    );
    // If hour and minute overlap, blend colors
    if (hourPos == minPos) {
      leds[hourPos] = blend(hourColor,
        CRGB((cfg.minR * cfg.minBrightness) / 255,
             (cfg.minG * cfg.minBrightness) / 255,
             (cfg.minB * cfg.minBrightness) / 255),
        128);
    } else {
      leds[hourPos] = hourColor;
    }
  }

  FastLED.show();
}

// ─── WiFi ──────────────────────────────────────────────────────────────────
void connectWiFi() {
  if (strlen(cfg.ssid) == 0) return;
  Serial.printf("Connecting to WiFi: %s\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi connection failed. Starting AP mode.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MarbleClock", "clock1234");
    Serial.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }
}

// ─── NTP sync ──────────────────────────────────────────────────────────────
void syncNTP() {
  if (!wifiConnected || !cfg.ntpEnabled) return;
  timeClient.setTimeOffset(cfg.utcOffsetSec);
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    ntpSynced = true;
    lastNTPSync = millis();
    Serial.printf("NTP synced: %s\n", timeClient.getFormattedTime().c_str());
  }
}

// ─── Web server ────────────────────────────────────────────────────────────
void setupWebServer() {
  // Serve static files from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // GET /api/status — returns current time and config as JSON
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["hour"]         = timeClient.getHours();
    doc["minute"]       = timeClient.getMinutes();
    doc["second"]       = timeClient.getSeconds();
    doc["ntpSynced"]    = ntpSynced;
    doc["wifiConnected"]= wifiConnected;
    doc["ip"]           = WiFi.localIP().toString();
    doc["ssid"]         = cfg.ssid;
    doc["numLeds"]      = cfg.numLeds;
    doc["brightness"]   = cfg.brightness;
    doc["hourR"]        = cfg.hourR;
    doc["hourG"]        = cfg.hourG;
    doc["hourB"]        = cfg.hourB;
    doc["minR"]         = cfg.minR;
    doc["minG"]         = cfg.minG;
    doc["minB"]         = cfg.minB;
    doc["hourBrightness"]= cfg.hourBrightness;
    doc["minBrightness"]= cfg.minBrightness;
    doc["utcOffsetSec"] = cfg.utcOffsetSec;
    doc["ntpEnabled"]   = cfg.ntpEnabled;
    doc["ledDensity"]   = cfg.ledDensity;
    doc["mappingMode"]  = cfg.mappingMode;
    doc["manualHour"]   = cfg.manualHour;
    doc["manualMinute"] = cfg.manualMinute;
    String json;
    serializeJson(doc, json);
    request->send(200, "application/json", json);
  });

  // POST /api/config — update configuration
  server.on("/api/config", HTTP_POST, [](AsyncWebServerRequest *request) {},
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, data, len);
      if (err) {
        request->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (doc["ssid"].is<const char*>())     strlcpy(cfg.ssid,     doc["ssid"],     sizeof(cfg.ssid));
      if (doc["password"].is<const char*>()) strlcpy(cfg.password, doc["password"], sizeof(cfg.password));
      if (doc["numLeds"].is<int>())          cfg.numLeds       = constrain((int)doc["numLeds"], 1, MAX_LEDS);
      if (doc["brightness"].is<int>())       cfg.brightness    = constrain((int)doc["brightness"], 0, 255);
      if (doc["hourR"].is<int>())            cfg.hourR         = (uint8_t)constrain((int)doc["hourR"], 0, 255);
      if (doc["hourG"].is<int>())            cfg.hourG         = (uint8_t)constrain((int)doc["hourG"], 0, 255);
      if (doc["hourB"].is<int>())            cfg.hourB         = (uint8_t)constrain((int)doc["hourB"], 0, 255);
      if (doc["minR"].is<int>())             cfg.minR          = (uint8_t)constrain((int)doc["minR"], 0, 255);
      if (doc["minG"].is<int>())             cfg.minG          = (uint8_t)constrain((int)doc["minG"], 0, 255);
      if (doc["minB"].is<int>())             cfg.minB          = (uint8_t)constrain((int)doc["minB"], 0, 255);
      if (doc["hourBrightness"].is<int>())   cfg.hourBrightness= constrain((int)doc["hourBrightness"], 0, 255);
      if (doc["minBrightness"].is<int>())    cfg.minBrightness = constrain((int)doc["minBrightness"], 0, 255);
      if (doc["utcOffsetSec"].is<int>())     cfg.utcOffsetSec  = (int)doc["utcOffsetSec"];
      if (doc["ntpEnabled"].is<bool>())      cfg.ntpEnabled    = (bool)doc["ntpEnabled"];
      if (doc["ledDensity"].is<int>())       cfg.ledDensity    = (int)doc["ledDensity"];
      if (doc["mappingMode"].is<int>())      cfg.mappingMode   = constrain((int)doc["mappingMode"], 0, 2);
      if (doc["manualHour"].is<int>())       cfg.manualHour    = constrain((int)doc["manualHour"], -1, 23);
      if (doc["manualMinute"].is<int>())     cfg.manualMinute  = constrain((int)doc["manualMinute"], 0, 59);
      // Apply global brightness to FastLED
      FastLED.setBrightness(cfg.brightness);
      saveConfig();
      // Re-init FastLED if numLeds changed
      request->send(200, "application/json", "{\"ok\":true}");
    }
  );

  // POST /api/syncntp — force NTP resync
  server.on("/api/syncntp", HTTP_POST, [](AsyncWebServerRequest *request) {
    syncNTP();
    request->send(200, "application/json", "{\"ok\":true}");
  });

  // POST /api/reconnect — reconnect WiFi with new credentials
  server.on("/api/reconnect", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
    delay(100);
    ESP.restart();
  });

  server.begin();
  Serial.println("Web server started.");
}

// ─── Setup ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Marble Ring Clock ===");

  loadConfig();

  // Init FastLED
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, MAX_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, cfg.numLeds, CRGB::Black);
  FastLED.show();

  // Init LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed!");
  } else {
    Serial.println("LittleFS OK");
  }

  connectWiFi();
  syncNTP();

  // OTA
  ArduinoOTA.setHostname("marble-clock");
  ArduinoOTA.begin();

  setupWebServer();

  // Boot animation: sweep LEDs
  for (int i = 0; i < cfg.numLeds; i++) {
    fill_solid(leds, cfg.numLeds, CRGB::Black);
    leds[i] = CRGB(0, 50, 80);
    FastLED.show();
    delay(20);
  }
  fill_solid(leds, cfg.numLeds, CRGB::Black);
  FastLED.show();
}

// ─── Loop ──────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();

  // NTP resync every hour
  if (wifiConnected && cfg.ntpEnabled &&
      (millis() - lastNTPSync > 3600000UL || lastNTPSync == 0)) {
    syncNTP();
  }

  // Update clock every second
  if (millis() - lastUpdate > 1000) {
    lastUpdate = millis();

    int h, m;
    if (cfg.manualHour >= 0) {
      // Manual time override
      h = cfg.manualHour;
      m = cfg.manualMinute;
    } else {
      timeClient.update();
      h = timeClient.getHours();
      m = timeClient.getMinutes();
    }

    renderClock(h, m);
  }
}
