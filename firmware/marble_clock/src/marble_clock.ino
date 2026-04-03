/*
 * Marble Ring Clock — ESP32
 * WS2812B LED ring clock with NTP sync and web configuration
 *
 * Supported boards:
 *   - Lolin32 Lite  → DATA_PIN 22 (default)
 *   - ESP32-C3 Mini → DATA_PIN 8  (set via -DDATA_PIN_OVERRIDE=8 in platformio.ini)
 *
 * Libraries:
 *   FastLED, ESPAsyncWebServer-esphome, AsyncTCP-esphome,
 *   NTPClient, ArduinoJson, LittleFS, Preferences, ArduinoOTA
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

// ─── Pin & LED defaults ──────────────────────────────────────────────────────
#ifndef DATA_PIN_OVERRIDE
  #define DATA_PIN 22   // Lolin32 Lite default
#else
  #define DATA_PIN DATA_PIN_OVERRIDE  // override da platformio.ini (es. 8 per C3)
#endif

#define MAX_LEDS         200
#define DEFAULT_NUM_LEDS  70
#define DEFAULT_BRIGHTNESS 80
#define LED_TYPE         WS2812B
#define COLOR_ORDER      GRB

// ─── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER       "pool.ntp.org"
#define DEFAULT_UTC_OFFSET 3600  // UTC+1 CET

// ─── Serial: su C3 con USB-CDC usiamo Serial (mappato su USB), ok su tutti ───
#define DBG Serial

// ─── Oggetti globali ─────────────────────────────────────────────────────────
CRGB leds[MAX_LEDS];
Preferences prefs;
AsyncWebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, NTP_SERVER);

// ─── Struttura configurazione ────────────────────────────────────────────────
struct Config {
  char    ssid[64];
  char    password[64];
  int     numLeds;
  int     brightness;
  uint8_t hourR, hourG, hourB;
  uint8_t minR,  minG,  minB;
  int     hourBrightness;
  int     minBrightness;
  int     utcOffsetSec;
  int     manualHour;     // -1 = usa NTP
  int     manualMinute;
  bool    ntpEnabled;
  int     ledDensity;
  int     mappingMode;    // 0=distribuiti, 1=primi 60, 2=tutti con gap
} cfg;

// ─── Stato runtime ───────────────────────────────────────────────────────────
bool wifiConnected = false;
bool ntpSynced     = false;
unsigned long lastNTPSync = 0;
unsigned long lastUpdate  = 0;

// ─── Persistenza config ──────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("clock", true);
  strlcpy(cfg.ssid,     prefs.getString("ssid",     "").c_str(), sizeof(cfg.ssid));
  strlcpy(cfg.password, prefs.getString("pass",     "").c_str(), sizeof(cfg.password));
  cfg.numLeds        = prefs.getInt("numLeds",    DEFAULT_NUM_LEDS);
  cfg.brightness     = prefs.getInt("bri",        DEFAULT_BRIGHTNESS);
  cfg.hourR          = prefs.getUChar("hR",        255);
  cfg.hourG          = prefs.getUChar("hG",        80);
  cfg.hourB          = prefs.getUChar("hB",        0);
  cfg.minR           = prefs.getUChar("mR",        0);
  cfg.minG           = prefs.getUChar("mG",        180);
  cfg.minB           = prefs.getUChar("mB",        255);
  cfg.hourBrightness = prefs.getInt("hBri",        200);
  cfg.minBrightness  = prefs.getInt("mBri",        255);
  cfg.utcOffsetSec   = prefs.getInt("utcOff",      DEFAULT_UTC_OFFSET);
  cfg.manualHour     = prefs.getInt("manHour",     -1);
  cfg.manualMinute   = prefs.getInt("manMin",      0);
  cfg.ntpEnabled     = prefs.getBool("ntpEn",      true);
  cfg.ledDensity     = prefs.getInt("density",     60);
  cfg.mappingMode    = prefs.getInt("mapMode",     0);
  prefs.end();
}

void saveConfig() {
  prefs.begin("clock", false);
  prefs.putString("ssid",    cfg.ssid);
  prefs.putString("pass",    cfg.password);
  prefs.putInt("numLeds",    cfg.numLeds);
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

// ─── LED mapping ─────────────────────────────────────────────────────────────
int logicalToPhysical(int logical, int totalLeds, int mode) {
  if (totalLeds <= 0) return 0;
  switch (mode) {
    case 1:  // Usa i primi min(totalLeds,60) LED direttamente
      return (logical >= totalLeds) ? -1 : logical;
    case 0:  // Distribuiti uniformemente (default)
    case 2:
    default:
      return (int)round((float)logical * totalLeds / 60.0f) % totalLeds;
  }
}

// ─── Rendering orologio ──────────────────────────────────────────────────────
void renderClock(int hour24, int minute) {
  fill_solid(leds, cfg.numLeds, CRGB::Black);

  // Lancetta minuti
  int minPos = logicalToPhysical(minute % 60, cfg.numLeds, cfg.mappingMode);
  if (minPos >= 0 && minPos < cfg.numLeds) {
    leds[minPos] = CRGB(
      (cfg.minR * cfg.minBrightness) / 255,
      (cfg.minG * cfg.minBrightness) / 255,
      (cfg.minB * cfg.minBrightness) / 255
    );
  }

  // Lancetta ore (con offset frazioni di ora)
  float hourFrac    = (hour24 % 12) * 5.0f + (minute / 12.0f);
  int   hourLogical = (int)round(hourFrac) % 60;
  int   hourPos     = logicalToPhysical(hourLogical, cfg.numLeds, cfg.mappingMode);
  if (hourPos >= 0 && hourPos < cfg.numLeds) {
    CRGB hc = CRGB(
      (cfg.hourR * cfg.hourBrightness) / 255,
      (cfg.hourG * cfg.hourBrightness) / 255,
      (cfg.hourB * cfg.hourBrightness) / 255
    );
    if (hourPos == minPos) {
      // Sovrapposizione: blend 50/50
      leds[hourPos] = blend(hc, leds[minPos], 128);
    } else {
      leds[hourPos] = hc;
    }
  }

  FastLED.show();
}

// ─── WiFi ─────────────────────────────────────────────────────────────────────
void connectWiFi() {
  if (strlen(cfg.ssid) == 0) {
    DBG.println("No SSID configured, starting AP mode.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MarbleClock", "clock1234");
    DBG.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    return;
  }
  DBG.printf("Connecting to WiFi: %s\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    DBG.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    DBG.printf("\nWiFi OK! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    DBG.println("\nWiFi failed. Starting AP mode.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MarbleClock", "clock1234");
    DBG.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }
}

// ─── NTP sync ────────────────────────────────────────────────────────────────
void syncNTP() {
  if (!wifiConnected || !cfg.ntpEnabled) return;
  timeClient.setTimeOffset(cfg.utcOffsetSec);
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    ntpSynced   = true;
    lastNTPSync = millis();
    DBG.printf("NTP synced: %s\n", timeClient.getFormattedTime().c_str());
  } else {
    DBG.println("NTP sync failed.");
  }
}

// ─── Web server ───────────────────────────────────────────────────────────────
void setupWebServer() {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // GET /api/status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["hour"]          = timeClient.getHours();
    doc["minute"]        = timeClient.getMinutes();
    doc["second"]        = timeClient.getSeconds();
    doc["ntpSynced"]     = ntpSynced;
    doc["wifiConnected"] = wifiConnected;
    doc["ip"]            = WiFi.localIP().toString();
    doc["ssid"]          = cfg.ssid;
    doc["numLeds"]       = cfg.numLeds;
    doc["brightness"]    = cfg.brightness;
    doc["hourR"]         = cfg.hourR;
    doc["hourG"]         = cfg.hourG;
    doc["hourB"]         = cfg.hourB;
    doc["minR"]          = cfg.minR;
    doc["minG"]          = cfg.minG;
    doc["minB"]          = cfg.minB;
    doc["hourBrightness"]= cfg.hourBrightness;
    doc["minBrightness"] = cfg.minBrightness;
    doc["utcOffsetSec"]  = cfg.utcOffsetSec;
    doc["ntpEnabled"]    = cfg.ntpEnabled;
    doc["ledDensity"]    = cfg.ledDensity;
    doc["mappingMode"]   = cfg.mappingMode;
    doc["manualHour"]    = cfg.manualHour;
    doc["manualMinute"]  = cfg.manualMinute;
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  // POST /api/config
  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
      }
      if (doc["ssid"].is<const char*>())     strlcpy(cfg.ssid,     doc["ssid"],     sizeof(cfg.ssid));
      if (doc["password"].is<const char*>()) strlcpy(cfg.password, doc["password"], sizeof(cfg.password));
      if (doc["numLeds"].is<int>())          cfg.numLeds        = constrain((int)doc["numLeds"], 1, MAX_LEDS);
      if (doc["brightness"].is<int>())       cfg.brightness     = constrain((int)doc["brightness"], 0, 255);
      if (doc["hourR"].is<int>())            cfg.hourR          = constrain((int)doc["hourR"], 0, 255);
      if (doc["hourG"].is<int>())            cfg.hourG          = constrain((int)doc["hourG"], 0, 255);
      if (doc["hourB"].is<int>())            cfg.hourB          = constrain((int)doc["hourB"], 0, 255);
      if (doc["minR"].is<int>())             cfg.minR           = constrain((int)doc["minR"], 0, 255);
      if (doc["minG"].is<int>())             cfg.minG           = constrain((int)doc["minG"], 0, 255);
      if (doc["minB"].is<int>())             cfg.minB           = constrain((int)doc["minB"], 0, 255);
      if (doc["hourBrightness"].is<int>())   cfg.hourBrightness = constrain((int)doc["hourBrightness"], 0, 255);
      if (doc["minBrightness"].is<int>())    cfg.minBrightness  = constrain((int)doc["minBrightness"], 0, 255);
      if (doc["utcOffsetSec"].is<int>())     cfg.utcOffsetSec   = (int)doc["utcOffsetSec"];
      if (doc["ntpEnabled"].is<bool>())      cfg.ntpEnabled     = (bool)doc["ntpEnabled"];
      if (doc["ledDensity"].is<int>())       cfg.ledDensity     = (int)doc["ledDensity"];
      if (doc["mappingMode"].is<int>())      cfg.mappingMode    = constrain((int)doc["mappingMode"], 0, 2);
      if (doc["manualHour"].is<int>())       cfg.manualHour     = constrain((int)doc["manualHour"], -1, 23);
      if (doc["manualMinute"].is<int>())     cfg.manualMinute   = constrain((int)doc["manualMinute"], 0, 59);
      FastLED.setBrightness(cfg.brightness);
      saveConfig();
      req->send(200, "application/json", "{\"ok\":true}");
    }
  );

  // POST /api/syncntp
  server.on("/api/syncntp", HTTP_POST, [](AsyncWebServerRequest *req) {
    syncNTP();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // POST /api/reconnect — salva e riavvia
  server.on("/api/reconnect", HTTP_POST, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });

  server.begin();
  DBG.println("Web server started.");
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  // Su ESP32-C3 con -DARDUINO_USB_CDC_ON_BOOT=1, Serial usa USB-CDC nativamente
  DBG.begin(115200);
  delay(500);
  DBG.println("\n=== Marble Ring Clock ===");
  DBG.printf("DATA_PIN = %d\n", DATA_PIN);

  loadConfig();

  // Init FastLED
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, MAX_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, cfg.numLeds, CRGB::Black);
  FastLED.show();

  // Init LittleFS
  if (!LittleFS.begin(true)) {
    DBG.println("LittleFS mount failed!");
  } else {
    DBG.println("LittleFS OK");
  }

  connectWiFi();
  syncNTP();

  ArduinoOTA.setHostname("marble-clock");
  ArduinoOTA.begin();

  setupWebServer();

  // Animazione di avvio: sweep
  for (int i = 0; i < cfg.numLeds; i++) {
    fill_solid(leds, cfg.numLeds, CRGB::Black);
    leds[i] = CRGB(0, 50, 80);
    FastLED.show();
    delay(20);
  }
  fill_solid(leds, cfg.numLeds, CRGB::Black);
  FastLED.show();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();

  // Resync NTP ogni ora
  if (wifiConnected && cfg.ntpEnabled &&
      (millis() - lastNTPSync > 3600000UL || lastNTPSync == 0)) {
    syncNTP();
  }

  // Aggiorna display ogni secondo
  if (millis() - lastUpdate > 1000) {
    lastUpdate = millis();
    int h, m;
    if (cfg.manualHour >= 0) {
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
