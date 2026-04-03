/*
 * Marble Ring Clock — ESP32
 * WS2812B / SK6812 LED ring clock with NTP sync and web configuration
 *
 * Board: Lolin32 Lite (target) / ESP32-C3 Mini (test)
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

// ─── Serial ──────────────────────────────────────────────────────────────────
#define DBG Serial0

// ─── Pin ─────────────────────────────────────────────────────────────────────
#ifndef DATA_PIN_OVERRIDE
  #define DATA_PIN 5    // Lolin32 Lite default (GPIO5)
#else
  #define DATA_PIN DATA_PIN_OVERRIDE
#endif

// ─── LED type selector ───────────────────────────────────────────────────────
// LED_MODEL: 0 = WS2812B (GRB), 1 = SK6812 (GRBW)
#define MAX_LEDS          200
#define DEFAULT_NUM_LEDS   70
#define DEFAULT_BRIGHTNESS 80

// ─── NTP ─────────────────────────────────────────────────────────────────────
#define NTP_SERVER        "pool.ntp.org"
#define DEFAULT_UTC_OFFSET 3600  // UTC+1 CET

// ─── Oggetti globali ─────────────────────────────────────────────────────────
CRGB  leds_rgb[MAX_LEDS];   // WS2812B
CRGBW leds_rgbw[MAX_LEDS];  // SK6812
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
  int     manualHour;    // -1 = usa NTP
  int     manualMinute;
  bool    ntpEnabled;
  int     ledDensity;
  int     mappingMode;   // 0=distribuiti, 1=primi 60, 2=tutti con gap
  int     ledModel;      // 0=WS2812B, 1=SK6812
} cfg;

// ─── Stato runtime ───────────────────────────────────────────────────────────
bool wifiConnected = false;
bool ntpSynced     = false;
bool littlefsOK    = false;
unsigned long lastNTPSync = 0;
unsigned long lastUpdate  = 0;

// ─── Persistenza config ──────────────────────────────────────────────────────
void loadConfig() {
  prefs.begin("clock", true);
  strlcpy(cfg.ssid,     prefs.getString("ssid", "").c_str(), sizeof(cfg.ssid));
  strlcpy(cfg.password, prefs.getString("pass", "").c_str(), sizeof(cfg.password));
  cfg.numLeds        = prefs.getInt("numLeds",   DEFAULT_NUM_LEDS);
  cfg.brightness     = prefs.getInt("bri",       DEFAULT_BRIGHTNESS);
  cfg.hourR          = prefs.getUChar("hR",       255);
  cfg.hourG          = prefs.getUChar("hG",       80);
  cfg.hourB          = prefs.getUChar("hB",       0);
  cfg.minR           = prefs.getUChar("mR",       0);
  cfg.minG           = prefs.getUChar("mG",       180);
  cfg.minB           = prefs.getUChar("mB",       255);
  cfg.hourBrightness = prefs.getInt("hBri",       200);
  cfg.minBrightness  = prefs.getInt("mBri",       255);
  cfg.utcOffsetSec   = prefs.getInt("utcOff",     DEFAULT_UTC_OFFSET);
  cfg.manualHour     = prefs.getInt("manHour",    -1);
  cfg.manualMinute   = prefs.getInt("manMin",     0);
  cfg.ntpEnabled     = prefs.getBool("ntpEn",     true);
  cfg.ledDensity     = prefs.getInt("density",    60);
  cfg.mappingMode    = prefs.getInt("mapMode",    0);
  cfg.ledModel       = prefs.getInt("ledModel",   0);  // 0=WS2812B default
  prefs.end();
}

void saveConfig() {
  prefs.begin("clock", false);
  prefs.putString("ssid",  cfg.ssid);
  prefs.putString("pass",  cfg.password);
  prefs.putInt("numLeds",  cfg.numLeds);
  prefs.putInt("bri",      cfg.brightness);
  prefs.putUChar("hR",     cfg.hourR);
  prefs.putUChar("hG",     cfg.hourG);
  prefs.putUChar("hB",     cfg.hourB);
  prefs.putUChar("mR",     cfg.minR);
  prefs.putUChar("mG",     cfg.minG);
  prefs.putUChar("mB",     cfg.minB);
  prefs.putInt("hBri",     cfg.hourBrightness);
  prefs.putInt("mBri",     cfg.minBrightness);
  prefs.putInt("utcOff",   cfg.utcOffsetSec);
  prefs.putInt("manHour",  cfg.manualHour);
  prefs.putInt("manMin",   cfg.manualMinute);
  prefs.putBool("ntpEn",   cfg.ntpEnabled);
  prefs.putInt("density",  cfg.ledDensity);
  prefs.putInt("mapMode",  cfg.mappingMode);
  prefs.putInt("ledModel", cfg.ledModel);
  prefs.end();
}

// ─── FastLED helpers ─────────────────────────────────────────────────────────
void ledsShow() {
  FastLED.show();
}

void ledsFill(CRGB color) {
  if (cfg.ledModel == 1) {
    CRGBW c(color.r, color.g, color.b, 0);
    for (int i = 0; i < cfg.numLeds; i++) leds_rgbw[i] = c;
  } else {
    fill_solid(leds_rgb, cfg.numLeds, color);
  }
}

void ledSet(int i, CRGB color) {
  if (i < 0 || i >= cfg.numLeds) return;
  if (cfg.ledModel == 1) {
    leds_rgbw[i] = CRGBW(color.r, color.g, color.b, 0);
  } else {
    leds_rgb[i] = color;
  }
}

CRGB ledGet(int i) {
  if (i < 0 || i >= cfg.numLeds) return CRGB::Black;
  if (cfg.ledModel == 1) {
    return CRGB(leds_rgbw[i].r, leds_rgbw[i].g, leds_rgbw[i].b);
  } else {
    return leds_rgb[i];
  }
}

// ─── LED mapping ─────────────────────────────────────────────────────────────
int logicalToPhysical(int logical, int totalLeds, int mode) {
  if (totalLeds <= 0) return 0;
  switch (mode) {
    case 1:
      return (logical >= totalLeds) ? -1 : logical;
    case 0:
    case 2:
    default:
      return (int)round((float)logical * totalLeds / 60.0f) % totalLeds;
  }
}

// ─── Rendering orologio ──────────────────────────────────────────────────────
void renderClock(int hour24, int minute) {
  ledsFill(CRGB::Black);

  int minPos = logicalToPhysical(minute % 60, cfg.numLeds, cfg.mappingMode);
  CRGB mc(
    (cfg.minR * cfg.minBrightness) / 255,
    (cfg.minG * cfg.minBrightness) / 255,
    (cfg.minB * cfg.minBrightness) / 255
  );
  ledSet(minPos, mc);

  float hourFrac    = (hour24 % 12) * 5.0f + (minute / 12.0f);
  int   hourLogical = (int)round(hourFrac) % 60;
  int   hourPos     = logicalToPhysical(hourLogical, cfg.numLeds, cfg.mappingMode);
  CRGB hc(
    (cfg.hourR * cfg.hourBrightness) / 255,
    (cfg.hourG * cfg.hourBrightness) / 255,
    (cfg.hourB * cfg.hourBrightness) / 255
  );
  if (hourPos == minPos) {
    ledSet(hourPos, blend(hc, mc, 128));
  } else {
    ledSet(hourPos, hc);
  }

  ledsShow();
}

// ─── WiFi ────────────────────────────────────────────────────────────────────
void connectWiFi() {
  if (strlen(cfg.ssid) == 0) {
    DBG.println("No SSID → AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MarbleClock", "clock1234");
    DBG.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
    return;
  }
  DBG.printf("Connecting to: %s\n", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid, cfg.password);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); DBG.print("."); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    DBG.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    DBG.println("\nFailed → AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MarbleClock", "clock1234");
    DBG.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }
}

// ─── NTP ─────────────────────────────────────────────────────────────────────
void syncNTP() {
  if (!wifiConnected || !cfg.ntpEnabled) return;
  timeClient.setTimeOffset(cfg.utcOffsetSec);
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    ntpSynced = true; lastNTPSync = millis();
    DBG.printf("NTP: %s\n", timeClient.getFormattedTime().c_str());
  } else {
    DBG.println("NTP failed");
  }
}

// ─── HTML inline di fallback (usato se LittleFS non ha index.html) ───────────
// Permette di configurare WiFi anche senza uploadfs
static const char FALLBACK_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Marble Clock</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:sans-serif;background:#111;color:#eee;padding:20px}
h1{color:#4fc3f7;margin-bottom:20px}h2{color:#aaa;margin:16px 0 8px}
input,select{width:100%;padding:8px;margin:4px 0 12px;background:#222;border:1px solid #444;color:#eee;border-radius:6px}
button{padding:10px 20px;background:#01696f;color:#fff;border:none;border-radius:6px;cursor:pointer;width:100%;margin:4px 0}
button:hover{background:#0c4e54}
.row{display:flex;gap:8px}.row input{flex:1}
.status{background:#1a1a1a;border-radius:8px;padding:12px;margin-bottom:16px;font-size:14px}
.ok{color:#6daa45}.err{color:#dd6974}
</style></head><body>
<h1>&#9679; Marble Clock</h1>
<div class='status' id='st'>Caricamento...</div>
<h2>WiFi</h2>
<input id='ssid' placeholder='SSID'>
<input id='pass' type='password' placeholder='Password'>
<h2>LED</h2>
<label>Tipo LED</label>
<select id='ledModel'><option value='0'>WS2812B</option><option value='1'>SK6812 (RGBW)</option></select>
<label>Numero LED</label><input id='numLeds' type='number' min='1' max='200'>
<label>Luminosit&agrave; globale (0-255)</label><input id='brightness' type='range' min='0' max='255'>
<h2>Colore Ore</h2>
<div class='row'><input id='hR' type='number' min='0' max='255' placeholder='R'><input id='hG' type='number' min='0' max='255' placeholder='G'><input id='hB' type='number' min='0' max='255' placeholder='B'></div>
<label>Intensit&agrave; lancetta ore</label><input id='hBri' type='range' min='0' max='255'>
<h2>Colore Minuti</h2>
<div class='row'><input id='mR' type='number' min='0' max='255' placeholder='R'><input id='mG' type='number' min='0' max='255' placeholder='G'><input id='mB' type='number' min='0' max='255' placeholder='B'></div>
<label>Intensit&agrave; lancetta minuti</label><input id='mBri' type='range' min='0' max='255'>
<h2>Orario</h2>
<label>Fuso orario (offset UTC in secondi, es. 3600=CET)</label>
<input id='utcOff' type='number'>
<label>Ora manuale (-1 = usa NTP)</label>
<div class='row'><input id='manH' type='number' min='-1' max='23' placeholder='Ora (-1=NTP)'><input id='manM' type='number' min='0' max='59' placeholder='Min'></div>
<h2>Mapping LED</h2>
<select id='mapMode'><option value='0'>Distribuiti (consigliato)</option><option value='1'>Primi 60</option><option value='2'>Tutti con gap</option></select>
<br>
<button onclick='save()'>&#128190; Salva &amp; Applica</button>
<button onclick='syncNTP()'>&#128337; Sync NTP</button>
<button onclick='reboot()'>&#128260; Riavvia</button>
<script>
async function load(){
  try{
    const r=await fetch('/api/status');const d=await r.json();
    document.getElementById('st').innerHTML=
      `WiFi: <span class='${d.wifiConnected?"ok":"err"}'>${d.wifiConnected?d.ssid+' ('+d.ip+')':'non connesso'}</span> | NTP: <span class='${d.ntpSynced?"ok":"err"}'>${d.ntpSynced?d.hour+':'+String(d.minute).padStart(2,'0'):'non sync'}</span>`;
    document.getElementById('ssid').value=d.ssid||'';
    document.getElementById('numLeds').value=d.numLeds;
    document.getElementById('brightness').value=d.brightness;
    document.getElementById('hR').value=d.hourR;document.getElementById('hG').value=d.hourG;document.getElementById('hB').value=d.hourB;
    document.getElementById('mR').value=d.minR;document.getElementById('mG').value=d.minG;document.getElementById('mB').value=d.minB;
    document.getElementById('hBri').value=d.hourBrightness;
    document.getElementById('mBri').value=d.minBrightness;
    document.getElementById('utcOff').value=d.utcOffsetSec;
    document.getElementById('manH').value=d.manualHour;
    document.getElementById('manM').value=d.manualMinute;
    document.getElementById('mapMode').value=d.mappingMode;
    document.getElementById('ledModel').value=d.ledModel||0;
  }catch(e){document.getElementById('st').innerHTML="<span class='err'>Errore connessione</span>";}
}
async function save(){
  const body={ssid:document.getElementById('ssid').value,password:document.getElementById('pass').value,
    numLeds:+document.getElementById('numLeds').value,brightness:+document.getElementById('brightness').value,
    hourR:+document.getElementById('hR').value,hourG:+document.getElementById('hG').value,hourB:+document.getElementById('hB').value,
    minR:+document.getElementById('mR').value,minG:+document.getElementById('mG').value,minB:+document.getElementById('mB').value,
    hourBrightness:+document.getElementById('hBri').value,minBrightness:+document.getElementById('mBri').value,
    utcOffsetSec:+document.getElementById('utcOff').value,
    manualHour:+document.getElementById('manH').value,manualMinute:+document.getElementById('manM').value,
    mappingMode:+document.getElementById('mapMode').value,ledModel:+document.getElementById('ledModel').value};
  await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  alert('Salvato!');
}
async function syncNTP(){await fetch('/api/syncntp',{method:'POST'});alert('NTP sync avviato');}
async function reboot(){await fetch('/api/reconnect',{method:'POST'});alert('Riavvio...');}
load();
</script></body></html>
)rawhtml";

// ─── Web server ───────────────────────────────────────────────────────────────
void setupWebServer() {
  // Serve LittleFS se disponibile, altrimenti usa fallback HTML inline
  if (littlefsOK) {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  }

  // Fallback root — risponde sempre, anche senza LittleFS
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!littlefsOK || !LittleFS.exists("/index.html")) {
      req->send_P(200, "text/html", FALLBACK_HTML);
    } else {
      req->send(LittleFS, "/index.html", "text/html");
    }
  });

  // GET /api/status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["hour"]           = timeClient.getHours();
    doc["minute"]         = timeClient.getMinutes();
    doc["second"]         = timeClient.getSeconds();
    doc["ntpSynced"]      = ntpSynced;
    doc["wifiConnected"]  = wifiConnected;
    doc["ip"]             = wifiConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    doc["ssid"]           = cfg.ssid;
    doc["numLeds"]        = cfg.numLeds;
    doc["brightness"]     = cfg.brightness;
    doc["hourR"]          = cfg.hourR;
    doc["hourG"]          = cfg.hourG;
    doc["hourB"]          = cfg.hourB;
    doc["minR"]           = cfg.minR;
    doc["minG"]           = cfg.minG;
    doc["minB"]           = cfg.minB;
    doc["hourBrightness"] = cfg.hourBrightness;
    doc["minBrightness"]  = cfg.minBrightness;
    doc["utcOffsetSec"]   = cfg.utcOffsetSec;
    doc["ntpEnabled"]     = cfg.ntpEnabled;
    doc["ledDensity"]     = cfg.ledDensity;
    doc["mappingMode"]    = cfg.mappingMode;
    doc["manualHour"]     = cfg.manualHour;
    doc["manualMinute"]   = cfg.manualMinute;
    doc["ledModel"]       = cfg.ledModel;
    String json; serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  // POST /api/config
  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"Invalid JSON\"}"); return;
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
      if (doc["ledModel"].is<int>())         cfg.ledModel       = constrain((int)doc["ledModel"], 0, 1);
      FastLED.setBrightness(cfg.brightness);
      saveConfig();
      req->send(200, "application/json", "{\"ok\":true}");
    }
  );

  server.on("/api/syncntp", HTTP_POST, [](AsyncWebServerRequest *req) {
    syncNTP();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/reconnect", HTTP_POST, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200); ESP.restart();
  });

  server.begin();
  DBG.println("Web server started.");
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  DBG.begin(115200);
  delay(500);
  DBG.println("\n=== Marble Ring Clock ===");
  DBG.printf("DATA_PIN = %d\n", DATA_PIN);

  loadConfig();

  // Inizializza FastLED in base al modello LED
  if (cfg.ledModel == 1) {
    FastLED.addLeds<SK6812, DATA_PIN, RGB>(leds_rgbw, MAX_LEDS)
           .setCorrection(TypicalLEDStrip);
    DBG.println("LED: SK6812 (RGBW)");
  } else {
    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds_rgb, MAX_LEDS)
           .setCorrection(TypicalLEDStrip);
    DBG.println("LED: WS2812B (GRB)");
  }
  FastLED.setBrightness(cfg.brightness);
  ledsFill(CRGB::Black);
  ledsShow();

  littlefsOK = LittleFS.begin(true);
  if (!littlefsOK) {
    DBG.println("LittleFS FAILED — using inline HTML fallback");
  } else {
    DBG.println("LittleFS OK");
    if (!LittleFS.exists("/index.html")) {
      DBG.println("index.html not found — using inline fallback");
    }
  }

  connectWiFi();
  syncNTP();

  ArduinoOTA.setHostname("marble-clock");
  ArduinoOTA.begin();

  setupWebServer();

  // Animazione avvio
  for (int i = 0; i < cfg.numLeds; i++) {
    ledsFill(CRGB::Black);
    ledSet(i, CRGB(0, 50, 80));
    ledsShow();
    delay(20);
  }
  ledsFill(CRGB::Black);
  ledsShow();

  DBG.println("Setup done.");
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  ArduinoOTA.handle();

  if (wifiConnected && cfg.ntpEnabled &&
      (millis() - lastNTPSync > 3600000UL || lastNTPSync == 0)) {
    syncNTP();
  }

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
