/*
 * Marble Ring Clock - ESP32
 * WS2812B / SK6812 LED ring clock with NTP sync and web configuration
 *
 * Board: Lolin32 Lite (target) / ESP32-C3 Mini (test)
 *
 * Fix log:
 *  - utcOffsetSec applicato subito senza reboot
 *  - ledOffset configurabile da webapp
 *  - ledSkip: salta N LED iniziali
 *  - ledReverse: inverte direzione anello
 *  - showSeconds: lancetta secondi on/off
 *  - FIX v3: /api/status manda bool nativi JSON
 *  - FIX v4: ESP32-C3 usa Serial0, ARDUINO_USB_MODE=1
 *  - FEAT v5: trail/fade per ogni lancetta (trailBefore, trailAfter, trailDecay)
 *    Ogni lancetta ha code/sfumature indipendenti prima e dopo il LED centrale.
 *    trailDecay: 0-100 (percentuale di luminosita residua per ogni step).
 *    Esempio: decay=60 -> step1=60%, step2=36%, step3=22% del LED centrale.
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
  uint8_t secR,  secG,  secB;
  int     hourBrightness;
  int     minBrightness;
  int     secBrightness;
  bool    showSeconds;
  int     utcOffsetSec;
  int     manualHour;
  int     manualMinute;
  bool    ntpEnabled;
  int     ledDensity;
  int     mappingMode;
  int     ledModel;
  int     ledOffset;
  int     ledSkip;
  bool    ledReverse;
  // Trail / fade per ogni lancetta
  int     hourTrailBefore;   // LED sfumati prima della lancetta ore
  int     hourTrailAfter;    // LED sfumati dopo la lancetta ore
  int     hourTrailDecay;    // decay % per step ore (0-100)
  int     minTrailBefore;
  int     minTrailAfter;
  int     minTrailDecay;
  int     secTrailBefore;
  int     secTrailAfter;
  int     secTrailDecay;
} cfg;

bool wifiConnected = false;
bool ntpSynced     = false;
bool littlefsOK    = false;
unsigned long lastNTPSync = 0;
unsigned long lastUpdate  = 0;

static inline bool readJsonBool(JsonDocument& doc, const char* key, bool fallback) {
  JsonVariant v = doc[key];
  if (v.isNull()) return fallback;
  return v.as<bool>();
}

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
  cfg.secR           = prefs.getUChar("sR",        200);
  cfg.secG           = prefs.getUChar("sG",        0);
  cfg.secB           = prefs.getUChar("sB",        0);
  cfg.hourBrightness = prefs.getInt("hBri",        200);
  cfg.minBrightness  = prefs.getInt("mBri",        255);
  cfg.secBrightness  = prefs.getInt("sBri",        160);
  cfg.showSeconds    = prefs.getBool("showSec",    true);
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
  // Trail defaults: 2 LED prima, 2 dopo, decay 50%
  cfg.hourTrailBefore = prefs.getInt("hTB",  2);
  cfg.hourTrailAfter  = prefs.getInt("hTA",  2);
  cfg.hourTrailDecay  = prefs.getInt("hTD",  50);
  cfg.minTrailBefore  = prefs.getInt("mTB",  2);
  cfg.minTrailAfter   = prefs.getInt("mTA",  2);
  cfg.minTrailDecay   = prefs.getInt("mTD",  50);
  cfg.secTrailBefore  = prefs.getInt("sTB",  1);
  cfg.secTrailAfter   = prefs.getInt("sTA",  1);
  cfg.secTrailDecay   = prefs.getInt("sTD",  40);
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
  prefs.putUChar("sR",        cfg.secR);
  prefs.putUChar("sG",        cfg.secG);
  prefs.putUChar("sB",        cfg.secB);
  prefs.putInt("hBri",        cfg.hourBrightness);
  prefs.putInt("mBri",        cfg.minBrightness);
  prefs.putInt("sBri",        cfg.secBrightness);
  prefs.putBool("showSec",    cfg.showSeconds);
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
  prefs.putInt("hTB",  cfg.hourTrailBefore);
  prefs.putInt("hTA",  cfg.hourTrailAfter);
  prefs.putInt("hTD",  cfg.hourTrailDecay);
  prefs.putInt("mTB",  cfg.minTrailBefore);
  prefs.putInt("mTA",  cfg.minTrailAfter);
  prefs.putInt("mTD",  cfg.minTrailDecay);
  prefs.putInt("sTB",  cfg.secTrailBefore);
  prefs.putInt("sTA",  cfg.secTrailAfter);
  prefs.putInt("sTD",  cfg.secTrailDecay);
  prefs.end();
}

// --- LED mapping -------------------------------------------------------------
// Restituisce posizione FISICA (assoluta nell'array leds[]) dato il valore
// logico 0-59 (minuti/secondi) oppure 0-59 (ore mappate su 60 step).
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
  pos = (pos + offset) % totalLeds;
  if (reverse) pos = (totalLeds - pos) % totalLeds;
  return skip + pos;
}

// Converte posizione logica (0-59) in posizione nell'anello (0..numLeds-1)
// senza aggiungere skip — usato internamente per il calcolo trail.
int logicalToRingPos(int logical, int totalLeds, int mode, int offset, bool reverse) {
  if (totalLeds <= 0) return -1;
  int pos;
  switch (mode) {
    case 1:
      pos = logical % totalLeds;
      break;
    case 0:
    case 2:
    default:
      pos = (int)round((float)logical * totalLeds / 60.0f) % totalLeds;
      break;
  }
  pos = (pos + offset) % totalLeds;
  if (reverse) pos = (totalLeds - pos) % totalLeds;
  return pos;
}

// Imposta un LED con addizione cromatica (non sovrascrive, somma i canali
// clampandoli a 255). Permette sovrapposizione trail di lancette diverse.
void addLed(int physPos, CRGB color) {
  if (physPos < cfg.ledSkip || physPos >= cfg.ledSkip + cfg.numLeds) return;
  leds[physPos].r = qadd8(leds[physPos].r, color.r);
  leds[physPos].g = qadd8(leds[physPos].g, color.g);
  leds[physPos].b = qadd8(leds[physPos].b, color.b);
}

// Disegna una lancetta con il suo trail.
//   ringPos   : posizione centrale nell'anello (0..numLeds-1, senza skip)
//   baseColor : colore pieno del LED centrale (gia scalato per brightness)
//   trailBefore, trailAfter : numero di LED sfumati prima/dopo
//   decayPct  : 0-100, percentuale di luminosita mantenuta per ogni step
void drawHand(int ringPos, CRGB baseColor, int trailBefore, int trailAfter, int decayPct) {
  int n = cfg.numLeds;
  if (n <= 0) return;
  decayPct = constrain(decayPct, 0, 100);

  // LED centrale - piena luminosita
  addLed(cfg.ledSkip + ringPos, baseColor);

  // Trail PRIMA (verso posizioni precedenti nell'anello)
  float factor = decayPct / 100.0f;
  float mult   = factor;
  for (int i = 1; i <= trailBefore; i++) {
    int pos = ((ringPos - i) % n + n) % n;
    CRGB tc(
      (uint8_t)(baseColor.r * mult),
      (uint8_t)(baseColor.g * mult),
      (uint8_t)(baseColor.b * mult)
    );
    addLed(cfg.ledSkip + pos, tc);
    mult *= factor;
    if (mult < 0.004f) break;
  }

  // Trail DOPO (verso posizioni successive nell'anello)
  mult = factor;
  for (int i = 1; i <= trailAfter; i++) {
    int pos = (ringPos + i) % n;
    CRGB tc(
      (uint8_t)(baseColor.r * mult),
      (uint8_t)(baseColor.g * mult),
      (uint8_t)(baseColor.b * mult)
    );
    addLed(cfg.ledSkip + pos, tc);
    mult *= factor;
    if (mult < 0.004f) break;
  }
}

// --- Clock rendering ---------------------------------------------------------
void renderClock(int hour24, int minute, int second) {
  fill_solid(leds, cfg.ledSkip + cfg.numLeds, CRGB::Black);

  // Secondi
  if (cfg.showSeconds) {
    int rp = logicalToRingPos(second % 60, cfg.numLeds, cfg.mappingMode,
                               cfg.ledOffset, cfg.ledReverse);
    CRGB sc(
      (cfg.secR * cfg.secBrightness) / 255,
      (cfg.secG * cfg.secBrightness) / 255,
      (cfg.secB * cfg.secBrightness) / 255
    );
    drawHand(rp, sc, cfg.secTrailBefore, cfg.secTrailAfter, cfg.secTrailDecay);
  }

  // Minuti
  {
    int rp = logicalToRingPos(minute % 60, cfg.numLeds, cfg.mappingMode,
                               cfg.ledOffset, cfg.ledReverse);
    CRGB mc(
      (cfg.minR * cfg.minBrightness) / 255,
      (cfg.minG * cfg.minBrightness) / 255,
      (cfg.minB * cfg.minBrightness) / 255
    );
    drawHand(rp, mc, cfg.minTrailBefore, cfg.minTrailAfter, cfg.minTrailDecay);
  }

  // Ore (si muovono di 5 step ogni ora + interpolazione sui minuti)
  {
    float hourFrac    = (hour24 % 12) * 5.0f + (minute / 12.0f);
    int   hourLogical = (int)round(hourFrac) % 60;
    int   rp = logicalToRingPos(hourLogical, cfg.numLeds, cfg.mappingMode,
                                 cfg.ledOffset, cfg.ledReverse);
    CRGB hc(
      (cfg.hourR * cfg.hourBrightness) / 255,
      (cfg.hourG * cfg.hourBrightness) / 255,
      (cfg.hourB * cfg.hourBrightness) / 255
    );
    drawHand(rp, hc, cfg.hourTrailBefore, cfg.hourTrailAfter, cfg.hourTrailDecay);
  }

  FastLED.show();
}

// --- FastLED init ------------------------------------------------------------
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

// --- Fallback HTML (incorporata nel firmware) --------------------------------
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
".row3{display:flex;gap:8px}.row3 input{flex:1}"
".status{background:#1a1a1a;border-radius:8px;padding:12px;margin-bottom:16px;font-size:14px}"
".ok{color:#6daa45}.err{color:#dd6974}"
".chk{display:flex;align-items:center;gap:8px;margin:8px 0}"
"label.small{font-size:12px;color:#aaa}"
"</style></head><body>"
"<h1>&#9679; Marble Clock</h1><div class='status' id='st'>Loading...</div>"
"<h2>WiFi</h2>"
"<label>SSID</label><input id='ssid' placeholder='Nome rete'>"
"<label>Password</label><input id='pass' type='password' placeholder='Password'>"
"<h2>LED Hardware</h2>"
"<label>Tipo LED</label><select id='ledModel'><option value='0'>WS2812B</option><option value='1'>SK6812</option></select>"
"<label>Numero LED nell&apos;anello</label><input id='numLeds' type='number' min='1' max='200'>"
"<label>LED skip (LED onboard da saltare)</label><input id='ledSkip' type='number' min='0' max='10'>"
"<label>Offset rotazione (LED di offset per ore 12)</label><input id='ledOffset' type='number' min='0' max='199'>"
"<div class='chk'><input type='checkbox' id='ledReverse'><label>Inverti direzione anello</label></div>"
"<label>Luminosita globale</label><input id='brightness' type='range' min='0' max='255'>"
"<h2>Mapping LED</h2>"
"<select id='mapMode'>"
"<option value='0'>Distribuiti uniformemente</option>"
"<option value='1'>Primi 60 diretti</option>"
"<option value='2'>Tutti con gap uniforme</option>"
"</select>"
"<h2>&#9711; Lancetta ORE</h2>"
"<label>Colore (R G B)</label>"
"<div class='row'><input id='hR' type='number' min='0' max='255' placeholder='R'>"
"<input id='hG' type='number' min='0' max='255' placeholder='G'>"
"<input id='hB' type='number' min='0' max='255' placeholder='B'></div>"
"<label>Intensita</label><input id='hBri' type='range' min='0' max='255'>"
"<label>Trail PRIMA del LED (0-10)</label><input id='hTB' type='number' min='0' max='10'>"
"<label>Trail DOPO il LED (0-10)</label><input id='hTA' type='number' min='0' max='10'>"
"<label>Decay trail % (0=niente, 100=invariato)</label><input id='hTD' type='range' min='0' max='100'><span id='hTDv'></span>"
"<h2>&#9711; Lancetta MINUTI</h2>"
"<label>Colore (R G B)</label>"
"<div class='row'><input id='mR' type='number' min='0' max='255' placeholder='R'>"
"<input id='mG' type='number' min='0' max='255' placeholder='G'>"
"<input id='mB' type='number' min='0' max='255' placeholder='B'></div>"
"<label>Intensita</label><input id='mBri' type='range' min='0' max='255'>"
"<label>Trail PRIMA del LED (0-10)</label><input id='mTB' type='number' min='0' max='10'>"
"<label>Trail DOPO il LED (0-10)</label><input id='mTA' type='number' min='0' max='10'>"
"<label>Decay trail %</label><input id='mTD' type='range' min='0' max='100'><span id='mTDv'></span>"
"<h2>&#9711; Lancetta SECONDI</h2>"
"<div class='chk'><input type='checkbox' id='showSeconds'><label>Mostra lancetta secondi</label></div>"
"<label>Colore (R G B)</label>"
"<div class='row'><input id='sR' type='number' min='0' max='255' placeholder='R'>"
"<input id='sG' type='number' min='0' max='255' placeholder='G'>"
"<input id='sB' type='number' min='0' max='255' placeholder='B'></div>"
"<label>Intensita</label><input id='sBri' type='range' min='0' max='255'>"
"<label>Trail PRIMA del LED (0-10)</label><input id='sTB' type='number' min='0' max='10'>"
"<label>Trail DOPO il LED (0-10)</label><input id='sTA' type='number' min='0' max='10'>"
"<label>Decay trail %</label><input id='sTD' type='range' min='0' max='100'><span id='sTDv'></span>"
"<h2>Orario</h2>"
"<label>UTC Offset secondi (3600=CET, 7200=CEST)</label><input id='utcOff' type='number'>"
"<label>Ora manuale (-1 = usa NTP)</label>"
"<div class='row'><input id='manH' type='number' min='-1' max='23' placeholder='Ora'>"
"<input id='manM' type='number' min='0' max='59' placeholder='Min'></div><br>"
"<button onclick='save()'>&#10003; Salva e applica</button>"
"<button onclick='doSyncNTP()'>&#8635; Sync NTP</button>"
"<button onclick='reboot()'>&#8635; Riavvia</button>"
"<script>"
"function sv(id,val){document.getElementById(id).value=val;}"
"function gv(id){return document.getElementById(id).value;}"
"function gi(id){return +document.getElementById(id).value;}"
"function gc(id){return document.getElementById(id).checked;}"
"function sc(id,v){document.getElementById(id).checked=!!v;}"
"function bindDecay(id,spId){"
"  const el=document.getElementById(id),sp=document.getElementById(spId);"
"  const upd=()=>sp.textContent=' '+el.value+'%';"
"  el.addEventListener('input',upd);upd();"
"}"
"async function load(){try{"
"const r=await fetch('/api/status');const d=await r.json();"
"document.getElementById('st').innerHTML="
"  `WiFi: <span class='${d.wifiConnected?'ok':'err'}'>${d.wifiConnected?d.ip:'offline'}</span>`"
"  +` | NTP: <span class='${d.ntpSynced?'ok':'err'}'>${d.ntpSynced?d.hour+':'+String(d.minute).padStart(2,'0'):'no sync'}</span>`;"
"sv('ssid',d.ssid||'');"
"sv('numLeds',d.numLeds);sv('ledSkip',d.ledSkip??1);sv('ledOffset',d.ledOffset??0);"
"sc('ledReverse',d.ledReverse);sv('brightness',d.brightness);"
"sv('hR',d.hourR);sv('hG',d.hourG);sv('hB',d.hourB);sv('hBri',d.hourBrightness);"
"sv('mR',d.minR);sv('mG',d.minG);sv('mB',d.minB);sv('mBri',d.minBrightness);"
"sv('sR',d.secR??200);sv('sG',d.secG??0);sv('sB',d.secB??0);sv('sBri',d.secBrightness??160);"
"sc('showSeconds',d.showSeconds);"
"sv('hTB',d.hourTrailBefore??2);sv('hTA',d.hourTrailAfter??2);sv('hTD',d.hourTrailDecay??50);"
"sv('mTB',d.minTrailBefore??2);sv('mTA',d.minTrailAfter??2);sv('mTD',d.minTrailDecay??50);"
"sv('sTB',d.secTrailBefore??1);sv('sTA',d.secTrailAfter??1);sv('sTD',d.secTrailDecay??40);"
"sv('utcOff',d.utcOffsetSec);sv('manH',d.manualHour);sv('manM',d.manualMinute);"
"sv('mapMode',d.mappingMode);sv('ledModel',d.ledModel??0);"
"['hTD','mTD','sTD'].forEach(id=>document.getElementById(id).dispatchEvent(new Event('input')));"
"}catch(e){document.getElementById('st').innerHTML=\"<span class='err'>Errore connessione</span>\";}}"
"async function save(){"
"const body={"
"ssid:gv('ssid'),password:gv('pass'),"
"numLeds:gi('numLeds'),ledSkip:gi('ledSkip'),ledOffset:gi('ledOffset'),"
"ledReverse:gc('ledReverse'),brightness:gi('brightness'),"
"hourR:gi('hR'),hourG:gi('hG'),hourB:gi('hB'),hourBrightness:gi('hBri'),"
"minR:gi('mR'),minG:gi('mG'),minB:gi('mB'),minBrightness:gi('mBri'),"
"secR:gi('sR'),secG:gi('sG'),secB:gi('sB'),secBrightness:gi('sBri'),"
"showSeconds:gc('showSeconds'),"
"hourTrailBefore:gi('hTB'),hourTrailAfter:gi('hTA'),hourTrailDecay:gi('hTD'),"
"minTrailBefore:gi('mTB'),minTrailAfter:gi('mTA'),minTrailDecay:gi('mTD'),"
"secTrailBefore:gi('sTB'),secTrailAfter:gi('sTA'),secTrailDecay:gi('sTD'),"
"utcOffsetSec:gi('utcOff'),manualHour:gi('manH'),manualMinute:gi('manM'),"
"mappingMode:gi('mapMode'),ledModel:gi('ledModel'),ntpEnabled:true};"
"await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"alert('Salvato!');load();}"
"async function doSyncNTP(){await fetch('/api/syncntp',{method:'POST'});alert('Sync NTP avviato');setTimeout(load,2500);}"
"async function reboot(){await fetch('/api/reconnect',{method:'POST'});alert('Riavvio in corso...');}"
"bindDecay('hTD','hTDv');bindDecay('mTD','mTDv');bindDecay('sTD','sTDv');"
"load();"
"</script></body></html>";

// --- Web server --------------------------------------------------------------
void setupWebServer() {
  if (littlefsOK) {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (!littlefsOK || !LittleFS.exists("/index.html")) {
      req->send(200, "text/html", FALLBACK_HTML);
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
    doc["ntpSynced"]      = (bool)ntpSynced;
    doc["wifiConnected"]  = (bool)wifiConnected;
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
    doc["secR"]           = cfg.secR;
    doc["secG"]           = cfg.secG;
    doc["secB"]           = cfg.secB;
    doc["hourBrightness"] = cfg.hourBrightness;
    doc["minBrightness"]  = cfg.minBrightness;
    doc["secBrightness"]  = cfg.secBrightness;
    doc["showSeconds"]    = (bool)cfg.showSeconds;
    doc["ntpEnabled"]     = (bool)cfg.ntpEnabled;
    doc["ledReverse"]     = (bool)cfg.ledReverse;
    doc["utcOffsetSec"]   = cfg.utcOffsetSec;
    doc["ledDensity"]     = cfg.ledDensity;
    doc["mappingMode"]    = cfg.mappingMode;
    doc["manualHour"]     = cfg.manualHour;
    doc["manualMinute"]   = cfg.manualMinute;
    doc["ledModel"]       = cfg.ledModel;
    doc["ledOffset"]      = cfg.ledOffset;
    doc["ledSkip"]        = cfg.ledSkip;
    doc["hourTrailBefore"] = cfg.hourTrailBefore;
    doc["hourTrailAfter"]  = cfg.hourTrailAfter;
    doc["hourTrailDecay"]  = cfg.hourTrailDecay;
    doc["minTrailBefore"]  = cfg.minTrailBefore;
    doc["minTrailAfter"]   = cfg.minTrailAfter;
    doc["minTrailDecay"]   = cfg.minTrailDecay;
    doc["secTrailBefore"]  = cfg.secTrailBefore;
    doc["secTrailAfter"]   = cfg.secTrailAfter;
    doc["secTrailDecay"]   = cfg.secTrailDecay;
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
      if (!doc["numLeds"].isNull())          cfg.numLeds        = constrain((int)doc["numLeds"], 1, MAX_LEDS);
      if (!doc["brightness"].isNull())       cfg.brightness     = constrain((int)doc["brightness"], 0, 255);
      if (!doc["hourR"].isNull())            cfg.hourR          = constrain((int)doc["hourR"], 0, 255);
      if (!doc["hourG"].isNull())            cfg.hourG          = constrain((int)doc["hourG"], 0, 255);
      if (!doc["hourB"].isNull())            cfg.hourB          = constrain((int)doc["hourB"], 0, 255);
      if (!doc["minR"].isNull())             cfg.minR           = constrain((int)doc["minR"], 0, 255);
      if (!doc["minG"].isNull())             cfg.minG           = constrain((int)doc["minG"], 0, 255);
      if (!doc["minB"].isNull())             cfg.minB           = constrain((int)doc["minB"], 0, 255);
      if (!doc["secR"].isNull())             cfg.secR           = constrain((int)doc["secR"], 0, 255);
      if (!doc["secG"].isNull())             cfg.secG           = constrain((int)doc["secG"], 0, 255);
      if (!doc["secB"].isNull())             cfg.secB           = constrain((int)doc["secB"], 0, 255);
      if (!doc["hourBrightness"].isNull())   cfg.hourBrightness = constrain((int)doc["hourBrightness"], 0, 255);
      if (!doc["minBrightness"].isNull())    cfg.minBrightness  = constrain((int)doc["minBrightness"], 0, 255);
      if (!doc["secBrightness"].isNull())    cfg.secBrightness  = constrain((int)doc["secBrightness"], 0, 255);
      cfg.showSeconds = readJsonBool(doc, "showSeconds", cfg.showSeconds);
      cfg.ntpEnabled  = readJsonBool(doc, "ntpEnabled",  cfg.ntpEnabled);
      cfg.ledReverse  = readJsonBool(doc, "ledReverse",  cfg.ledReverse);
      if (!doc["utcOffsetSec"].isNull()) {
        int newOff = (int)doc["utcOffsetSec"];
        if (newOff != cfg.utcOffsetSec) { cfg.utcOffsetSec = newOff; utcChanged = true; }
      }
      if (!doc["ledDensity"].isNull())   cfg.ledDensity  = (int)doc["ledDensity"];
      if (!doc["mappingMode"].isNull())  cfg.mappingMode = constrain((int)doc["mappingMode"], 0, 2);
      if (!doc["manualHour"].isNull())   cfg.manualHour  = constrain((int)doc["manualHour"], -1, 23);
      if (!doc["manualMinute"].isNull()) cfg.manualMinute= constrain((int)doc["manualMinute"], 0, 59);
      if (!doc["ledOffset"].isNull())    cfg.ledOffset   = constrain((int)doc["ledOffset"], 0, MAX_LEDS - 1);
      if (!doc["ledSkip"].isNull())      cfg.ledSkip     = constrain((int)doc["ledSkip"], 0, 10);
      if (!doc["ledModel"].isNull()) {
        int newModel = constrain((int)doc["ledModel"], 0, 1);
        if (newModel != cfg.ledModel) { cfg.ledModel = newModel; modelChanged = true; }
      }
      // Trail
      if (!doc["hourTrailBefore"].isNull()) cfg.hourTrailBefore = constrain((int)doc["hourTrailBefore"], 0, 10);
      if (!doc["hourTrailAfter"].isNull())  cfg.hourTrailAfter  = constrain((int)doc["hourTrailAfter"],  0, 10);
      if (!doc["hourTrailDecay"].isNull())  cfg.hourTrailDecay  = constrain((int)doc["hourTrailDecay"],  0, 100);
      if (!doc["minTrailBefore"].isNull())  cfg.minTrailBefore  = constrain((int)doc["minTrailBefore"],  0, 10);
      if (!doc["minTrailAfter"].isNull())   cfg.minTrailAfter   = constrain((int)doc["minTrailAfter"],   0, 10);
      if (!doc["minTrailDecay"].isNull())   cfg.minTrailDecay   = constrain((int)doc["minTrailDecay"],   0, 100);
      if (!doc["secTrailBefore"].isNull())  cfg.secTrailBefore  = constrain((int)doc["secTrailBefore"],  0, 10);
      if (!doc["secTrailAfter"].isNull())   cfg.secTrailAfter   = constrain((int)doc["secTrailAfter"],   0, 10);
      if (!doc["secTrailDecay"].isNull())   cfg.secTrailDecay   = constrain((int)doc["secTrailDecay"],   0, 100);
      FastLED.setBrightness(cfg.brightness);
      saveConfig();
      if (utcChanged && wifiConnected && cfg.ntpEnabled) syncNTP();
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

  littlefsOK = LittleFS.begin(true);
  DBG.println(littlefsOK ? "LittleFS OK" : "LittleFS FAILED - fallback attivo");

  connectWiFi();
  syncNTP();

  ArduinoOTA.setHostname("marble-clock");
  ArduinoOTA.begin();

  setupWebServer();

  // Startup animation: sweep dell'anello
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
    int h, m, s;
    if (cfg.manualHour >= 0) {
      h = cfg.manualHour;
      m = cfg.manualMinute;
      s = 0;
    } else {
      timeClient.update();
      h = timeClient.getHours();
      m = timeClient.getMinutes();
      s = timeClient.getSeconds();
    }
    renderClock(h, m, s);
  }
}
