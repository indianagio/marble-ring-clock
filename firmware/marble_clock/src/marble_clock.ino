/*
 * Marble Ring Clock - ESP32
 * WS2812B / SK6812 LED ring clock with NTP sync and web configuration
 *
 * Board: Lolin32 Lite (target) / ESP32-C3 Mini (test)
 *
 * Fix log:
 *  - utcOffsetSec applicato subito senza reboot (timeClient reinizializzato)
 *  - ledOffset (rotazione anello) configurabile da webapp
 *  - LED 0 = ore 12 per default (offset 0)
 *  - ledSkip: salta N LED all'inizio della striscia (es. LED onboard ESP32-C3)
 *  - DATA_PIN default = 8 (WS2812 onboard ESP32-C3 Mini)
 *  - DEFAULT_NUM_LEDS = 40 (anello di test)
 *  - ledReverse: inverte la direzione di scorrimento dell'anello
 *  - ledModel modificabile dalla webapp (richiede reboot per reinit FastLED)
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

#define DBG Serial0

#ifndef DATA_PIN_OVERRIDE
  #define DATA_PIN 8
#else
  #define DATA_PIN DATA_PIN_OVERRIDE
#endif

#define MAX_LEDS           200
#define DEFAULT_NUM_LEDS    40
#define DEFAULT_BRIGHTNESS  80
#define DEFAULT_LED_SKIP     1
#define NTP_SERVER         "pool.ntp.org"
#define DEFAULT_UTC_OFFSET  3600

CRGB leds[MAX_LEDS];

Preferences    prefs;
AsyncWebServer server(80);
WiFiUDP        ntpUDP;
NTPClient      timeClient(ntpUDP, NTP_SERVER);

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
  int     manualHour;
  int     manualMinute;
  bool    ntpEnabled;
  int     ledDensity;
  int     mappingMode;  // 0=spread, 1=first60, 2=all-with-gap
  int     ledModel;     // 0=WS2812B, 1=SK6812
  int     ledOffset;
  int     ledSkip;
  bool    ledReverse;   // true = inverte la direzione (LED verso parete)
} cfg;

bool wifiConnected = false;
bool ntpSynced     = false;
bool littlefsOK    = false;
unsigned long lastNTPSync = 0;
unsigned long lastUpdate  = 0;

// --- Config ------------------------------------------------------------------
void loadConfig() {
  prefs.begin("clock", true);
  strlcpy(cfg.ssid,     prefs.getString("ssid", "").c_str(), sizeof(cfg.ssid));
  strlcpy(cfg.password, prefs.getString("pass", "").c_str(), sizeof(cfg.password));
  cfg.numLeds        = prefs.getInt("numLeds",   DEFAULT_NUM_LEDS);
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
  cfg.ledModel       = prefs.getInt("ledModel",    0);
  cfg.ledOffset      = prefs.getInt("ledOffset",   0);
  cfg.ledSkip        = prefs.getInt("ledSkip",     DEFAULT_LED_SKIP);
  cfg.ledReverse     = prefs.getBool("ledReverse", false);
  prefs.end();
}

void saveConfig() {
  prefs.begin("clock", false);
  prefs.putString("ssid",     cfg.ssid);
  prefs.putString("pass",     cfg.password);
  prefs.putInt("numLeds",     cfg.numLeds);
  prefs.putInt("bri",         cfg.brightness);
  prefs.putUChar("hR",        cfg.hourR);
  prefs.putUChar("hG",        cfg.hourG);
  prefs.putUChar("hB",        cfg.hourB);
  prefs.putUChar("mR",        cfg.minR);
  prefs.putUChar("mG",        cfg.minG);
  prefs.putUChar("mB",        cfg.minB);
  prefs.putInt("hBri",        cfg.hourBrightness);
  prefs.putInt("mBri",        cfg.minBrightness);
  prefs.putInt("utcOff",      cfg.utcOffsetSec);
  prefs.putInt("manHour",     cfg.manualHour);
  prefs.putInt("manMin",      cfg.manualMinute);
  prefs.putBool("ntpEn",      cfg.ntpEnabled);
  prefs.putInt("density",     cfg.ledDensity);
  prefs.putInt("mapMode",     cfg.mappingMode);
  prefs.putInt("ledModel",    cfg.ledModel);
  prefs.putInt("ledOffset",   cfg.ledOffset);
  prefs.putInt("ledSkip",     cfg.ledSkip);
  prefs.putBool("ledReverse", cfg.ledReverse);
  prefs.end();
}

// --- LED mapping -------------------------------------------------------------
// logicalToPhysical: mappa posizione logica (0-59) -> indice fisico nel buffer
//
// ledSkip   : LED fisici iniziali sempre spenti (es. LED onboard C3)
// ledOffset : rotazione anello (quale LED = ore 12)
// ledReverse: inverte la direzione di numerazione dei LED dell'anello
//             utile quando la striscia e' montata "verso la parete" e i LED
//             scorrono in senso antiorario anziche' orario
int logicalToPhysical(int logical, int totalLeds, int mode, int offset, int skip, bool reverse) {
  if (totalLeds <= 0) return -1;
  int pos;
  switch (mode) {
    case 1:
      pos = logical;
      if (pos >= totalLeds) return -1;
      break;
    case 0:
    case 2:
    default:
      pos = (int)round((float)logical * totalLeds / 60.0f) % totalLeds;
      break;
  }
  // Applica rotazione
  pos = (pos + offset) % totalLeds;
  // Inversione: rispecchia rispetto al numero totale di LED dell'anello
  if (reverse) pos = (totalLeds - pos) % totalLeds;
  return skip + pos;
}

// --- Clock rendering ---------------------------------------------------------
void renderClock(int hour24, int minute) {
  fill_solid(leds, cfg.ledSkip + cfg.numLeds, CRGB::Black);

  int minPos = logicalToPhysical(minute % 60, cfg.numLeds, cfg.mappingMode,
                                  cfg.ledOffset, cfg.ledSkip, cfg.ledReverse);
  CRGB mc(
    (cfg.minR  * cfg.minBrightness)  / 255,
    (cfg.minG  * cfg.minBrightness)  / 255,
    (cfg.minB  * cfg.minBrightness)  / 255
  );
  if (minPos >= cfg.ledSkip && minPos < cfg.ledSkip + cfg.numLeds)
    leds[minPos] = mc;

  float hourFrac    = (hour24 % 12) * 5.0f + (minute / 12.0f);
  int   hourLogical = (int)round(hourFrac) % 60;
  int   hourPos     = logicalToPhysical(hourLogical, cfg.numLeds, cfg.mappingMode,
                                         cfg.ledOffset, cfg.ledSkip, cfg.ledReverse);
  CRGB hc(
    (cfg.hourR * cfg.hourBrightness) / 255,
    (cfg.hourG * cfg.hourBrightness) / 255,
    (cfg.hourB * cfg.hourBrightness) / 255
  );
  if (hourPos >= cfg.ledSkip && hourPos < cfg.ledSkip + cfg.numLeds) {
    leds[hourPos] = (hourPos == minPos) ? blend(hc, mc, 128) : hc;
  }

  FastLED.show();
}

// --- FastLED init ------------------------------------------------------------
// Chiamata all'avvio e ogni volta che ledModel cambia dalla webapp.
// FastLED non supporta il cambio dinamico del tipo di LED senza reboot,
// quindi salviamo e facciamo restart.
void initFastLED() {
  if (cfg.ledModel == 1) {
    FastLED.addLeds<SK6812, DATA_PIN, GRB>(leds, MAX_LEDS).setCorrection(TypicalLEDStrip);
    DBG.println("LED: SK6812");
  } else {
    FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, MAX_LEDS).setCorrection(TypicalLEDStrip);
    DBG.println("LED: WS2812B");
  }
  FastLED.setBrightness(cfg.brightness);
}

// --- WiFi --------------------------------------------------------------------
void connectWiFi() {
  if (strlen(cfg.ssid) == 0) {
    DBG.println("No SSID - starting AP mode");
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
    DBG.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    DBG.println("\nWiFi failed - AP mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("MarbleClock", "clock1234");
    DBG.printf("AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  }
}

// --- NTP ---------------------------------------------------------------------
void syncNTP() {
  if (!wifiConnected || !cfg.ntpEnabled) return;
  timeClient.end();
  timeClient.setTimeOffset(cfg.utcOffsetSec);
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    ntpSynced   = true;
    lastNTPSync = millis();
    DBG.printf("NTP: %s (offset %ds)\n",
      timeClient.getFormattedTime().c_str(), cfg.utcOffsetSec);
  } else {
    DBG.println("NTP sync failed");
  }
}

// --- Fallback HTML -----------------------------------------------------------
static const char FALLBACK_HTML[] PROGMEM =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Marble Clock</title>"
"<style>*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:sans-serif;background:#111;color:#eee;padding:20px}"
"h1{color:#4fc3f7;margin-bottom:20px}h2{color:#aaa;margin:16px 0 8px}"
"input,select{width:100%;padding:8px;margin:4px 0 12px;background:#222;border:1px solid #444;color:#eee;border-radius:6px}"
"button{padding:10px 20px;background:#01696f;color:#fff;border:none;border-radius:6px;cursor:pointer;width:100%;margin:4px 0}"
".row{display:flex;gap:8px}.row input{flex:1}"
".status{background:#1a1a1a;border-radius:8px;padding:12px;margin-bottom:16px;font-size:14px}"
".ok{color:#6daa45}.err{color:#dd6974}"
".chk{display:flex;align-items:center;gap:8px;margin:8px 0}</style></head><body>"
"<h1>Marble Clock</h1><div class='status' id='st'>Loading...</div>"
"<h2>WiFi</h2><input id='ssid' placeholder='SSID'><input id='pass' type='password' placeholder='Password'>"
"<h2>LED</h2>"
"<label>Tipo LED</label><select id='ledModel'><option value='0'>WS2812B (default)</option><option value='1'>SK6812</option></select>"
"<small style='color:#aaa'>Il cambio tipo LED richiede riavvio</small><br><br>"
"<label>Numero LED anello</label><input id='numLeds' type='number' min='1' max='200'>"
"<label>LED da saltare (ledSkip)</label><input id='ledSkip' type='number' min='0' max='10'>"
"<label>Offset rotazione</label><input id='ledOffset' type='number' min='0' max='199'>"
"<div class='chk'><input type='checkbox' id='ledReverse'><label>Inverti direzione (LED verso parete)</label></div>"
"<label>Luminosita globale</label><input id='brightness' type='range' min='0' max='255'>"
"<h2>Colori ORE (R G B)</h2>"
"<div class='row'><input id='hR' type='number' min='0' max='255' placeholder='R'>"
"<input id='hG' type='number' min='0' max='255' placeholder='G'>"
"<input id='hB' type='number' min='0' max='255' placeholder='B'></div>"
"<label>Intensita ore</label><input id='hBri' type='range' min='0' max='255'>"
"<h2>Colori MINUTI (R G B)</h2>"
"<div class='row'><input id='mR' type='number' min='0' max='255' placeholder='R'>"
"<input id='mG' type='number' min='0' max='255' placeholder='G'>"
"<input id='mB' type='number' min='0' max='255' placeholder='B'></div>"
"<label>Intensita minuti</label><input id='mBri' type='range' min='0' max='255'>"
"<h2>Orario</h2>"
"<label>UTC Offset secondi (3600=CET, 7200=CEST)</label><input id='utcOff' type='number'>"
"<label>Ora manuale (-1=NTP)</label>"
"<div class='row'><input id='manH' type='number' min='-1' max='23'><input id='manM' type='number' min='0' max='59'></div>"
"<h2>Mapping</h2><select id='mapMode'>"
"<option value='0'>Distribuiti uniformemente</option>"
"<option value='1'>Primi 60</option><option value='2'>Tutti con gap</option></select><br><br>"
"<button onclick='save()'>Salva e applica</button>"
"<button onclick='doSyncNTP()'>Sync NTP ora</button>"
"<button onclick='reboot()'>Riavvia</button>"
"<script>"
"async function load(){try{const r=await fetch('/api/status');const d=await r.json();"
"document.getElementById('st').innerHTML=`WiFi: <span class='${d.wifiConnected?'ok':'err'}'>${d.wifiConnected?d.ssid+' ('+d.ip+')':'offline'}</span>`"
"+` | NTP: <span class='${d.ntpSynced?'ok':'err'}'>${d.ntpSynced?d.hour+':'+String(d.minute).padStart(2,'0'):'no sync'}</span>`;"
"document.getElementById('ssid').value=d.ssid||'';"
"document.getElementById('numLeds').value=d.numLeds;"
"document.getElementById('ledSkip').value=d.ledSkip!=null?d.ledSkip:1;"
"document.getElementById('ledOffset').value=d.ledOffset||0;"
"document.getElementById('ledReverse').checked=d.ledReverse||false;"
"document.getElementById('brightness').value=d.brightness;"
"document.getElementById('hR').value=d.hourR;document.getElementById('hG').value=d.hourG;document.getElementById('hB').value=d.hourB;"
"document.getElementById('mR').value=d.minR;document.getElementById('mG').value=d.minG;document.getElementById('mB').value=d.minB;"
"document.getElementById('hBri').value=d.hourBrightness;document.getElementById('mBri').value=d.minBrightness;"
"document.getElementById('utcOff').value=d.utcOffsetSec;"
"document.getElementById('manH').value=d.manualHour;document.getElementById('manM').value=d.manualMinute;"
"document.getElementById('mapMode').value=d.mappingMode;"
"document.getElementById('ledModel').value=d.ledModel||0;"
"}catch(e){document.getElementById('st').innerHTML=\"<span class='err'>Errore connessione</span>\";}"
"}"
"async function save(){"
"const prevModel=parseInt(document.getElementById('ledModel').dataset.loaded||0);"
"const newModel=parseInt(document.getElementById('ledModel').value);"
"const body={ssid:document.getElementById('ssid').value,password:document.getElementById('pass').value,"
"numLeds:+document.getElementById('numLeds').value,ledSkip:+document.getElementById('ledSkip').value,"
"ledOffset:+document.getElementById('ledOffset').value,"
"ledReverse:document.getElementById('ledReverse').checked,"
"brightness:+document.getElementById('brightness').value,"
"hourR:+document.getElementById('hR').value,hourG:+document.getElementById('hG').value,hourB:+document.getElementById('hB').value,"
"minR:+document.getElementById('mR').value,minG:+document.getElementById('mG').value,minB:+document.getElementById('mB').value,"
"hourBrightness:+document.getElementById('hBri').value,minBrightness:+document.getElementById('mBri').value,"
"utcOffsetSec:+document.getElementById('utcOff').value,"
"manualHour:+document.getElementById('manH').value,manualMinute:+document.getElementById('manM').value,"
"mappingMode:+document.getElementById('mapMode').value,ledModel:newModel};"
"await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"if(newModel!==prevModel){alert('Tipo LED cambiato. Riavvio necessario.');await fetch('/api/reconnect',{method:'POST'});}"
"else{alert('Salvato!');load();}}"
"async function doSyncNTP(){await fetch('/api/syncntp',{method:'POST'});alert('Sync NTP avviato');setTimeout(load,2500);}"
"async function reboot(){await fetch('/api/reconnect',{method:'POST'});alert('Riavvio in corso...');}"
"load().then(()=>{const m=document.getElementById('ledModel');m.dataset.loaded=m.value;});"
"</script></body></html>";

// --- Web server --------------------------------------------------------------
void setupWebServer() {
  if (littlefsOK) {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!littlefsOK || !LittleFS.exists("/index.html")) {
      req->send_P(200, "text/html", FALLBACK_HTML);
    } else {
      req->send(LittleFS, "/index.html", "text/html");
    }
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    if (cfg.manualHour >= 0) {
      doc["hour"]   = cfg.manualHour;
      doc["minute"] = cfg.manualMinute;
      doc["second"] = 0;
    } else {
      doc["hour"]   = timeClient.getHours();
      doc["minute"] = timeClient.getMinutes();
      doc["second"] = timeClient.getSeconds();
    }
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
    doc["ledOffset"]      = cfg.ledOffset;
    doc["ledSkip"]        = cfg.ledSkip;
    doc["ledReverse"]     = cfg.ledReverse;
    String json;
    serializeJson(doc, json);
    req->send(200, "application/json", json);
  });

  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
      JsonDocument doc;
      if (deserializeJson(doc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"JSON non valido\"}");
        return;
      }
      bool utcChanged   = false;
      bool modelChanged = false;
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
      if (doc["utcOffsetSec"].is<int>()) {
        int newOff = (int)doc["utcOffsetSec"];
        if (newOff != cfg.utcOffsetSec) { cfg.utcOffsetSec = newOff; utcChanged = true; }
      }
      if (doc["ntpEnabled"].is<bool>())     cfg.ntpEnabled  = (bool)doc["ntpEnabled"];
      if (doc["ledDensity"].is<int>())      cfg.ledDensity  = (int)doc["ledDensity"];
      if (doc["mappingMode"].is<int>())     cfg.mappingMode = constrain((int)doc["mappingMode"], 0, 2);
      if (doc["manualHour"].is<int>())      cfg.manualHour  = constrain((int)doc["manualHour"], -1, 23);
      if (doc["manualMinute"].is<int>())    cfg.manualMinute= constrain((int)doc["manualMinute"], 0, 59);
      if (doc["ledOffset"].is<int>())       cfg.ledOffset   = constrain((int)doc["ledOffset"], 0, MAX_LEDS - 1);
      if (doc["ledSkip"].is<int>())         cfg.ledSkip     = constrain((int)doc["ledSkip"], 0, 10);
      if (doc["ledReverse"].is<bool>())     cfg.ledReverse  = (bool)doc["ledReverse"];
      if (doc["ledModel"].is<int>()) {
        int newModel = constrain((int)doc["ledModel"], 0, 1);
        if (newModel != cfg.ledModel) { cfg.ledModel = newModel; modelChanged = true; }
      }
      FastLED.setBrightness(cfg.brightness);
      saveConfig();
      if (utcChanged && wifiConnected && cfg.ntpEnabled) syncNTP();
      // Se il modello LED e' cambiato, la webapp fara' un /api/reconnect separato
      req->send(200, "application/json", modelChanged ?
        "{\"ok\":true,\"rebootRequired\":true}" :
        "{\"ok\":true,\"rebootRequired\":false}"
      );
    }
  );

  server.on("/api/syncntp", HTTP_POST, [](AsyncWebServerRequest *req) {
    syncNTP();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/reconnect", HTTP_POST, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"ok\":true}");
    delay(200);
    ESP.restart();
  });

  server.begin();
  DBG.println("Web server started.");
}

// --- Setup -------------------------------------------------------------------
void setup() {
  DBG.begin(115200);
  delay(500);
  DBG.println("\n=== Marble Ring Clock ===");
  DBG.printf("DATA_PIN = %d\n", DATA_PIN);

  loadConfig();
  initFastLED();

  fill_solid(leds, MAX_LEDS, CRGB::Black);
  FastLED.show();

  DBG.printf("Config: %d LED, skip %d, offset %d, reverse %d, model %d\n",
    cfg.numLeds, cfg.ledSkip, cfg.ledOffset, cfg.ledReverse, cfg.ledModel);

  littlefsOK = LittleFS.begin(true);
  DBG.println(littlefsOK ? "LittleFS OK" : "LittleFS FAILED - fallback attivo");
  if (littlefsOK && !LittleFS.exists("/index.html"))
    DBG.println("index.html mancante - fallback attivo");

  connectWiFi();
  syncNTP();

  ArduinoOTA.setHostname("marble-clock");
  ArduinoOTA.begin();

  setupWebServer();

  // Startup sweep sui soli LED dell'anello
  for (int i = 0; i < cfg.numLeds; i++) {
    fill_solid(leds, MAX_LEDS, CRGB::Black);
    leds[cfg.ledSkip + i] = CRGB(0, 50, 80);
    FastLED.show();
    delay(20);
  }
  fill_solid(leds, MAX_LEDS, CRGB::Black);
  FastLED.show();

  DBG.println("Setup done.");
}

// --- Loop --------------------------------------------------------------------
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
