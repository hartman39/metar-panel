/*
  METAR PANEL  -  M5Stack Core2 (v1.3)
  ------------------------------------------------------------------
  Destination weather and frequencies on a 2" screen, over Starlink Wi-Fi.

  Screens
    Picker   asks where you're headed at power-up (home field always first)
    Entry    spin in any ICAO
    Cadence  refresh weather every 15 / 30 / 45 min
    WX       flight category, best-runway wind components, vis, sky,
             temp/dew, altimeter, raw METAR, observation age
    FREQ     destination frequencies

    Wi-Fi    saved networks; add or forget them from your phone

  The three touch circles under the screen
    WX     tap: weather page        hold: refresh weather now
    FREQ   tap: frequency page      hold: re-download frequencies
    APT    tap: change airport (divert)
  Tap the status line at the bottom of the WX page to change the cadence.
  Wi-Fi: APT, then the middle key (WI-FI).

  Wi-Fi
    Up to 8 saved networks. The panel joins the strongest one it can see and
    re-scans if the connection drops. Add networks from your phone: the panel
    makes its own hotspot (METAR-PANEL) and a setup page opens on the phone.
    Networks with a "click to accept" login page can't be used.

  Data
    Weather + runways : aviationweather.gov Data API (NOAA / AWC)
    KFWQ weather      : unofficial relay of the field's own AWOS (AWOS Live), used
                        only while KFWQ has no METAR on aviationweather.gov
    Frequencies       : OurAirports (community-maintained), cached per airport
  SUPPLEMENTAL ONLY. Not an approved source of weather or frequencies.

  Libraries : M5Unified (installs M5GFX), ArduinoJson 7.x
  Board     : M5Stack board package -> "M5Core2"
*/

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SD.h>
#include <time.h>
#include <math.h>

// ============================ CONFIG ============================
// Optional: one network built into the firmware. Normally leave these ""
// and add networks from your phone instead (APT > WI-FI > SET UP FROM PHONE).
#define WIFI_SSID  ""
#define WIFI_PASS  ""
#define AP_NAME    "METAR-PANEL" // the panel's own hotspot during phone setup
#define HOME_ICAO  "KFWQ"       // always the first tile on the picker

// Up to 4 more picker tiles. Your last destination also gets a tile.
const char* const FAVORITES[] = { "KAGC", "2G4", "KBVI" };

const uint8_t  CADENCES[3]      = { 15, 30, 45 };  // minutes
const uint8_t  DEFAULT_CADENCE  = 15;
const uint16_t STALE_AFTER_MIN  = 75;      // obs older than this shows STALE
const uint32_t RETRY_MS         = 60000;   // after a failed fetch, retry every minute

// Unofficial reports for fields whose AWOS isn't on aviationweather.gov yet.
// The relay serves <RELAY_URL><icao in lower case>.json in aviationweather.gov's format.
// Used only when the field has no METAR of its own; once KFWQ's AWOS is disseminated
// the official METAR wins automatically. Set RELAY_URL to "" to turn this off.
#define RELAY_URL "https://raw.githubusercontent.com/hartman39/kfwq-wx-relay/main/docs/"
const char* const RELAY_IDS[]   = { "KFWQ" };
const uint16_t RELAY_MAX_AGE_MIN = 90;     // older relay data is ignored (nearest station instead)
// ================================================================

#define RGB565(c) ((uint16_t)((((c) >> 8) & 0xF800) | (((c) >> 5) & 0x07E0) | (((c) & 0xFF) >> 3)))

const uint16_t C_BG    = RGB565(0x05070A);
const uint16_t C_TILE  = RGB565(0x10141A);
const uint16_t C_EDGE  = RGB565(0x232A32);
const uint16_t C_EDGE2 = RGB565(0x2C343D);
const uint16_t C_TEXT  = RGB565(0xF2F5F7);
const uint16_t C_SUB   = RGB565(0xA7B1BC);
const uint16_t C_LBL   = RGB565(0x8D99A6);
const uint16_t C_DIM   = RGB565(0x4A5059);
const uint16_t C_CYAN  = RGB565(0x4FD1E8);
const uint16_t C_AMBER = RGB565(0xF5A524);
const uint16_t C_RWY   = RGB565(0x4E5661);
const uint16_t C_VFR   = RGB565(0x2FBF71);
const uint16_t C_MVFR  = RGB565(0x2563EB);
const uint16_t C_IFR   = RGB565(0xD93036);
const uint16_t C_LIFR  = RGB565(0xC026D3);
const uint16_t C_DARK  = RGB565(0x06140C);

const char* const UA      = "CorePanel-METAR/1.5";
const char* const VERSION = "1.5";
const char* const TAIL    = "N110VT";
const char* const AWC     = "https://aviationweather.gov/api/data/";
const char* const OA_URL  = "https://davidmegginson.github.io/ourairports-data/airport-frequencies.csv";
const char* const ENTRY_CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

// ---------------------------- types -----------------------------
enum Screen { SCR_PICK, SCR_ENTRY, SCR_CADENCE, SCR_LOADING, SCR_WX, SCR_FREQ, SCR_WIFI, SCR_SETUP };
enum Cat    { CAT_NONE, CAT_VFR, CAT_MVFR, CAT_IFR, CAT_LIFR };

struct WxWord { const char* code; const char* word; };
struct Layer  { char cover[5]; int base; };
struct Metar {
  bool  valid;
  char  icao[6];
  char  name[32];
  char  raw[200];
  Cat   cat, visCat, cigCat;
  long  obs;
  int   wdir;  bool vrb;  int wspd;  int wgst;
  char  vis[12]; float visSm;
  float temp, dewp, altim;
  Layer layers[4]; int nLayers;
  int   ceiling;               // ft AGL, -1 = none
  float lat, lon;
  bool  proxy;                 // true = nearest station, not the destination
  float proxyNm;
  char  proxyDir[3];           // direction of that station from the destination
  bool  speci;                 // special observation (conditions changed between hourlies)
  char  wxCode[24];            // present weather as reported, e.g. "-RA BR"
  char  wxText[40];            // ...in words: "Light rain, mist"
  bool  wxHazard;              // thunderstorm, freezing precip, hail, squall, funnel, ash
  bool  maint;                 // "$": station needs maintenance
  bool  relay;                 // the field's own AWOS via the unofficial relay (not on aviationweather.gov)
};

// TAF, reduced to what the forecast strip needs: a flight category per hour.
const int TAF_HOURS = 12;
struct Taf {
  bool  valid;
  char  icao[6];
  bool  proxy; float distNm; char dir[3];
  long  issue;                 // epoch s
  long  start;                 // epoch s of the first hour cell
  uint8_t base[TAF_HOURS];     // prevailing category (Cat); CAT_NONE = outside the TAF
  uint8_t temp[TAF_HOURS];     // worse category under TEMPO/PROB, else CAT_NONE
};
struct Runway  { char ends[2][5]; int hdg; };   // hdg = true heading of ends[0]
struct Freq    { char label[26]; char mhz[8]; uint8_t prio; char type[6]; };
struct Airport {
  char   icao[6];
  char   name[32];
  int    elevFt;
  Runway rwy[6];  int nRwy;
  Freq   fq[14];  int nFq;  int primary;
  char   fqSrc[40];
  char   awcFreqs[120];
  float  lat, lon;
  char   altId[6];        // the other spelling of the ident (2G4 <-> K2G4), from AWC
};
struct Tile    { char icao[6]; char tag[6]; char name[24]; Cat cat; bool hasWx; };
struct Box     { int x, y, w, h; };
struct TypeInfo{ const char* type; uint8_t prio; const char* label; };

// Frequency types worth showing, in the order you'd use them inbound.
const TypeInfo TYPES[] = {
  {"ATIS", 0, "ATIS"},     {"AWOS", 1, "AWOS"},       {"ASOS", 1, "ASOS"},
  {"APP",  2, "APPROACH"}, {"A/D",  2, "APP/DEP"},    {"ARR",  2, "ARRIVAL"},
  {"TWR",  3, "TOWER"},    {"CTAF", 4, "CTAF"},       {"UNIC", 5, "UNICOM"},
  {"GND",  6, "GROUND"},   {"CLD",  7, "CLEARANCE"},  {"DEP",  8, "DEPARTURE"},
  {"AFIS", 9, "AFIS"},     {"A/G",  9, "AIR/GROUND"}, {"ATF",  9, "ATF"},
};
const int N_TYPES = sizeof(TYPES) / sizeof(TYPES[0]);

// Built-in frequencies for the airports you use most: instant, and they work
// with no internet. Checked against AirNav (FAA data) Oct 2026; re-check these
// against the Chart Supplement every few months. "ids" lists every spelling.
// Format per line: label|MHz|sort order|type
struct BuiltinFreqs { const char* ids; const char* data; };
const BuiltinFreqs BUILTIN_FREQS[] = {
  { "KFWQ FWQ",
    "CTAF/UNICOM|122.800|4|CTAF\n"
    "AWOS-3PT|118.475|1|AWOS\n"
    "PITTSBURGH APP/DEP|119.350|2|A/D\n" },
  { "KAGC AGC",
    "TOWER|121.100|3|TWR\n"
    "ATIS / ASOS|120.550|0|ATIS\n"
    "PITTSBURGH APP/DEP|119.350|2|A/D\n"
    "GROUND|121.700|6|GND\n"
    "UNICOM|122.950|5|UNIC\n" },
  { "KBVI BVI",
    "TWR M-F / CTAF|120.300|3|TWR\n"
    "ATIS / AWOS|118.350|0|ATIS\n"
    "PITTSBURGH APP/DEP|124.750|2|A/D\n"
    "GROUND M-F 0730-1930|121.800|6|GND\n"
    "CLNC (TWR CLOSED)|124.850|7|CLD\n" },
  { "2G4 K2G4",
    "CTAF/UNICOM|123.000|4|CTAF\n"
    "AWOS-3PT|120.125|1|AWOS\n"
    "CLARKSBURG APP/DEP|119.425|2|A/D\n"
    "CLEVELAND CTR (BKUP)|124.400|8|CNTR\n" },
};

const char* builtinFreqsFor(const char* id, const char* alt) {
  for (const auto& b : BUILTIN_FREQS) {
    char buf[32]; strlcpy(buf, b.ids, sizeof buf);
    for (char* t = strtok(buf, " "); t; t = strtok(nullptr, " "))
      if ((id && *id && !strcmp(t, id)) || (alt && *alt && !strcmp(t, alt))) return b.data;
  }
  return nullptr;
}

// --------------------------- globals ----------------------------
M5Canvas    cv(&M5.Display);
Preferences prefs;

Screen   screen        = SCR_PICK;
bool     airportLoaded = false;
bool     startupFlow   = true;    // power-up: pick airport -> cadence -> load
bool     cadenceChosen = false;
Airport  ap;
Metar    wx;
bool     noMetar       = false;   // station exists but reports no METAR
uint8_t  cadence       = DEFAULT_CADENCE;
uint32_t nextFetchMs   = 0;
uint32_t lastOkMs      = 0;
bool     lastFetchOk   = false;
bool     everFetched   = false;
bool     ntpStarted    = false;

// Wi-Fi
enum WState { W_IDLE, W_SCANNING, W_JOINING, W_UP, W_NONE_SAVED, W_NONE_SEEN, W_SETUP };
struct SavedNet { char ssid[33]; char pass[65]; };
const int MAX_NETS = 8;
SavedNet nets[MAX_NETS];
int      nNets         = 0;
WState   wState        = W_IDLE;
uint32_t wTimer        = 0;       // when the current Wi-Fi step started / next one is due
char     wSsid[33]     = "";      // network being joined, or joined
char     wFailed[33]   = "";      // last network that wouldn't let us in
int      wFailCount    = 0;
bool     wFromDrop     = false;   // JOINING because a working link dropped (not a new attempt)
void wifiTick();
char     lastDest[6]   = HOME_ICAO;
char     pendingIcao[6] = "";

// Background networking. Weather refreshes, picker categories and late frequency
// downloads run on the other CPU core so the screen and buttons never freeze.
// netMutex: one network job at a time (foreground loads take it too).
// dataMutex: guards the shared airport/weather/tile data while drawing or committing.
SemaphoreHandle_t netMutex  = nullptr;
SemaphoreHandle_t dataMutex = nullptr;
#define NET_LOCK()    xSemaphoreTakeRecursive(netMutex, portMAX_DELAY)
#define NET_UNLOCK()  xSemaphoreGiveRecursive(netMutex)
#define DATA_LOCK()   xSemaphoreTakeRecursive(dataMutex, portMAX_DELAY)
#define DATA_UNLOCK() xSemaphoreGiveRecursive(dataMutex)
volatile bool     needWx    = false, needTiles = false, needFreqs = false, netBusy = false;
volatile uint32_t apGen     = 0;      // bumped when the airport changes: stale results are dropped
volatile uint32_t tilesGen  = 0;
volatile bool     fgWaiting = false;  // the screen side wants the network: long jobs bail out
void fgNetBegin();
void fgNetEnd();
void pushChanged();

// SD card frequency database: every airport's frequencies, downloaded once and
// refreshed monthly, so any airport loads instantly and offline.
// The SD card shares its wires with the screen, so card access and screen
// updates take turns through busMutex.
SemaphoreHandle_t busMutex = nullptr;
#define BUS_LOCK()    xSemaphoreTakeRecursive(busMutex, portMAX_DELAY)
#define BUS_UNLOCK()  xSemaphoreGiveRecursive(busMutex)
const char* const DB_DIR  = "/metarpanel";
const char* const DB_PATH = "/metarpanel/freqs.txt";
const char* const DB_TMP  = "/metarpanel/freqs.tmp";
const uint32_t DB_MAX_AGE_S = 28UL * 24 * 3600;   // refresh monthly
bool     sdOk       = false;
uint32_t dbStamp    = 0;          // when the database was downloaded (epoch s; 1 = unknown time)
uint32_t dbRows     = 0;
uint32_t dbRetryMs  = 30000;      // first attempt 30 s after power-up
volatile bool needDb = false;
volatile int  dbPct  = -1;        // download progress for the Wi-Fi screen, -1 = idle

bool     apInfoOk      = false;   // runways/name loaded (retried if Starlink was still booting)
bool     freqsPending  = false;   // airport loaded offline: get its frequencies once Wi-Fi is up
int  knownCount();
void startSetup(Screen ret);
void openWifi();
Airport  prevAp;                  // restored if a typed identifier turns out not to exist
Metar    prevWx;

Tile     tiles[6];
int      nTiles        = 0;
bool     tilesFetched  = false;
uint32_t tilesRetryMs  = 0;

char     entryChars[4];
int      entrySel      = 0;
char     entryMsg[40]  = "";
bool     entryPad      = false;   // keypad open (tap a letter box)

int      freqPage      = 0;
int      windSel       = -1;      // -1 = best runway; else 2*runway + end (tap the wind tile to cycle)
bool     showTaf       = true;    // bottom WX tile: forecast strip, or the raw METAR (tap to toggle)
uint8_t  briMode       = 0;       // 0 auto (dims at dusk), 1 day, 2 night
int      curBri        = -1;
char     loadLines[3][44];
int      loadPct       = -1;

bool     dirty         = true;
uint32_t lastDrawMs    = 0;

const lgfx::IFont* const BIG_FONTS[3] = { &fonts::DejaVu24, &fonts::DejaVu18, &fonts::DejaVu12 };

// Touch targets (screen coordinates, 320x240)
const Box B_WIND   = {6, 34, 98, 118};
const Box B_T[4]   = {{108, 34, 102, 56}, {214, 34, 100, 56}, {108, 96, 102, 56}, {214, 96, 100, 56}};
const Box B_RAW    = {6, 156, 308, 40};
const Box B_STATUS = {0, 196, 320, 26};
const Box B_TYPE   = {176, 180, 138, 40};
const Box B_LOAD   = {182, 182, 132, 40};
const Box B_PRI    = {6, 34, 308, 50};
const Box B_LIST   = {6, 88, 308, 108};

// ============================ helpers ============================
bool inBox(const Box& b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

void txt(const char* s, int x, int y, const lgfx::IFont* f, uint16_t col, textdatum_t d) {
  cv.setFont(f);
  cv.setTextColor(col);
  cv.setTextDatum(d);
  cv.drawString(s, x, y);
}

int textW(const char* s, const lgfx::IFont* f) { cv.setFont(f); return cv.textWidth(s); }

// Copy src into out, trimming with ".." until it fits maxW pixels.
void fitText(const char* src, char* out, size_t n, int maxW, const lgfx::IFont* f) {
  strlcpy(out, src, n);
  if (textW(out, f) <= maxW) return;
  char tmp[64];
  size_t len = strlen(out);
  while (len > 0) {
    out[--len] = 0;
    snprintf(tmp, sizeof tmp, "%s..", out);
    if (textW(tmp, f) <= maxW) { strlcpy(out, tmp, n); return; }
  }
}

// Largest of DejaVu 24/18/12 that fits.
const lgfx::IFont* pickFont(const char* s, int maxW) {
  for (int i = 0; i < 3; i++) if (textW(s, BIG_FONTS[i]) <= maxW) return BIG_FONTS[i];
  return BIG_FONTS[2];
}

void tileBg(const Box& b, uint16_t edge) {
  cv.fillRoundRect(b.x, b.y, b.w, b.h, 5, C_TILE);
  cv.drawRoundRect(b.x, b.y, b.w, b.h, 5, edge);
}

void upcase(char* s) { for (; *s; s++) *s = toupper((unsigned char)*s); }

void trim(char* s) {
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
  size_t i = 0; while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s + i, n - i + 1);
}

// "Pittsburgh/Allegheny Cnty, PA, US" -> "Allegheny Cnty";  "MONONGAHELA/ROSTRAVER " -> "Rostraver"
void shortName(const char* src, char* out, size_t n) {
  char buf[64]; strlcpy(buf, src, sizeof buf);
  char* comma = strchr(buf, ','); if (comma) *comma = 0;
  char* p = strrchr(buf, '/'); p = p ? p + 1 : buf;
  trim(p);
  bool allCaps = true;
  for (char* q = p; *q; q++) if (islower((unsigned char)*q)) { allCaps = false; break; }
  if (allCaps) {                       // title-case all-caps names
    bool start = true;
    for (char* q = p; *q; q++) {
      if (isalpha((unsigned char)*q)) { *q = start ? *q : tolower((unsigned char)*q); start = false; }
      else start = true;
    }
  }
  strlcpy(out, p, n);
}

Cat catFromStr(const char* s) {
  if (!strcmp(s, "VFR"))  return CAT_VFR;
  if (!strcmp(s, "MVFR")) return CAT_MVFR;
  if (!strcmp(s, "IFR"))  return CAT_IFR;
  if (!strcmp(s, "LIFR")) return CAT_LIFR;
  return CAT_NONE;
}
const char* catText(Cat c) {
  switch (c) { case CAT_VFR: return "VFR"; case CAT_MVFR: return "MVFR"; case CAT_IFR: return "IFR"; case CAT_LIFR: return "LIFR"; default: return "--"; }
}
uint16_t catColor(Cat c) {
  switch (c) { case CAT_VFR: return C_VFR; case CAT_MVFR: return C_MVFR; case CAT_IFR: return C_IFR; case CAT_LIFR: return C_LIFR; default: return C_DIM; }
}
uint16_t catInk(Cat c) { return c == CAT_VFR ? C_DARK : C_TEXT; }

// FAA flight categories
Cat catFromCeiling(int cig) {
  if (cig < 0) return CAT_VFR;
  if (cig < 500) return CAT_LIFR;
  if (cig < 1000) return CAT_IFR;
  if (cig <= 3000) return CAT_MVFR;
  return CAT_VFR;
}
Cat catFromVis(float v) {
  if (isnan(v)) return CAT_VFR;
  if (v < 1) return CAT_LIFR;
  if (v < 3) return CAT_IFR;
  if (v <= 5) return CAT_MVFR;
  return CAT_VFR;
}

void drawPill(int x, int y, int w, int h, Cat c, const lgfx::IFont* f) {
  cv.fillRoundRect(x, y, w, h, 4, catColor(c));
  txt(catText(c), x + w / 2, y + h / 2 + 1, f, catInk(c), textdatum_t::middle_center);
}

bool timeSynced() { return time(nullptr) > 1700000000; }

void zuluHHMM(long t, char* out, size_t n, bool colon) {
  time_t tt = (time_t)t; struct tm g; gmtime_r(&tt, &g);
  snprintf(out, n, colon ? "%02d:%02dZ" : "%02d%02dZ", g.tm_hour, g.tm_min);
}

int normDeg(int d) { while (d > 180) d -= 360; while (d < -180) d += 360; return d; }

// ------------------------------ the sun ------------------------------
// Sun elevation in degrees (good to a fraction of a degree, plenty for dusk dimming
// and sunrise/sunset to about a minute).
float sunElevDeg(time_t t, float latDeg, float lonDeg) {
  const double R = 0.017453292519943295;
  double d = ((double)t - 946728000.0) / 86400.0;       // days since J2000.0
  double g = (357.529 + 0.98560028 * d) * R;
  double q = 280.459 + 0.98564736 * d;
  double L = (q + 1.915 * sin(g) + 0.020 * sin(2 * g)) * R;
  double e = (23.439 - 0.00000036 * d) * R;
  double ra  = atan2(cos(e) * sin(L), cos(L));
  double dec = asin(sin(e) * sin(L));
  double gmst = fmod(18.697374558 + 24.06570982441908 * d, 24.0); if (gmst < 0) gmst += 24.0;
  double H = gmst * 15.0 * R + lonDeg * R - ra;
  double el = asin(sin(latDeg * R) * sin(dec) + cos(latDeg * R) * cos(dec) * cos(H));
  return (float)(el / R);
}

// Next sunrise or sunset (upper limb at the horizon) within 24 h. 0 = none (polar).
long nextSunEvent(time_t now, float lat, float lon, bool* isSet) {
  const float H0 = -0.833f;
  float prev = sunElevDeg(now, lat, lon);
  for (long s = 300; s <= 86400; s += 300) {
    float cur = sunElevDeg(now + s, lat, lon);
    if ((prev > H0) != (cur > H0)) {
      long lo = now + s - 300, hi = now + s;               // narrow down to the minute
      while (hi - lo > 30) { long m = (lo + hi) / 2; if ((sunElevDeg(m, lat, lon) > H0) == (prev > H0)) lo = m; else hi = m; }
      *isSet = prev > H0;
      return hi;
    }
    prev = cur;
  }
  return 0;
}

void formatVis(float v, char* out, size_t n) {
  int whole = (int)floorf(v + 0.001f);
  float f = v - whole;
  const char* frac = nullptr;
  if (fabsf(f - 0.125f) < 0.04f) frac = "1/8";
  else if (fabsf(f - 0.25f) < 0.05f) frac = "1/4";
  else if (fabsf(f - 0.5f) < 0.05f) frac = "1/2";
  else if (fabsf(f - 0.75f) < 0.05f) frac = "3/4";
  if (!frac) snprintf(out, n, "%d", (int)roundf(v));
  else if (whole == 0) snprintf(out, n, "%s", frac);
  else snprintf(out, n, "%d %s", whole, frac);
}

// ============================ network ============================
int httpGet(const String& url, String& body) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure client;
  client.setInsecure();                         // read-only public data
  client.setHandshakeTimeout(10);               // seconds; the default (120) can hang on a dead link
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return -2;
  http.setUserAgent(UA);
  int code = http.GET();
  if (code == 200) body = http.getString();
  http.end();
  return code;
}

// ---------------------- present weather, in words ----------------------
const WxWord WX_DESC[] = { {"TS","T-storm"}, {"SH","showers"}, {"FZ","freezing"}, {"BL","blowing"},
                           {"DR","drifting"}, {"MI","shallow"}, {"BC","patchy"}, {"PR","partial"} };
const WxWord WX_PHEN[] = { {"RA","rain"}, {"DZ","drizzle"}, {"SN","snow"}, {"SG","snow grains"},
                           {"IC","ice crystals"}, {"PL","ice pellets"}, {"GR","hail"}, {"GS","small hail"},
                           {"UP","unknown precip"}, {"BR","mist"}, {"FG","fog"}, {"FU","smoke"},
                           {"VA","volcanic ash"}, {"DU","dust"}, {"SA","sand"}, {"HZ","haze"},
                           {"PY","spray"}, {"PO","dust whirls"}, {"SQ","squalls"}, {"FC","funnel cloud"},
                           {"SS","sandstorm"}, {"DS","duststorm"} };

const char* wxLookup(const WxWord* t, int n, const char* p) {
  for (int i = 0; i < n; i++) if (p[0] == t[i].code[0] && p[1] == t[i].code[1]) return t[i].word;
  return nullptr;
}

// "-TSRA BR" -> "Light T-storm rain, mist". Returns false for tokens that aren't weather.
bool decodeWxToken(const char* tok, char* out, size_t n, bool* hazard) {
  const char* p = tok;
  const char* inten = "";
  bool near = false;
  if (*p == '-') { inten = "Light "; p++; }
  else if (*p == '+') { inten = "Heavy "; p++; if (hazard) *hazard = true; }
  else if (p[0] == 'V' && p[1] == 'C') { near = true; p += 2; }
  char words[48] = "";
  int parts = 0;
  bool showers = false;
  while (p[0] && p[1]) {
    if (p[0] == 'S' && p[1] == 'H') { showers = true; p += 2; parts++; continue; }
    const char* w = wxLookup(WX_DESC, sizeof WX_DESC / sizeof WX_DESC[0], p);
    if (!w) w = wxLookup(WX_PHEN, sizeof WX_PHEN / sizeof WX_PHEN[0], p);
    if (!w) return false;
    if (hazard && (!strncmp(p, "TS", 2) || !strncmp(p, "FZ", 2) || !strncmp(p, "GR", 2) ||
                   !strncmp(p, "SQ", 2) || !strncmp(p, "FC", 2) || !strncmp(p, "VA", 2))) *hazard = true;
    if (words[0]) strlcat(words, " ", sizeof words);
    strlcat(words, w, sizeof words);
    parts++;
    p += 2;
  }
  if (*p || !parts) return false;
  if (showers) { if (words[0]) strlcat(words, " ", sizeof words); strlcat(words, "showers", sizeof words); }
  snprintf(out, n, "%s%s%s", inten, words, near ? " nearby" : "");
  if (out[0]) out[0] = toupper((unsigned char)out[0]);
  return true;
}

void decodeWx(const char* codes, char* out, size_t n, bool* hazard) {
  out[0] = 0;
  char buf[32]; strlcpy(buf, codes, sizeof buf);
  for (char* t = strtok(buf, " "); t; t = strtok(nullptr, " ")) {
    char w[48];
    if (!decodeWxToken(t, w, sizeof w, hazard)) continue;
    if (out[0]) { strlcat(out, ", ", n); w[0] = tolower((unsigned char)w[0]); }
    strlcat(out, w, n);
  }
}

void parseMetar(JsonObject o, Metar& m) {
  memset(&m, 0, sizeof m);
  m.valid = true;
  strlcpy(m.icao, o["icaoId"] | "", sizeof m.icao);
  shortName(o["name"] | "", m.name, sizeof m.name);
  strlcpy(m.raw, o["rawOb"] | "", sizeof m.raw);
  m.obs  = o["obsTime"] | 0L;
  m.lat  = o["lat"] | NAN;
  m.lon  = o["lon"] | NAN;
  m.speci = !strcmp(o["metarType"] | "", "SPECI");
  strlcpy(m.wxCode, o["wxString"] | "", sizeof m.wxCode);
  decodeWx(m.wxCode, m.wxText, sizeof m.wxText, &m.wxHazard);
  size_t rl = strlen(m.raw);
  m.maint = rl >= 2 && m.raw[rl - 1] == '$' && m.raw[rl - 2] == ' ';

  m.vrb  = o["wdir"].is<const char*>();        // "VRB"
  m.wdir = m.vrb ? -1 : (int)(o["wdir"] | -1);
  m.wspd = o["wspd"] | 0;
  m.wgst = o["wgst"] | 0;

  if (o["visib"].is<const char*>()) {          // "10+" or "1/2"
    const char* s = o["visib"];
    strlcpy(m.vis, s, sizeof m.vis);
    const char* slash = strchr(s, '/');
    if (slash) {
      int whole = 0, num = 0, den = 1;
      if (strchr(s, ' ')) sscanf(s, "%d %d/%d", &whole, &num, &den); else sscanf(s, "%d/%d", &num, &den);
      m.visSm = whole + (den ? (float)num / den : 0);
    } else m.visSm = atof(s);
  } else if (o["visib"].is<float>()) {
    m.visSm = o["visib"].as<float>();
    formatVis(m.visSm, m.vis, sizeof m.vis);
  } else { m.visSm = NAN; strlcpy(m.vis, "--", sizeof m.vis); }

  m.temp = o["temp"] | NAN;
  m.dewp = o["dewp"] | NAN;

  // Altimeter: exact inches from the raw "A2991" group; fall back to hPa.
  m.altim = NAN;
  for (const char* p = m.raw; (p = strstr(p, " A")) != nullptr; p += 2) {
    if (isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) && isdigit((unsigned char)p[4]) &&
        isdigit((unsigned char)p[5]) && (p[6] == ' ' || p[6] == 0)) { m.altim = atoi(p + 2) / 100.0f; break; }
  }
  if (isnan(m.altim) && o["altim"].is<float>()) m.altim = o["altim"].as<float>() * 0.0295300f;

  m.ceiling = -1;
  JsonArray cl = o["clouds"].as<JsonArray>();
  for (JsonObject c : cl) {
    if (m.nLayers >= 4) break;
    Layer& L = m.layers[m.nLayers++];
    strlcpy(L.cover, c["cover"] | "", sizeof L.cover);
    L.base = c["base"] | -1;
    bool cig = !strcmp(L.cover, "BKN") || !strcmp(L.cover, "OVC") || !strcmp(L.cover, "OVX");
    if (cig && L.base >= 0 && (m.ceiling < 0 || L.base < m.ceiling)) m.ceiling = L.base;
  }
  if (m.nLayers == 0) {                         // CLR / SKC / CAVOK
    strlcpy(m.layers[0].cover, o["cover"] | "CLR", sizeof m.layers[0].cover);
    m.layers[0].base = -1; m.nLayers = 1;
  }

  m.cigCat = catFromCeiling(m.ceiling);
  m.visCat = catFromVis(m.visSm);
  m.cat = catFromStr(o["fltCat"] | "");
  if (m.cat == CAT_NONE) m.cat = m.cigCat > m.visCat ? m.cigCat : m.visCat;
}

// Distance (nm) and 8-point direction from (lat1,lon1) to (lat2,lon2). Fine at these ranges.
float distNm(float lat1, float lon1, float lat2, float lon2, char* dir) {
  float dy = (lat2 - lat1) * 60.0f;
  float dx = (lon2 - lon1) * 60.0f * cosf(lat1 * DEG_TO_RAD);
  if (dir) {
    static const char* const D8[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    float brg = atan2f(dx, dy) / DEG_TO_RAD; if (brg < 0) brg += 360;
    strcpy(dir, D8[(int)((brg + 22.5f) / 45.0f) % 8]);
  }
  return sqrtf(dx * dx + dy * dy);
}

// Destination has no METAR: use the nearest station that does (~30 nm box, then ~45 nm).
// Returns false only on a network problem.
bool fetchNearest(const Airport& ap, Metar& wx) {
  wx.valid = false;
  if (isnan(ap.lat) || isnan(ap.lon)) return true;
  const float spans[2] = {0.5f, 0.75f};          // degrees of latitude
  for (int pass = 0; pass < 2; pass++) {
    float dLat = spans[pass], dLon = dLat / cosf(ap.lat * DEG_TO_RAD);
    char q[96];
    snprintf(q, sizeof q, "metar?bbox=%.3f,%.3f,%.3f,%.3f&format=json", ap.lat - dLat, ap.lon - dLon, ap.lat + dLat, ap.lon + dLon);
    String body;
    int code = httpGet(String(AWC) + q, body);
    if (code == 204 || (code == 200 && body.length() < 5)) continue;
    if (code != 200) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonObject best; float bestD = 1e9f; long bestT = -1;
    for (JsonObject o : doc.as<JsonArray>()) {
      float la = o["lat"] | NAN, lo = o["lon"] | NAN;
      const char* sid = o["icaoId"] | "";
      if (isnan(la) || isnan(lo) || !strcmp(sid, ap.icao) || (ap.altId[0] && !strcmp(sid, ap.altId))) continue;
      float d = distNm(ap.lat, ap.lon, la, lo, nullptr);
      long t = o["obsTime"] | 0L;
      if (d < bestD - 0.05f || (fabsf(d - bestD) <= 0.05f && t > bestT)) { bestD = d; bestT = t; best = o; }
    }
    if (bestT < 0) continue;
    parseMetar(best, wx);
    wx.proxy = true;
    wx.proxyNm = distNm(ap.lat, ap.lon, wx.lat, wx.lon, wx.proxyDir);
    return true;
  }
  return true;                                   // nothing reporting nearby
}

// --------------------- unofficial AWOS relay (see RELAY_URL) ---------------------
bool hasRelay(const char* icao) {
  if (!RELAY_URL[0] || !icao || !*icao) return false;
  for (size_t i = 0; i < sizeof RELAY_IDS / sizeof RELAY_IDS[0]; i++)
    if (!strcmp(RELAY_IDS[i], icao)) return true;
  return false;
}

// 1 = fresh report, 0 = nothing usable (no file, empty, or too old), -1 = network problem.
int fetchRelay(const char* icao, Metar& wx) {
  if (!hasRelay(icao)) return 0;
  char lc[8]; strlcpy(lc, icao, sizeof lc);
  for (char* p = lc; *p; p++) *p = tolower((unsigned char)*p);
  String body;
  int code = httpGet(String(RELAY_URL) + lc + ".json", body);
  if (code == 404) return 0;
  if (code != 200) return -1;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return -1;
  JsonObject o = doc[0].as<JsonObject>();
  if (o.isNull()) return 0;
  long t = o["obsTime"] | 0L;
  if (t <= 0 || (timeSynced() && (long)time(nullptr) - t > RELAY_MAX_AGE_MIN * 60L)) return 0;
  parseMetar(o, wx);
  wx.relay = true;
  return 1;
}

// The field has no METAR on aviationweather.gov: try its own AWOS through the
// relay, then fall back to the nearest reporting station.
bool noOwnMetar(const Airport& ap, Metar& wx, bool& noMetar) {
  const char* rid = hasRelay(ap.icao) ? ap.icao : (hasRelay(ap.altId) ? ap.altId : nullptr);
  if (rid && fetchRelay(rid, wx) == 1) { noMetar = false; return true; }
  memset(&wx, 0, sizeof wx);
  noMetar = true;
  return fetchNearest(ap, wx);
}

// Returns true if the request worked (even if the station has no METAR).
// Writes only to the Metar/flag passed in, so it's safe to run in the background.
bool fetchWeather(const Airport& ap, Metar& wx, bool& noMetar) {
  String body;
  String ids = ap.icao;
  if (ap.altId[0]) { ids += ","; ids += ap.altId; }
  int code = httpGet(String(AWC) + "metar?ids=" + ids + "&format=json", body);
  if (code == 204 || (code == 200 && body.length() < 5)) return noOwnMetar(ap, wx, noMetar);
  if (code != 200) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonArray a = doc.as<JsonArray>();
  JsonObject best; long bestT = -1;
  for (JsonObject o : a) { long t = o["obsTime"] | 0L; if (t > bestT) { bestT = t; best = o; } }
  if (bestT < 0) return noOwnMetar(ap, wx, noMetar);
  parseMetar(best, wx);
  noMetar = false;
  return true;
}

// ------------------------------- TAF -------------------------------
float visFromJson(JsonVariantConst v) {
  if (v.is<const char*>()) {                     // "6+", "1/2", "1 1/2"
    const char* s = v.as<const char*>();
    if (strchr(s, '/')) {
      int whole = 0, num = 0, den = 1;
      if (strchr(s, ' ')) sscanf(s, "%d %d/%d", &whole, &num, &den); else sscanf(s, "%d/%d", &num, &den);
      return whole + (den ? (float)num / den : 0);
    }
    return atof(s);
  }
  if (v.is<float>()) return v.as<float>();
  return NAN;
}

// Ceiling (ft) from a forecast group; *given = the group says anything about sky.
int cigFromJson(JsonObjectConst f, bool* given) {
  *given = false;
  if (!f["vertVis"].isNull()) {                  // obscured sky: vertical visibility is the ceiling
    *given = true;
    int vv = f["vertVis"].as<int>();
    return vv < 100 ? vv * 100 : vv;
  }
  JsonArrayConst cl = f["clouds"].as<JsonArrayConst>();
  if (cl.isNull() || cl.size() == 0) return -1;
  *given = true;
  int c = -1;
  for (JsonObjectConst k : cl) {
    const char* cov = k["cover"] | "";
    int base = k["base"] | -1;
    bool cig = !strcmp(cov, "BKN") || !strcmp(cov, "OVC") || !strcmp(cov, "OVX");
    if (cig && base >= 0 && (c < 0 || base < c)) c = base;
  }
  return c;
}

bool tempoLike(JsonObjectConst f) {
  const char* ch = f["fcstChange"] | "";
  return !strcmp(ch, "TEMPO") || !strncmp(ch, "PROB", 4) || !f["probability"].isNull();
}

Cat catOf(float vis, int cig) {
  Cat a = catFromVis(vis), b = catFromCeiling(cig);
  return a > b ? a : b;
}

// Category for each of the next TAF_HOURS hours. FM groups replace the forecast,
// BECMG changes only what it mentions, TEMPO/PROB are shown as a worse band.
void computeTaf(JsonObjectConst t, Taf& out, long now) {
  long vf = t["validTimeFrom"] | 0L, vt = t["validTimeTo"] | 0L;
  long h0 = now - now % 3600;
  if (h0 < vf - 3600) h0 = vf - vf % 3600;
  out.start = h0;
  out.valid = true;
  JsonArrayConst fc = t["fcsts"].as<JsonArrayConst>();
  for (int k = 0; k < TAF_HOURS; k++) {
    long tm = h0 + k * 3600L + 1800;             // middle of the hour
    out.base[k] = CAT_NONE; out.temp[k] = CAT_NONE;
    if (tm < vf || tm >= vt) continue;
    float pv = NAN; int pc = -1; bool any = false;
    for (JsonObjectConst f : fc) {
      if (tempoLike(f)) continue;
      long tf = f["timeFrom"] | 0L;
      if (tf > tm) continue;
      bool bec = !strcmp(f["fcstChange"] | "", "BECMG");
      bool g; int c = cigFromJson(f, &g);
      float v = visFromJson(f["visib"]);
      if (!bec) { pv = v; pc = g ? c : -1; }       // initial group or FM: the whole picture
      else { if (!isnan(v)) pv = v; if (g) pc = c; }
      any = true;
    }
    if (!any) continue;
    Cat b = catOf(pv, pc), w = CAT_NONE;
    for (JsonObjectConst f : fc) {
      if (!tempoLike(f)) continue;
      long tf = f["timeFrom"] | 0L, tt = f["timeTo"] | 0L;
      if (tm < tf || tm >= tt) continue;
      bool g; int c = cigFromJson(f, &g);
      float v = visFromJson(f["visib"]);
      Cat tc = catOf(isnan(v) ? pv : v, g ? c : pc);
      if (tc > b && tc > w) w = tc;
    }
    out.base[k] = b; out.temp[k] = w;
  }
}

// Destination's own TAF, else the nearest one within ~50 nm. False only on a network problem.
bool fetchTaf(const Airport& a, Taf& t) {
  memset(&t, 0, sizeof t);
  String ids = a.icao;
  if (a.altId[0]) { ids += ","; ids += a.altId; }
  String body;
  int code = httpGet(String(AWC) + "taf?ids=" + ids + "&format=json", body);
  if (code != 200 && code != 204) return false;
  JsonDocument doc;
  JsonObjectConst best;
  if (code == 200 && body.length() > 5 && !deserializeJson(doc, body)) {
    for (JsonObjectConst o : doc.as<JsonArrayConst>()) { if (best.isNull() || (o["mostRecent"] | 0) == 1) best = o; }
  }
  if (best.isNull() && !isnan(a.lat) && !isnan(a.lon)) {
    float dLat = 0.8f, dLon = dLat / cosf(a.lat * DEG_TO_RAD);
    char q[96];
    snprintf(q, sizeof q, "taf?bbox=%.3f,%.3f,%.3f,%.3f&format=json", a.lat - dLat, a.lon - dLon, a.lat + dLat, a.lon + dLon);
    body = "";
    code = httpGet(String(AWC) + q, body);
    if (code != 200 && code != 204) return false;
    doc.clear();
    if (code == 200 && body.length() > 5 && !deserializeJson(doc, body)) {
      float bd = 1e9f;
      for (JsonObjectConst o : doc.as<JsonArrayConst>()) {
        float la = o["lat"] | NAN, lo = o["lon"] | NAN;
        if (isnan(la) || isnan(lo)) continue;
        float d = distNm(a.lat, a.lon, la, lo, nullptr);
        if (d < bd) { bd = d; best = o; }
      }
      if (!best.isNull()) {
        t.proxy = true;
        t.distNm = distNm(a.lat, a.lon, best["lat"] | 0.0f, best["lon"] | 0.0f, t.dir);
      }
    }
  }
  if (best.isNull()) return true;                // no TAF anywhere near: fine
  strlcpy(t.icao, best["icaoId"] | "", sizeof t.icao);
  long now = timeSynced() ? (long)time(nullptr) : (long)(best["validTimeFrom"] | 0L);
  t.issue = best["validTimeFrom"] | 0L;
  computeTaf(best, t, now);
  return true;
}

Taf taf;

int fetchAirportInfo(Airport& a);

// One weather refresh: works on copies, then commits under the data lock, and
// only if the airport hasn't changed meanwhile. Used by the background task and
// by foreground loads (which hold netMutex, so nothing else is fetching).
Airport jobAp;
Metar   jobWx;
void wxJob() {
  uint32_t gen; bool infoDone;
  DATA_LOCK(); jobAp = ap; gen = apGen; infoDone = apInfoOk; DATA_UNLOCK();
  int info = 0;
  if (!infoDone && WiFi.status() == WL_CONNECTED) info = fetchAirportInfo(jobAp);
  bool nm = false;
  memset(&jobWx, 0, sizeof jobWx);
  bool ok = fetchWeather(jobAp, jobWx, nm);
  static Taf jobTaf;
  bool tafOk = ok && fetchTaf(jobAp, jobTaf);
  DATA_LOCK();
  if (gen == apGen) {
    if (info != 0) apInfoOk = true;
    if (info > 0) {
      strlcpy(ap.name, jobAp.name, sizeof ap.name); ap.elevFt = jobAp.elevFt;
      ap.lat = jobAp.lat; ap.lon = jobAp.lon; strlcpy(ap.altId, jobAp.altId, sizeof ap.altId);
      memcpy(ap.rwy, jobAp.rwy, sizeof ap.rwy); ap.nRwy = jobAp.nRwy;
      strlcpy(ap.awcFreqs, jobAp.awcFreqs, sizeof ap.awcFreqs);
    }
    if (ok) { wx = jobWx; noMetar = nm; lastOkMs = millis(); nextFetchMs = millis() + cadence * 60000UL; }
    if (tafOk) taf = jobTaf;
    else nextFetchMs = millis() + RETRY_MS;
    lastFetchOk = ok;
    everFetched = everFetched || ok;
  }
  DATA_UNLOCK();
  dirty = true;
}

void doFetch() { wxJob(); }

// Same airport? Handles 3-character FAA idents (2G4) vs ICAO (K2G4).
bool sameAirport(const char* id, JsonObject o) {
  const char* icao = o["icaoId"] | "";
  const char* faa  = o["faaId"] | "";
  if (!strcmp(id, icao) || (*faa && !strcmp(id, faa))) return true;
  return strlen(id) == 3 && icao[0] == 'K' && !strcmp(icao + 1, id);
}

// Picker: one request for every tile's category + name. Fills the array given.
bool fetchTileCats(Tile* tiles, int nTiles) {
  String ids;
  for (int i = 0; i < nTiles; i++) { if (i) ids += ","; ids += tiles[i].icao; }
  String body;
  JsonDocument doc;
  // Starlink often has Wi-Fi up before it has internet: the caller retries.
  int code = httpGet(String(AWC) + "metar?ids=" + ids + "&format=json", body);
  if ((code != 200 && code != 204) || (code == 200 && deserializeJson(doc, body))) return false;
  long bestT[6] = {0};
  for (JsonObject o : doc.as<JsonArray>()) {
    long t = o["obsTime"] | 0L;
    for (int i = 0; i < nTiles; i++) {
      if (!sameAirport(tiles[i].icao, o) || t < bestT[i]) continue;
      bestT[i] = t;
      tiles[i].hasWx = true;
      tiles[i].cat = catFromStr(o["fltCat"] | "");
      shortName(o["name"] | "", tiles[i].name, sizeof tiles[i].name);
    }
  }
  // Fields with no METAR of their own but a relay (KFWQ): category from the relay.
  for (int i = 0; i < nTiles; i++) {
    if (tiles[i].hasWx || !hasRelay(tiles[i].icao)) continue;
    static Metar rm;
    if (fetchRelay(tiles[i].icao, rm) == 1) { tiles[i].hasWx = true; tiles[i].cat = rm.cat; }
  }
  // Fields with no METAR (like KFWQ) still need a name: get it from the airport records.
  String missing;
  for (int i = 0; i < nTiles; i++) if (!tiles[i].name[0]) { if (missing.length()) missing += ","; missing += tiles[i].icao; }
  if (!missing.length()) return true;
  String body2;
  JsonDocument doc2;
  if (httpGet(String(AWC) + "airport?ids=" + missing + "&format=json", body2) != 200 || deserializeJson(doc2, body2)) return true;
  for (JsonObject o : doc2.as<JsonArray>())
    for (int i = 0; i < nTiles; i++) if (sameAirport(tiles[i].icao, o)) shortName(o["name"] | "", tiles[i].name, sizeof tiles[i].name);
  return true;
}

Tile jobTiles[6];
void tilesJob() {
  int n; uint32_t gen;
  DATA_LOCK(); n = nTiles; memcpy(jobTiles, tiles, sizeof jobTiles); gen = tilesGen; DATA_UNLOCK();
  bool ok = fetchTileCats(jobTiles, n);
  DATA_LOCK();
  if (gen == tilesGen) {
    if (ok) memcpy(tiles, jobTiles, sizeof jobTiles);
    else { tilesFetched = false; tilesRetryMs = millis() + 20000; }
  }
  DATA_UNLOCK();
  dirty = true;
}

// ---------------------------- frequencies ----------------------------
const TypeInfo* typeInfo(const char* t) {
  for (int i = 0; i < N_TYPES; i++) if (!strcasecmp(TYPES[i].type, t)) return &TYPES[i];
  return nullptr;
}

void addFreq(Airport& a, const char* type, const char* desc, float mhz) {
  if (a.nFq >= (int)(sizeof a.fq / sizeof a.fq[0])) return;
  if (mhz < 108.0f || mhz > 137.0f) return;    // VHF airband only
  const TypeInfo* ti = typeInfo(type);
  if (!ti) return;
  Freq& f = a.fq[a.nFq];
  snprintf(f.mhz, sizeof f.mhz, "%.3f", mhz);
  for (int i = 0; i < a.nFq; i++) if (!strcmp(a.fq[i].mhz, f.mhz) && !strcmp(a.fq[i].type, ti->type)) return;
  strlcpy(f.type, ti->type, sizeof f.type);
  f.prio = ti->prio;
  if (!desc || !*desc || !strcasecmp(desc, type)) strlcpy(f.label, ti->label, sizeof f.label);
  else { strlcpy(f.label, desc, sizeof f.label); upcase(f.label); }
  a.nFq++;
}

void removeFreq(Airport& a, int i) {
  for (int j = i; j < a.nFq - 1; j++) a.fq[j] = a.fq[j + 1];
  a.nFq--;
}

int findType(const Airport& a, const char* t) {
  for (int i = 0; i < a.nFq; i++) if (!strcmp(a.fq[i].type, t)) return i;
  return -1;
}

void finalizeFreqs(Airport& a) {
  // Part-time tower whose CTAF is the tower frequency -> one line.
  int twr = findType(a, "TWR");
  for (int i = a.nFq - 1; twr >= 0 && i >= 0; i--) {
    if (!strcmp(a.fq[i].type, "CTAF") && !strcmp(a.fq[i].mhz, a.fq[twr].mhz)) {
      strlcpy(a.fq[twr].label, "TOWER / CTAF", sizeof a.fq[twr].label);
      removeFreq(a, i);
      twr = findType(a, "TWR");
    }
  }
  // CTAF and UNICOM on the same frequency -> one line.
  int ctaf = findType(a, "CTAF");
  for (int i = a.nFq - 1; ctaf >= 0 && i >= 0; i--) {
    if (!strcmp(a.fq[i].type, "UNIC") && !strcmp(a.fq[i].mhz, a.fq[ctaf].mhz)) {
      strlcpy(a.fq[ctaf].label, "CTAF/UNICOM", sizeof a.fq[ctaf].label);
      removeFreq(a, i);
      ctaf = findType(a, "CTAF");
    }
  }
  // Stable sort by priority.
  for (int i = 1; i < a.nFq; i++) {
    Freq k = a.fq[i]; int j = i - 1;
    while (j >= 0 && a.fq[j].prio > k.prio) { a.fq[j + 1] = a.fq[j]; j--; }
    a.fq[j + 1] = k;
  }
  a.primary = findType(a, "TWR");
  if (a.primary < 0) a.primary = findType(a, "CTAF");
  if (a.primary < 0) a.primary = findType(a, "UNIC");
  if (a.primary < 0 && a.nFq) a.primary = 0;
}

// Cache format: "label|mhz|prio|type\n" per line.
void freqsFromCache(Airport& a, const String& s) {
  a.nFq = 0;
  int start = 0;
  while (start < (int)s.length() && a.nFq < (int)(sizeof a.fq / sizeof a.fq[0])) {
    int end = s.indexOf('\n', start); if (end < 0) end = s.length();
    String line = s.substring(start, end);
    start = end + 1;
    int p1 = line.indexOf('|'), p2 = line.indexOf('|', p1 + 1), p3 = line.indexOf('|', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) continue;
    Freq& f = a.fq[a.nFq++];
    strlcpy(f.label, line.substring(0, p1).c_str(), sizeof f.label);
    strlcpy(f.mhz, line.substring(p1 + 1, p2).c_str(), sizeof f.mhz);
    f.prio = line.substring(p2 + 1, p3).toInt();
    strlcpy(f.type, line.substring(p3 + 1).c_str(), sizeof f.type);
  }
  finalizeFreqs(a);
}

String freqsToCache(const Airport& a) {
  String s;
  for (int i = 0; i < a.nFq; i++) {
    s += a.fq[i].label; s += '|'; s += a.fq[i].mhz; s += '|'; s += a.fq[i].prio; s += '|'; s += a.fq[i].type; s += '\n';
  }
  return s;
}

// Split one CSV line in place. Handles double-quoted fields.
int splitCsv(char* line, char* f[], int maxF) {
  int n = 0; char* p = line;
  while (*p && n < maxF) {
    if (*p == '"') {
      f[n++] = ++p;
      while (*p && *p != '"') p++;
      if (*p == '"') *p++ = 0;
      if (*p == ',') p++;
    } else {
      f[n++] = p;
      while (*p && *p != ',') p++;
      if (*p == ',') *p++ = 0;
    }
  }
  return n;
}

void setLoad(int line, const char* s) { strlcpy(loadLines[line], s, sizeof loadLines[line]); dirty = true; }
void drawNow();

// Streams the ~1.3 MB OurAirports file and keeps only this airport's rows.
// The file is grouped by airport, so we stop right after our block.
// progress: show a progress bar (foreground only). gen: give up if the airport changes.
bool fetchFreqsOurAirports(Airport& a, bool progress, uint32_t gen) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
  HTTPClient http;
  http.useHTTP10(true);                         // no chunked encoding -> plain stream
  http.setConnectTimeout(6000);
  http.setTimeout(15000);
  if (!http.begin(client, OA_URL)) return false;
  http.setUserAgent(UA);
  if (http.GET() != 200) { http.end(); return false; }

  int total = http.getSize(), got = 0, lastPct = -1;
  Stream* s = http.getStreamPtr();
  char needle[12]; snprintf(needle, sizeof needle, "\"%s\"", a.icao);
  char needle2[12] = "";
  if (a.altId[0]) snprintf(needle2, sizeof needle2, "\"%s\"", a.altId);
  char line[200]; int li = 0;
  uint8_t buf[1024];
  bool found = false, done = false;
  uint32_t lastData = millis();
  a.nFq = 0;

  while (!done && (total < 0 || got < total)) {
    if (gen != apGen || (!progress && fgWaiting)) { http.end(); return false; }   // user moved on
    int av = s->available();
    if (av <= 0) {
      if (!http.connected() || millis() - lastData > 15000) break;
      delay(2);
      continue;
    }
    int n = s->readBytes(buf, av < (int)sizeof buf ? av : (int)sizeof buf);
    got += n; lastData = millis();
    for (int i = 0; i < n && !done; i++) {
      char c = (char)buf[i];
      if (c != '\n') { if (c != '\r' && li < (int)sizeof line - 1) line[li++] = c; continue; }
      line[li] = 0; li = 0;
      if (strstr(line, needle) || (needle2[0] && strstr(line, needle2))) {
        char* f[6];
        if (splitCsv(line, f, 6) == 6 && (!strcmp(f[2], a.icao) || (a.altId[0] && !strcmp(f[2], a.altId)))) {
          addFreq(a, f[3], f[4], atof(f[5])); found = true;
        }
      } else if (found) done = true;
    }
    if (progress && total > 0) {
      int pct = (int)(100LL * got / total);
      if (pct / 5 != lastPct / 5) {
        lastPct = pct; loadPct = pct;
        char t[44]; snprintf(t, sizeof t, "Frequencies  %d%%", pct); setLoad(2, t); drawNow();
      }
    }
  }
  http.end();
  if (found) finalizeFreqs(a);
  return found;
}

// AWC's airport record has a short freq list, e.g. "ATIS,120.55;LCL/P,121.1"
void freqsFromAwc(Airport& a) {
  a.nFq = 0;
  char buf[120]; strlcpy(buf, a.awcFreqs, sizeof buf);
  for (char* tok = strtok(buf, ";"); tok; tok = strtok(nullptr, ";")) {
    char* comma = strchr(tok, ','); if (!comma) continue;
    *comma = 0;
    char type[8]; strlcpy(type, tok, sizeof type);
    if (!strncmp(type, "LCL", 3)) strlcpy(type, "TWR", sizeof type);
    else if (!strncmp(type, "GND", 3)) strlcpy(type, "GND", sizeof type);
    else if (strstr(type, "ATIS")) strlcpy(type, "ATIS", sizeof type);
    addFreq(a, type, "", atof(comma + 1));
  }
  finalizeFreqs(a);
}

// ------------------------- SD frequency database -------------------------
// File format, one line per frequency, sorted by ident (the source is):
//   IDENT|TYPE|DESCRIPTION|MHZ
bool dbReadLine(File& f, char* buf, int n) {
  int i = 0, c = -1;
  while ((c = f.read()) >= 0) { if (c == '\n') break; if (c != '\r' && i < n - 1) buf[i++] = (char)c; }
  buf[i] = 0;
  return i > 0 || c >= 0;
}

// Adds every database row for one ident. Caller holds BUS_LOCK.
int dbFind(File& f, const char* id, Airport& a) {
  if (!id || !*id) return 0;
  char line[160];
  uint32_t lo = 0, hi = f.size();
  while (hi - lo > 512) {                        // binary search on byte offsets
    uint32_t mid = lo + (hi - lo) / 2;
    f.seek(mid);
    dbReadLine(f, line, sizeof line);            // skip the partial line
    uint32_t start = f.position();
    if (!dbReadLine(f, line, sizeof line)) { hi = mid; continue; }
    char* bar = strchr(line, '|'); if (bar) *bar = 0;
    if (strcmp(line, id) < 0) lo = start; else hi = mid;
  }
  f.seek(lo);
  if (lo) { /* lo is always a line start */ }
  int n = 0;
  while (dbReadLine(f, line, sizeof line)) {
    char* fld[4]; int k = 0; char* p = line;
    fld[k++] = p;
    while (*p && k < 4) { if (*p == '|') { *p = 0; fld[k++] = p + 1; } p++; }
    if (k < 4) continue;
    int cmp = strcmp(fld[0], id);
    if (cmp < 0) continue;
    if (cmp > 0) break;
    addFreq(a, fld[1], fld[2], atof(fld[3])); n++;
  }
  return n;
}

// Looks the airport up under every spelling we know (KFWQ / FWQ, 2G4 / K2G4).
bool dbLookup(Airport& a) {
  if (!sdOk || !dbStamp) return false;
  char kid[6] = "";
  if (strlen(a.icao) == 3 && !a.altId[0]) snprintf(kid, sizeof kid, "K%s", a.icao);
  a.nFq = 0;
  BUS_LOCK();
  File f = SD.open(DB_PATH, FILE_READ);
  int n = 0;
  if (f) {
    n = dbFind(f, a.icao, a);
    if (!n) n = dbFind(f, a.altId, a);
    if (!n) n = dbFind(f, kid, a);
    f.close();
  }
  BUS_UNLOCK();
  if (!n) return false;
  finalizeFreqs(a);
  char d[24] = "Database";
  if (dbStamp > 1) { time_t t = dbStamp; struct tm g; gmtime_r(&t, &g); snprintf(d, sizeof d, "Database %d/%d", g.tm_mon + 1, g.tm_mday); }
  strlcpy(a.fqSrc, d, sizeof a.fqSrc);
  return true;
}

// Background: stream the OurAirports file to the card, keeping only the
// frequencies the panel shows. Gives up (and tries later) if anything else needs the network.
void dbJob() {
  if (!sdOk || WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client; client.setInsecure(); client.setHandshakeTimeout(10);
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(6000);
  http.setTimeout(15000);
  if (!http.begin(client, OA_URL)) { dbRetryMs = millis() + 600000UL; return; }
  http.setUserAgent(UA);
  if (http.GET() != 200) { http.end(); dbRetryMs = millis() + 600000UL; return; }

  BUS_LOCK();
  SD.mkdir(DB_DIR);
  SD.remove(DB_TMP);
  File out = SD.open(DB_TMP, FILE_WRITE);
  BUS_UNLOCK();
  if (!out) { http.end(); dbRetryMs = millis() + 3600000UL; return; }

  int total = http.getSize(), got = 0;
  Stream* s = http.getStreamPtr();
  static char wbuf[4096]; int wn = 0;
  static uint8_t buf[1024];
  char line[200]; int li = 0;
  uint32_t rows = 0, lastData = millis();
  bool ok = true, ended = false;
  dbPct = 0;
  while (total < 0 || got < total) {
    if (fgWaiting || wState == W_SETUP) { ok = false; break; }
    int av = s->available();
    if (av <= 0) {
      if (!http.connected()) { ended = true; break; }
      if (millis() - lastData > 15000) { ok = false; break; }
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    int n = s->readBytes(buf, av < (int)sizeof buf ? av : (int)sizeof buf);
    got += n; lastData = millis();
    for (int i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c != '\n') { if (c != '\r' && li < (int)sizeof line - 1) line[li++] = c; continue; }
      line[li] = 0; li = 0;
      char* f[6];
      if (splitCsv(line, f, 6) != 6) continue;
      const TypeInfo* ti = typeInfo(f[3]);
      float mhz = atof(f[5]);
      if (!ti || mhz < 108.0f || mhz > 137.0f || !f[2][0]) continue;
      for (char* q = f[4]; *q; q++) if (*q == '|') *q = '/';
      char row[120];
      int len = snprintf(row, sizeof row, "%s|%s|%s|%.3f\n", f[2], f[3], f[4], mhz);
      if (len <= 0 || len >= (int)sizeof row) continue;
      if (wn + len > (int)sizeof wbuf) {
        BUS_LOCK(); size_t w = out.write((const uint8_t*)wbuf, wn); BUS_UNLOCK();
        if ((int)w != wn) { ok = false; break; }
        wn = 0;
      }
      memcpy(wbuf + wn, row, len); wn += len; rows++;
    }
    if (!ok) break;
    if (total > 0) { int p = (int)(100LL * got / total); if (p != dbPct) { dbPct = p; dirty = true; } }
  }
  http.end();
  if (ok && wn) { BUS_LOCK(); ok = out.write((const uint8_t*)wbuf, wn) == (size_t)wn; BUS_UNLOCK(); }
  BUS_LOCK();
  out.close();
  bool complete = ok && rows > 10000 && (total < 0 ? ended : got >= total);
  if (complete) { SD.remove(DB_PATH); complete = SD.rename(DB_TMP, DB_PATH); }
  else SD.remove(DB_TMP);
  BUS_UNLOCK();
  dbPct = -1; dirty = true;
  if (!complete) { dbRetryMs = millis() + (fgWaiting ? 120000UL : 900000UL); return; }
  DATA_LOCK();
  dbStamp = timeSynced() ? (uint32_t)time(nullptr) : 1;
  dbRows = rows;
  prefs.putULong("dbt", dbStamp);
  prefs.putULong("dbn", dbRows);
  // The current airport had no (or only partial) frequencies? Fill them in now.
  if (airportLoaded && (ap.nFq == 0 || !strncmp(ap.fqSrc, "AWC", 3))) needFreqs = true;
  DATA_UNLOCK();
}

void sdInit() {
  int cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (cs < 0) cs = GPIO_NUM_4;
  if (M5.hasSD())
    SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk), M5.getPin(m5::pin_name_t::sd_spi_miso),
              M5.getPin(m5::pin_name_t::sd_spi_mosi), cs);
  sdOk = SD.begin(cs, SPI, 25000000);
  if (!sdOk) return;
  SD.mkdir(DB_DIR);
  dbStamp = prefs.getULong("dbt", 0);
  dbRows  = prefs.getULong("dbn", 0);
  if (!SD.exists(DB_PATH)) { dbStamp = 0; dbRows = 0; }
  SD.remove(DB_TMP);                              // leftover from an interrupted download
}

// Foreground: built-in list, then the SD database, then this panel's saved copy, then download.
void loadFreqs(Airport& a, bool forceDownload) {
  String key = String("f") + a.icao;
  const char* built = builtinFreqsFor(a.icao, a.altId);
  if (!forceDownload && built) {
    freqsFromCache(a, built);
    strlcpy(a.fqSrc, "Built-in (Oct 2026)", sizeof a.fqSrc);
    setLoad(2, "Frequencies  built-in");
    return;
  }
  if (!forceDownload && dbLookup(a)) { setLoad(2, "Frequencies  from SD card"); return; }
  if (!forceDownload && prefs.isKey(key.c_str())) {
    freqsFromCache(a, prefs.getString(key.c_str(), ""));
    if (a.nFq) { strlcpy(a.fqSrc, "OurAirports (saved)", sizeof a.fqSrc); setLoad(2, "Frequencies  saved"); return; }
  }
  setLoad(2, "Frequencies  0%"); drawNow();
  if (fetchFreqsOurAirports(a, true, apGen)) {
    prefs.putString(key.c_str(), freqsToCache(a));
    strlcpy(a.fqSrc, "OurAirports", sizeof a.fqSrc);
    setLoad(2, "Frequencies  OK");
    return;
  }
  if (a.awcFreqs[0] && strcmp(a.awcFreqs, "-")) {
    freqsFromAwc(a);
    if (a.nFq) { strlcpy(a.fqSrc, "AWC (partial list)", sizeof a.fqSrc); setLoad(2, "Frequencies  partial"); return; }
  }
  if (built) {                                  // a forced re-download failed: fall back
    freqsFromCache(a, built);
    strlcpy(a.fqSrc, "Built-in (Oct 2026)", sizeof a.fqSrc);
    setLoad(2, "Frequencies  built-in");
    return;
  }
  // Keep whatever was there (e.g. a failed re-download) or report none.
  if (prefs.isKey(key.c_str())) { freqsFromCache(a, prefs.getString(key.c_str(), "")); strlcpy(a.fqSrc, "OurAirports (saved)", sizeof a.fqSrc); }
  else { a.nFq = 0; a.primary = -1; strlcpy(a.fqSrc, "Not available", sizeof a.fqSrc); }
  setLoad(2, "Frequencies  unavailable");
}

// Background: airport was loaded offline, so download its frequencies now.
void freqsJob() {
  uint32_t gen;
  DATA_LOCK(); jobAp = ap; gen = apGen; DATA_UNLOCK();
  bool fromDb = dbLookup(jobAp);
  if (!fromDb && !fetchFreqsOurAirports(jobAp, false, gen)) return;
  DATA_LOCK();
  if (gen == apGen) {
    memcpy(ap.fq, jobAp.fq, sizeof ap.fq); ap.nFq = jobAp.nFq; ap.primary = jobAp.primary;
    strlcpy(ap.fqSrc, fromDb ? jobAp.fqSrc : "OurAirports", sizeof ap.fqSrc);
    if (!fromDb) prefs.putString((String("f") + ap.icao).c_str(), freqsToCache(ap));
  }
  DATA_UNLOCK();
  dirty = true;
}

// The background worker: one job at a time, never while setting up Wi-Fi.
void netStep() {
  if (WiFi.status() != WL_CONNECTED || wState == W_SETUP) return;
  if (!(needWx || needTiles || needFreqs || needDb)) return;
  if (fgWaiting) return;                          // let the screen side go first
  NET_LOCK();
  netBusy = true; dirty = true;
  if (needTiles) { needTiles = false; tilesJob(); }
  if (needWx)    { needWx = false;    wxJob(); }
  if (needFreqs) { needFreqs = false; freqsJob(); }
  // The big database download only runs when nothing else is waiting.
  if (needDb && !needTiles && !needWx && !needFreqs) { needDb = false; dbJob(); }
  netBusy = false; dirty = true;
  NET_UNLOCK();
}

// Foreground network work (loading an airport etc.): ask long background jobs
// to stop, then take the network.
void fgNetBegin() { fgWaiting = true; NET_LOCK(); fgWaiting = false; }
void fgNetEnd()   { NET_UNLOCK(); }

void netTask(void*) {
  for (;;) { netStep(); vTaskDelay(pdMS_TO_TICKS(50)); }
}

// --------------------------- airport info ---------------------------
// Returns -1 not found, 0 network problem (keep going), 1 OK.
int fetchAirportInfo(Airport& a) {
  String body;
  int code = httpGet(String(AWC) + "airport?ids=" + a.icao + "&format=json", body);
  if (code == 204 || (code == 200 && body.length() < 5)) return -1;
  if (code != 200) return 0;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return 0;
  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() == 0) return -1;
  JsonObject o = arr[0];
  shortName(o["name"] | "", a.name, sizeof a.name);
  const char* icaoId = o["icaoId"] | "";
  const char* faaId  = o["faaId"] | "";
  a.altId[0] = 0;
  if (*icaoId && strcmp(icaoId, a.icao)) strlcpy(a.altId, icaoId, sizeof a.altId);
  else if (*faaId && strcmp(faaId, a.icao)) strlcpy(a.altId, faaId, sizeof a.altId);
  a.elevFt = o["elev"].is<float>() ? (int)lroundf(o["elev"].as<float>() * 3.28084f) : -1;
  a.lat = o["lat"] | NAN;
  a.lon = o["lon"] | NAN;
  strlcpy(a.awcFreqs, o["freqs"] | "", sizeof a.awcFreqs);
  a.nRwy = 0;
  for (JsonObject r : o["runways"].as<JsonArray>()) {
    if (a.nRwy >= 6) break;
    if (!r["alignment"].is<int>() && !r["alignment"].is<float>()) continue;   // helipads etc.
    const char* id = r["id"].is<const char*>() ? r["id"].as<const char*>() : (r["ident"] | "");
    const char* slash = strchr(id, '/');
    if (!slash) continue;
    Runway& R = a.rwy[a.nRwy++];
    size_t l0 = slash - id; if (l0 > 4) l0 = 4;
    memcpy(R.ends[0], id, l0); R.ends[0][l0] = 0;
    strlcpy(R.ends[1], slash + 1, sizeof R.ends[1]);
    R.hdg = (int)lroundf(r["alignment"].as<float>());   // true heading, same reference as METAR wind
  }
  return 1;
}

// Returns false only when the identifier definitely doesn't exist.
bool loadAirport(const char* icao) {
  DATA_LOCK();
  apGen++;                                      // anything in flight for the old airport is dropped
  needWx = false; needFreqs = false; freqsPending = false;
  memset(&ap, 0, sizeof ap);
  memset(&wx, 0, sizeof wx);
  memset(&taf, 0, sizeof taf);
  windSel = -1;
  strlcpy(ap.icao, icao, sizeof ap.icao);
  ap.primary = -1; ap.elevFt = -1; ap.lat = NAN; ap.lon = NAN;
  noMetar = false; everFetched = false; lastFetchOk = false; loadPct = -1;
  for (int i = 0; i < 3; i++) loadLines[i][0] = 0;
  screen = SCR_LOADING;
  DATA_UNLOCK();

  // Give Wi-Fi a moment if it's still joining.
  // (No point waiting when nothing is saved or nothing saved is in range.)
  if (WiFi.status() != WL_CONNECTED && knownCount() > 0 && wState != W_NONE_SEEN) {
    setLoad(0, "Waiting for Wi-Fi"); drawNow();
    for (uint32_t t0 = millis(); WiFi.status() != WL_CONNECTED && millis() - t0 < 15000; ) { M5.update(); wifiTick(); delay(100); }
  }

  setLoad(0, "Airport info ..."); drawNow();
  int info = fetchAirportInfo(ap);
  if (info < 0) {
    // Offline lookups can't tell "doesn't exist" from "no internet"; only a real "not found" lands here.
    return false;
  }
  apInfoOk = info > 0;
  setLoad(0, apInfoOk ? "Airport info  OK" : "Airport info  offline");

  setLoad(1, "Weather ..."); drawNow();
  doFetch();
  char wl[44];
  if (!lastFetchOk) strlcpy(wl, "Weather  will retry", sizeof wl);
  else if (wx.valid && wx.proxy) snprintf(wl, sizeof wl, "Weather  no METAR, using %s", wx.icao);
  else if (wx.valid && wx.relay) strlcpy(wl, "Weather  own AWOS (unofficial)", sizeof wl);
  else strlcpy(wl, noMetar ? "Weather  none nearby" : "Weather  OK", sizeof wl);
  setLoad(1, wl);
  drawNow();

  loadFreqs(ap, false);
  freqsPending = ap.nFq == 0 && WiFi.status() != WL_CONNECTED;
  if (!ap.name[0] && wx.valid) strlcpy(ap.name, wx.name, sizeof ap.name);
  return true;
}

// ============================ flow ============================
void buildTiles() {
  DATA_LOCK();
  tilesGen++;
  nTiles = 0;
  auto add = [](const char* id, const char* tag) {
    if (nTiles >= 6 || !id || !*id) return;
    for (int i = 0; i < nTiles; i++) if (!strcmp(tiles[i].icao, id)) return;
    Tile& t = tiles[nTiles++];
    memset(&t, 0, sizeof t);
    strlcpy(t.icao, id, sizeof t.icao); strlcpy(t.tag, tag, sizeof t.tag);
  };
  add(HOME_ICAO, "HOME");
  add(lastDest, "LAST");
  for (size_t i = 0; i < sizeof FAVORITES / sizeof FAVORITES[0]; i++) add(FAVORITES[i], "FAV");
  tilesFetched = false; tilesRetryMs = 0;
  DATA_UNLOCK();
}

void openPicker() {
  buildTiles();
  screen = SCR_PICK;
  dirty = true;
}

void openEntry() {
  const char* seed = airportLoaded ? ap.icao : lastDest;
  for (int i = 0; i < 4; i++) entryChars[i] = (i < (int)strlen(seed)) ? seed[i] : ' ';
  entrySel = 0; entryMsg[0] = 0; entryPad = false;
  screen = SCR_ENTRY; dirty = true;
}

void beginLoad(const char* icao) {
  fgNetBegin();                                 // background jobs finish or bail first
  prevAp = ap; prevWx = wx;
  static Taf prevTaf; prevTaf = taf;
  bool prevInfo = apInfoOk;
  if (!loadAirport(icao)) {
    if (airportLoaded) { DATA_LOCK(); ap = prevAp; wx = prevWx; taf = prevTaf; apInfoOk = prevInfo; apGen++; nextFetchMs = millis(); DATA_UNLOCK(); }
    fgNetEnd();
    openEntry();
    for (int i = 0; i < 4; i++) entryChars[i] = (i < (int)strlen(icao)) ? icao[i] : ' ';
    snprintf(entryMsg, sizeof entryMsg, "%s not found", icao);
    return;
  }
  fgNetEnd();
  airportLoaded = true;
  startupFlow = false;
  strlcpy(lastDest, icao, sizeof lastDest);
  prefs.putString("dest", lastDest);
  freqPage = 0;
  screen = SCR_WX;
  dirty = true;
}

void chooseAirport(const char* icao) {
  if (startupFlow && !cadenceChosen) { strlcpy(pendingIcao, icao, sizeof pendingIcao); screen = SCR_CADENCE; dirty = true; }
  else beginLoad(icao);
}

void setCadence(uint8_t m) {
  cadence = m;
  cadenceChosen = true;
  prefs.putUChar("cad", m);
  if (airportLoaded) {                          // reschedule from the last good fetch
    nextFetchMs = lastFetchOk ? lastOkMs + m * 60000UL : millis();
    screen = SCR_WX; dirty = true;
  } else beginLoad(pendingIcao);
}

// ============================ drawing ============================
void drawKeys(const char* a, const char* b, const char* c, int active) {
  const char* lab[3] = {a, b, c};
  const int xs[3] = {53, 160, 267};
  for (int i = 0; i < 3; i++) {
    if (!lab[i] || !*lab[i]) continue;
    bool on = (i == active);
    txt(lab[i], xs[i], 231, &fonts::DejaVu12, on ? C_CYAN : C_LBL, textdatum_t::middle_center);
    if (on) cv.fillRect(xs[i] - 12, 238, 24, 2, C_CYAN);
  }
}

void drawHeader() {
  txt(ap.icao, 8, 5, &fonts::DejaVu24, C_TEXT, textdatum_t::top_left);
  int x = 8 + textW(ap.icao, &fonts::DejaVu24) + 8;
  Cat c = wx.valid ? wx.cat : CAT_NONE;
  int pw = 52, ph = 22, px = 314 - pw, py = 6;
  if (wx.valid && wx.proxy) {                    // hollow pill = not this airport's own weather
    cv.drawRoundRect(px, py, pw, ph, 4, catColor(c));
    cv.drawRoundRect(px + 1, py + 1, pw - 2, ph - 2, 3, catColor(c));
    txt(catText(c), px + pw / 2, py + ph / 2 + 1, &fonts::FreeSansBold9pt7b, catColor(c) == C_MVFR ? C_CYAN : catColor(c), textdatum_t::middle_center);
  } else drawPill(px, py, pw, ph, c, &fonts::FreeSansBold9pt7b);
  // The clock gives up its space when we need it to say whose weather this is.
  int cx = px;
  if (!(wx.valid && wx.proxy)) {
    char clk[10] = "--:--Z";
    if (timeSynced()) zuluHHMM((long)time(nullptr), clk, sizeof clk, true);
    cx = px - 8 - textW(clk, &fonts::DejaVu12);
    txt(clk, cx, 12, &fonts::DejaVu12, C_SUB, textdatum_t::top_left);
  }
  char nm[32];
  if (wx.valid && wx.proxy) {
    char via[32]; snprintf(via, sizeof via, "WX: %s %dNM %s", wx.icao, (int)lroundf(wx.proxyNm), wx.proxyDir);
    fitText(via, nm, sizeof nm, cx - 8 - x, &fonts::DejaVu12);
    txt(nm, x, 12, &fonts::DejaVu12, C_AMBER, textdatum_t::top_left);
  } else if (wx.valid && wx.relay) {             // the field's own AWOS, but not via the FAA
    fitText("AWOS LIVE  UNOFFICIAL", nm, sizeof nm, cx - 8 - x, &fonts::DejaVu12);
    txt(nm, x, 12, &fonts::DejaVu12, C_CYAN, textdatum_t::top_left);
  } else {
    fitText(ap.name[0] ? ap.name : wx.name, nm, sizeof nm, cx - 8 - x, &fonts::DejaVu12);
    txt(nm, x, 12, &fonts::DejaVu12, C_LBL, textdatum_t::top_left);
  }
}

void drawWindTile() {
  const Box& b = B_WIND;
  tileBg(b, C_EDGE);
  int cx = b.x + b.w / 2, cy = 82, r = 28;

  // Pick the runway end with the most headwind (or the one the pilot tapped to).
  int bestR = -1, bestE = 0; float bestHw = -999;
  bool haveWind = wx.valid && !wx.vrb && wx.wspd > 0 && wx.wdir >= 0;
  for (int i = 0; i < ap.nRwy; i++) for (int e = 0; e < 2; e++) {
    int h = ap.rwy[i].hdg + (e ? 180 : 0);
    float hw = haveWind ? wx.wspd * cosf(normDeg(wx.wdir - h) * DEG_TO_RAD) : 0;
    if (hw > bestHw) { bestHw = hw; bestR = i; bestE = e; }
  }
  int bR = bestR, bE = bestE;
  if (windSel >= 0 && windSel < 2 * ap.nRwy) {
    bestR = windSel / 2; bestE = windSel % 2;
    int h = ap.rwy[bestR].hdg + (bestE ? 180 : 0);
    bestHw = haveWind ? wx.wspd * cosf(normDeg(wx.wdir - h) * DEG_TO_RAD) : 0;
  }
  bool isBest = bestR == bR && bestE == bE;

  char lab[28] = "WIND";
  if (bestR >= 0 && haveWind) {
    if (isBest) snprintf(lab, sizeof lab, "WIND  RWY %s", ap.rwy[bestR].ends[bestE]);
    else snprintf(lab, sizeof lab, "RWY %s  (TAP)", ap.rwy[bestR].ends[bestE]);
  }
  char lab2[28]; fitText(lab, lab2, sizeof lab2, b.w - 10, &fonts::DejaVu9);
  txt(lab2, b.x + 6, b.y + 4, &fonts::DejaVu9, isBest ? C_LBL : C_CYAN, textdatum_t::top_left);

  cv.drawCircle(cx, cy, r, C_EDGE2);
  txt("N", cx, cy - r + 3, &fonts::DejaVu9, C_DIM, textdatum_t::top_center);

  if (bestR >= 0) {
    const Runway& R = ap.rwy[bestR];
    float a = R.hdg * DEG_TO_RAD, half = r - 7;
    float dx = sinf(a) * half, dy = -cosf(a) * half;
    cv.drawWideLine(cx - dx, cy - dy, cx + dx, cy + dy, 3.5f, C_RWY);
    cv.drawLine(cx - dx * 0.8f, cy - dy * 0.8f, cx + dx * 0.8f, cy + dy * 0.8f, C_SUB);
    // Label only the end you'd land on, at its threshold (opposite its heading).
    if (haveWind) {
      float th = (R.hdg + (bestE ? 0 : 180)) * DEG_TO_RAD;
      int tx = cx + sinf(th) * (half - 3), ty = cy - cosf(th) * (half - 3);
      txt(R.ends[bestE], tx + (int)(cosf(th) * 9), ty + (int)(sinf(th) * 9), &fonts::DejaVu9, C_TEXT, textdatum_t::middle_center);
    }
  }
  if (haveWind) {
    float w = wx.wdir * DEG_TO_RAD;
    float tx = cx + sinf(w) * (r + 1), ty = cy - cosf(w) * (r + 1);
    float hx = cx + sinf(w) * 9,       hy = cy - cosf(w) * 9;
    cv.drawWideLine(tx, ty, hx + sinf(w) * 7, hy - cosf(w) * 7, 1.6f, C_CYAN);
    float px = cosf(w) * 5, py = sinf(w) * 5;
    float bx = hx + sinf(w) * 9, by = hy - cosf(w) * 9;
    cv.fillTriangle(hx, hy, bx + px, by + py, bx - px, by - py, C_CYAN);
  }

  char val[20], comp[28] = "";
  if (!wx.valid) strlcpy(val, "--", sizeof val);
  else if (wx.wspd == 0) strlcpy(val, "CALM", sizeof val);
  else if (wx.vrb) snprintf(val, sizeof val, wx.wgst ? "VRB/%02dG%02d" : "VRB/%02d", wx.wspd, wx.wgst);
  else snprintf(val, sizeof val, wx.wgst ? "%03d/%02dG%02d" : "%03d/%02d", wx.wdir, wx.wspd, wx.wgst);

  if (haveWind && bestR >= 0) {
    int h = ap.rwy[bestR].hdg + (bestE ? 180 : 0);
    float d = normDeg(wx.wdir - h) * DEG_TO_RAD;
    int hw = (int)lroundf(wx.wspd * cosf(d));
    int xw = (int)lroundf(fabsf(wx.wspd * sinf(d)));
    int xg = (int)lroundf(fabsf(wx.wgst * sinf(d)));
    char side = normDeg(wx.wdir - h) > 0 ? 'R' : 'L';
    if (xw == 0 && xg == 0) side = ' ';
    if (hw >= 0) {
      if (wx.wgst) snprintf(comp, sizeof comp, "HW%d XW%d-%d%c", hw, xw, xg, side);
      else snprintf(comp, sizeof comp, "HW %d  XW %d%c", hw, xw, side);
    } else snprintf(comp, sizeof comp, "TAILWIND %d", -hw);
  } else if (wx.valid && wx.vrb) strlcpy(comp, "Variable", sizeof comp);

  txt(val, cx, 114, pickFont(val, b.w - 8) == BIG_FONTS[0] ? &fonts::DejaVu18 : pickFont(val, b.w - 8), C_TEXT, textdatum_t::top_center);
  txt(comp, cx, 136, textW(comp, &fonts::DejaVu12) <= b.w - 6 ? &fonts::DejaVu12 : &fonts::DejaVu9,
      (bestHw < 0 && haveWind) ? C_AMBER : C_CYAN, textdatum_t::top_center);
}

void drawValueTile(const Box& b, const char* lab, const char* val, const char* sub, uint16_t edge, uint16_t subCol) {
  tileBg(b, edge);
  txt(lab, b.x + 6, b.y + 4, &fonts::DejaVu9, edge == C_EDGE ? C_LBL : edge, textdatum_t::top_left);
  txt(val, b.x + 6, b.y + 16, pickFont(val, b.w - 10), C_TEXT, textdatum_t::top_left);
  char s[32]; fitText(sub, s, sizeof s, b.w - 10, &fonts::DejaVu9);
  txt(s, b.x + 6, b.y + 43, &fonts::DejaVu9, subCol, textdatum_t::top_left);
}

// The next 12 hours of the TAF as colored hour blocks, labeled in Zulu.
// A lower band shows a worse category under TEMPO / PROB.
void drawForecast() {
  tileBg(B_RAW, C_EDGE);
  char lab[40];
  if (taf.proxy) snprintf(lab, sizeof lab, "TAF %s  %dNM %s", taf.icao, (int)lroundf(taf.distNm), taf.dir);
  else snprintf(lab, sizeof lab, "TAF %s", taf.icao);
  txt(lab, B_RAW.x + 6, B_RAW.y + 3, &fonts::DejaVu9, taf.proxy ? C_AMBER : C_LBL, textdatum_t::top_left);
  txt("tap: raw METAR", B_RAW.x + B_RAW.w - 6, B_RAW.y + 3, &fonts::DejaVu9, C_DIM, textdatum_t::top_right);
  const int n = TAF_HOURS, cw = (B_RAW.w - 10) / n, x0 = B_RAW.x + (B_RAW.w - n * cw) / 2, y = B_RAW.y + 15, h = 21;
  for (int k = 0; k < n; k++) {
    int x = x0 + k * cw;
    Cat b = (Cat)taf.base[k], t = (Cat)taf.temp[k];
    uint16_t col = b == CAT_NONE ? C_EDGE : catColor(b);
    cv.fillRect(x + 1, y, cw - 2, h, col);
    if (t != CAT_NONE) {
      cv.fillRect(x + 1, y + h - 6, cw - 2, 6, catColor(t));
      cv.drawFastHLine(x + 1, y + h - 7, cw - 2, C_TILE);
    }
    time_t ht = taf.start + k * 3600L; struct tm g; gmtime_r(&ht, &g);
    char hh[4]; snprintf(hh, sizeof hh, "%02d", g.tm_hour);
    txt(hh, x + cw / 2, y + (t != CAT_NONE ? 7 : 10), &fonts::DejaVu9, b == CAT_NONE ? C_DIM : catInk(b), textdatum_t::middle_center);
  }
  cv.fillRect(x0 + 1, y - 2, cw - 2, 2, C_TEXT);    // "now" marker on the first hour
}

void drawRaw() {
  tileBg(B_RAW, C_EDGE);
  // Word-wrap the raw METAR into up to 3 lines.
  const lgfx::IFont* f = &fonts::DejaVu9;
  int maxW = B_RAW.w - 12, y = B_RAW.y + 4, lines = 0;
  char line[90] = ""; char word[40];
  const char* p = wx.raw;
  while (*p && lines < 3) {
    int n = 0; while (*p == ' ') p++;
    while (*p && *p != ' ' && n < (int)sizeof word - 1) word[n++] = *p++;
    word[n] = 0;
    if (!n) break;
    char test[130]; snprintf(test, sizeof test, line[0] ? "%s %s" : "%s%s", line, word);
    if (textW(test, f) > maxW && line[0]) {
      txt(line, B_RAW.x + 6, y, f, C_SUB, textdatum_t::top_left);
      y += 11; lines++;
      strlcpy(line, word, sizeof line);
    } else strlcpy(line, test, sizeof line);
  }
  if (line[0] && lines < 3) txt(line, B_RAW.x + 6, y, f, C_SUB, textdatum_t::top_left);
}

void drawStatusRow() {
  int x = 8, y = 201;
  bool stale = false;
  char obs[40] = "";
  if (wx.valid) {
    char hhmm[8]; zuluHHMM(wx.obs, hhmm, sizeof hhmm, false);
    if (timeSynced()) {
      long age = ((long)time(nullptr) - wx.obs) / 60; if (age < 0) age = 0;
      stale = age > STALE_AFTER_MIN;
      snprintf(obs, sizeof obs, "OBS %s  %ldm ago", hhmm, age);
    } else snprintf(obs, sizeof obs, "OBS %s", hhmm);
  } else strlcpy(obs, everFetched ? (noMetar ? "No METAR at this field" : "No data") : "Waiting for weather", sizeof obs);

  // Right side first (link + battery), then fit the obs text into what's left.
  bool link = WiFi.status() == WL_CONNECTED && lastFetchOk;
  char right[32];
  int bat = M5.Power.getBatteryLevel();
  long nextMin = ((long)(nextFetchMs - millis()) + 59999) / 60000; if (nextMin < 0) nextMin = 0;
  if (link && (needWx || netBusy)) strlcpy(right, "updating...", sizeof right);
  else if (link) snprintf(right, sizeof right, "next %ldm", nextMin);
  else strlcpy(right, WiFi.status() == WL_CONNECTED ? "RETRYING" : "NO WI-FI", sizeof right);
  int bx = 312;
  if (bat >= 0) {
    char bs[8]; snprintf(bs, sizeof bs, "%d%%", bat);
    txt(bs, bx, y + 2, &fonts::DejaVu9, bat < 20 ? C_AMBER : C_LBL, textdatum_t::top_right);
    bx -= textW(bs, &fonts::DejaVu9) + 8;
  }
  cv.fillCircle(bx - 3, y + 6, 3, link ? C_VFR : C_AMBER);
  bx -= 10;
  txt(right, bx, y + 2, &fonts::DejaVu9, link ? C_LBL : C_AMBER, textdatum_t::top_right);
  bx -= textW(right, &fonts::DejaVu9) + 8;

  if (stale) {
    cv.fillRoundRect(x, y, 40, 14, 3, C_AMBER);
    txt("STALE", x + 20, y + 7, &fonts::DejaVu9, C_BG, textdatum_t::middle_center);
    x += 46;
  } else if (wx.valid && wx.speci) {               // a special: conditions just changed
    cv.fillRoundRect(x, y, 40, 14, 3, C_CYAN);
    txt("SPECI", x + 20, y + 7, &fonts::DejaVu9, C_BG, textdatum_t::middle_center);
    x += 46;
  }
  char o2[40]; fitText(obs, o2, sizeof o2, bx - x, &fonts::DejaVu12);
  txt(o2, x, y, &fonts::DejaVu12, stale ? C_AMBER : C_SUB, textdatum_t::top_left);
}

const Box B_ADDWIFI = {50, 136, 220, 44};

// Offline empty state: say so, and give a one-tap way to add a network.
void drawOfflineTile(const char* what) {
  tileBg({6, 34, 308, 162}, C_AMBER);
  bool joining = wState == W_JOINING || wState == W_SCANNING;
  char l1[48];
  if (joining && wState == W_JOINING) snprintf(l1, sizeof l1, "Joining %s...", wSsid);
  else if (joining) strlcpy(l1, "Looking for Wi-Fi...", sizeof l1);
  else strlcpy(l1, knownCount() == 0 ? "No Wi-Fi saved" : "No saved Wi-Fi in range", sizeof l1);
  char f1[48]; fitText(l1, f1, sizeof f1, 290, &fonts::DejaVu18);
  txt(f1, 160, 74, &fonts::DejaVu18, C_AMBER, textdatum_t::middle_center);
  txt(what, 160, 104, &fonts::DejaVu12, C_SUB, textdatum_t::middle_center);
  cv.fillRoundRect(B_ADDWIFI.x, B_ADDWIFI.y, B_ADDWIFI.w, B_ADDWIFI.h, 6, C_CYAN);
  txt("ADD WI-FI", B_ADDWIFI.x + B_ADDWIFI.w / 2, B_ADDWIFI.y + B_ADDWIFI.h / 2 + 1, &fonts::DejaVu18, C_BG, textdatum_t::middle_center);
}

void drawWx() {
  drawHeader();
  if (!wx.valid && WiFi.status() != WL_CONNECTED) {
    drawOfflineTile("Weather loads as soon as it connects");
  } else if (!wx.valid) {
    tileBg({6, 34, 308, 162}, C_EDGE);
    const char* l1 = noMetar ? "No METAR reported here" : (everFetched ? "No weather data" : "Getting weather...");
    txt(l1, 160, 100, &fonts::DejaVu18, C_TEXT, textdatum_t::middle_center);
    txt(noMetar ? "Frequencies are on FREQ" : "Hold WX to retry now", 160, 126, &fonts::DejaVu12, C_SUB, textdatum_t::middle_center);
  } else {
    drawWindTile();
    // Visibility
    char v[16]; snprintf(v, sizeof v, "%s SM", wx.vis);
    bool visLimits = wx.cat > CAT_VFR && wx.visCat == wx.cat;
    bool cigLimits = wx.cat > CAT_VFR && wx.cigCat == wx.cat;
    const char* vsub = wx.wxText[0] ? wx.wxText : (wx.visSm >= 10 ? "Unrestricted" : "");
    drawValueTile(B_T[0], "VISIBILITY", v, vsub, visLimits ? catColor(wx.cat) : C_EDGE, wx.wxHazard ? C_AMBER : C_SUB);
    // Sky
    char sky[16], skySub[32];
    const Layer& L0 = wx.layers[0];
    if (L0.base >= 0) snprintf(sky, sizeof sky, "%s %03d", L0.cover, L0.base / 100); else strlcpy(sky, L0.cover, sizeof sky);
    if (wx.ceiling >= 0) snprintf(skySub, sizeof skySub, "Ceiling %d ft", wx.ceiling);
    else if (wx.nLayers > 1 && wx.layers[1].base >= 0) snprintf(skySub, sizeof skySub, "%s %03d above", wx.layers[1].cover, wx.layers[1].base / 100);
    else strlcpy(skySub, "No ceiling", sizeof skySub);
    drawValueTile(B_T[1], "SKY", sky, skySub, cigLimits ? catColor(wx.cat) : C_EDGE, C_SUB);
    // Temp / dew
    char td[16], tdSub[28]; bool fog = false;
    if (isnan(wx.temp)) { strlcpy(td, "--", sizeof td); tdSub[0] = 0; }
    else {
      snprintf(td, sizeof td, "%d/%d", (int)lroundf(wx.temp), isnan(wx.dewp) ? 0 : (int)lroundf(wx.dewp));
      float sp = isnan(wx.dewp) ? 99 : wx.temp - wx.dewp;
      fog = sp <= 2.0f;
      if (fog) strlcpy(tdSub, "FOG RISK", sizeof tdSub);
      else snprintf(tdSub, sizeof tdSub, "Spread %d", (int)lroundf(sp));
    }
    drawValueTile(B_T[2], "TEMP / DEW  C", td, tdSub, C_EDGE, fog ? C_AMBER : C_SUB);
    // Altimeter
    char alt[12];
    if (isnan(wx.altim)) strlcpy(alt, "--", sizeof alt); else snprintf(alt, sizeof alt, "%.2f", wx.altim);
    // Density altitude at the destination's field elevation ("~" when using a nearby station).
    char daS[24] = "inHg"; uint16_t daCol = C_SUB;
    if (!isnan(wx.altim) && !isnan(wx.temp) && ap.elevFt >= 0) {
      float pa  = ap.elevFt + (29.92f - wx.altim) * 1000.0f;
      float isa = 15.0f - 1.98f * pa / 1000.0f;
      int   da  = (int)lroundf((pa + 118.8f * (wx.temp - isa)) / 10.0f) * 10;
      char n[12]; int v = da < 0 ? -da : da;
      if (v >= 1000) snprintf(n, sizeof n, "%s%d,%03d", da < 0 ? "-" : "", v / 1000, v % 1000);
      else snprintf(n, sizeof n, "%d", da);
      snprintf(daS, sizeof daS, "DA %s%s ft", wx.proxy ? "~" : "", n);
      if (da > ap.elevFt + 2000) daCol = C_AMBER;   // hot day: noticeably less performance
    }
    drawValueTile(B_T[3], "ALTIMETER", alt, daS, C_EDGE, daCol);
    if (showTaf && taf.valid) drawForecast(); else drawRaw();
  }
  drawStatusRow();
  drawKeys("WX", "FREQ", "APT", 0);
}

void drawFreq() {
  drawHeader();
  if (ap.nFq == 0 && WiFi.status() != WL_CONNECTED) {
    drawOfflineTile("Frequencies load as soon as it connects");
  } else if (ap.nFq == 0) {
    tileBg({6, 34, 308, 162}, C_EDGE);
    txt("No frequencies found", 160, 100, &fonts::DejaVu18, C_TEXT, textdatum_t::middle_center);
    txt("Hold FREQ to try the download again", 160, 126, &fonts::DejaVu12, C_SUB, textdatum_t::middle_center);
  } else {
    // Primary (tower, else CTAF, else UNICOM)
    const Freq& P = ap.fq[ap.primary >= 0 ? ap.primary : 0];
    tileBg(B_PRI, C_CYAN);
    char pl[26]; fitText(P.label, pl, sizeof pl, 120, &fonts::DejaVu12);
    txt(pl, B_PRI.x + 8, B_PRI.y + 9, &fonts::DejaVu12, C_CYAN, textdatum_t::top_left);
    txt("PRIMARY", B_PRI.x + 8, B_PRI.y + 29, &fonts::DejaVu9, C_LBL, textdatum_t::top_left);
    txt(P.mhz, B_PRI.x + B_PRI.w - 8, B_PRI.y + B_PRI.h / 2 + 2, &fonts::DejaVu40, C_CYAN, textdatum_t::middle_right);

    // Everything else, 5 rows per page
    tileBg(B_LIST, C_EDGE);
    int idx[14], n = 0;
    for (int i = 0; i < ap.nFq; i++) if (i != (ap.primary >= 0 ? ap.primary : 0)) idx[n++] = i;
    int per = n > 5 ? 4 : 5;                   // leave room for the page marker
    int pages = (n + per - 1) / per; if (pages < 1) pages = 1;
    if (freqPage >= pages) freqPage = 0;
    for (int r = 0; r < per; r++) {
      int k = freqPage * per + r; if (k >= n) break;
      const Freq& F = ap.fq[idx[k]];
      int y = B_LIST.y + 2 + r * 21;
      char lb[26]; fitText(F.label, lb, sizeof lb, 170, &fonts::DejaVu12);
      txt(lb, B_LIST.x + 8, y + 5, &fonts::DejaVu12, C_SUB, textdatum_t::top_left);
      txt(F.mhz, B_LIST.x + B_LIST.w - 8, y + 2, &fonts::DejaVu18, C_TEXT, textdatum_t::top_right);
      if (r < per - 1 && k + 1 < n) cv.drawFastHLine(B_LIST.x + 6, y + 21, B_LIST.w - 12, C_EDGE);
    }
    if (n == 0) txt("No other frequencies listed", 160, B_LIST.y + 50, &fonts::DejaVu12, C_LBL, textdatum_t::middle_center);
    if (pages > 1) {
      char pg[24]; snprintf(pg, sizeof pg, "MORE  %d/%d  tap list", freqPage + 1, pages);
      txt(pg, B_LIST.x + B_LIST.w - 8, B_LIST.y + B_LIST.h - 3, &fonts::DejaVu9, C_LBL, textdatum_t::bottom_right);
    }
  }
  // Footer: elevation and the next sunrise/sunset on the right, data source on the left.
  char right[40] = "";
  if (ap.elevFt >= 0) snprintf(right, sizeof right, "ELEV %d", ap.elevFt);
  if (timeSynced() && !isnan(ap.lat)) {
    bool set; long ev = nextSunEvent(time(nullptr), ap.lat, ap.lon, &set);
    if (ev) { char hm[8]; zuluHHMM(ev, hm, sizeof hm, false); char s[16]; snprintf(s, sizeof s, "  %s %s", set ? "SS" : "SR", hm); strlcat(right, s, sizeof right); }
  }
  txt(right, 312, 203, &fonts::DejaVu9, C_LBL, textdatum_t::top_right);
  char src[72]; snprintf(src, sizeof src, "%s - verify", ap.fqSrc);
  char s2[64]; fitText(src, s2, sizeof s2, 304 - textW(right, &fonts::DejaVu9) - 12, &fonts::DejaVu9);
  txt(s2, 8, 203, &fonts::DejaVu9, C_LBL, textdatum_t::top_left);
  drawKeys("WX", "FREQ", "APT", 1);
}

Box tileBox(int i) { return { 6 + (i % 3) * 104, 32 + (i / 3) * 72, 100, 68 }; }

void drawPicker() {
  txt(startupFlow ? "WHERE ARE YOU HEADED?" : "CHANGE AIRPORT", 8, 7, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  const char* current = airportLoaded ? ap.icao : lastDest;
  for (int i = 0; i < nTiles; i++) {
    Box b = tileBox(i);
    const Tile& t = tiles[i];
    tileBg(b, !strcmp(t.icao, current) ? C_CYAN : C_EDGE);
    txt(t.icao, b.x + 6, b.y + 6, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
    char nm[24]; fitText(t.name[0] ? t.name : "", nm, sizeof nm, b.w - 12, &fonts::DejaVu9);
    txt(nm, b.x + 6, b.y + 32, &fonts::DejaVu9, C_TEXT, textdatum_t::top_left);
    txt(t.tag, b.x + 6, b.y + 50, &fonts::DejaVu9, !strcmp(t.tag, "HOME") ? C_CYAN : C_LBL, textdatum_t::top_left);
    if (t.cat != CAT_NONE) drawPill(b.x + b.w - 40, b.y + b.h - 20, 34, 15, t.cat, &fonts::DejaVu9);
    else if (tilesFetched && !t.hasWx && WiFi.status() == WL_CONNECTED)
      txt("NO WX", b.x + b.w - 6, b.y + b.h - 13, &fonts::DejaVu9, C_DIM, textdatum_t::middle_right);
  }
  bool wifi = WiFi.status() == WL_CONNECTED;
  txt(startupFlow ? "Tap your destination" : "Tap to divert", 8, 186, &fonts::DejaVu12, C_SUB, textdatum_t::top_left);
  char wl[48];
  if (wifi) strlcpy(wl, "or type any ICAO", sizeof wl);
  else if (wState == W_NONE_SAVED) strlcpy(wl, "No Wi-Fi saved: tap WI-FI", sizeof wl);
  else if (wState == W_NONE_SEEN) strlcpy(wl, "No saved Wi-Fi in range", sizeof wl);
  else if (wState == W_JOINING) snprintf(wl, sizeof wl, "Joining %s...", wSsid);
  else strlcpy(wl, "Looking for Wi-Fi...", sizeof wl);
  char w2[48]; fitText(wl, w2, sizeof w2, B_TYPE.x - 14, &fonts::DejaVu9);
  txt(w2, 8, 203, &fonts::DejaVu9, wifi ? C_LBL : C_AMBER, textdatum_t::top_left);
  cv.drawRoundRect(B_TYPE.x, B_TYPE.y, B_TYPE.w, B_TYPE.h, 6, C_CYAN);
  cv.drawRoundRect(B_TYPE.x + 1, B_TYPE.y + 1, B_TYPE.w - 2, B_TYPE.h - 2, 5, C_CYAN);
  txt("TYPE ICAO >", B_TYPE.x + B_TYPE.w / 2, B_TYPE.y + B_TYPE.h / 2, &fonts::DejaVu12, C_CYAN, textdatum_t::middle_center);
  drawKeys(airportLoaded ? "BACK" : "", "WI-FI", "APT", 2);
}

Box entryCol(int i, int part) {   // part 0 = up, 1 = slot, 2 = down
  int x = 20 + i * 72;
  if (part == 0) return {x, 30, 66, 40};
  if (part == 1) return {x, 74, 66, 58};
  return {x, 136, 66, 40};
}

// Which column/arrow a touch lands on. Columns are hit across their full 72 px
// pitch (no dead gaps); arrows extend up to the title and down to the slot.
// Returns +1 (up arrow), -1 (down arrow) or 0, with the column in *col.
int entryArrowAt(int x, int y, int* col) {
  if (x < 10) return 0;
  int i = (x - 17) / 72;
  if (i < 0) i = 0;
  if (i > 3) i = 3;                              // the right edge belongs to column 4, all the way out
  *col = i;
  if (y >= 24 && y < 72) return 1;
  if (y >= 134 && y < 180) return -1;
  return 0;
}

void entryStep(int i, int step) {
  int len = strlen(ENTRY_CHARS);
  const char* p = strchr(ENTRY_CHARS, entryChars[i]);
  int k = p ? p - ENTRY_CHARS : 0;
  entryChars[i] = ENTRY_CHARS[(k + step + len) % len];
  entrySel = i; entryMsg[0] = 0; dirty = true;
}

int      entryHeldCol  = -1;     // arrow being held down, for repeat
int      entryHeldStep = 0;
uint32_t entryRepeatMs = 0;
int      entryRepeats  = 0;

// Keypad: 8 x 5 keys. 36 letters/digits, then blank, back, next, done.
const char PAD_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
const int  PAD_COLS = 8, PAD_ROWS = 5, PAD_X = 4, PAD_Y = 36, PAD_W = 39, PAD_H = 37;
int padKeyAt(int x, int y) {
  if (x < PAD_X || y < PAD_Y) return -1;
  int c = (x - PAD_X) / PAD_W, r = (y - PAD_Y) / PAD_H;
  if (c >= PAD_COLS) c = PAD_COLS - 1;
  if (r >= PAD_ROWS) return -1;
  return r * PAD_COLS + c;
}

void drawKeypad() {
  txt("TYPE ICAO", 8, 7, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  // Current ident in small boxes, the position being typed highlighted.
  for (int i = 0; i < 4; i++) {
    int bx = 192 + i * 30, by = 4;
    bool sel = i == entrySel;
    cv.fillRoundRect(bx, by, 26, 26, 4, C_TILE);
    cv.drawRoundRect(bx, by, 26, 26, 4, sel ? C_CYAN : C_EDGE2);
    char ch[2] = { entryChars[i] == ' ' ? '_' : entryChars[i], 0 };
    txt(ch, bx + 13, by + 14, &fonts::DejaVu18, sel ? C_CYAN : C_TEXT, textdatum_t::middle_center);
  }
  const char* extra[4] = {"_", "<", ">", "OK"};
  for (int k = 0; k < PAD_COLS * PAD_ROWS; k++) {
    int c = k % PAD_COLS, r = k / PAD_COLS;
    int x = PAD_X + c * PAD_W, y = PAD_Y + r * PAD_H;
    bool special = k >= 36;
    uint16_t bg = k == 39 ? C_CYAN : C_TILE;
    cv.fillRoundRect(x + 1, y + 1, PAD_W - 3, PAD_H - 3, 5, bg);
    if (!special || k != 39) cv.drawRoundRect(x + 1, y + 1, PAD_W - 3, PAD_H - 3, 5, C_EDGE2);
    char lbl[3] = { (char)(k < 36 ? PAD_KEYS[k] : 0), 0, 0 };
    const char* s = special ? extra[k - 36] : lbl;
    txt(s, x + PAD_W / 2 - 1, y + PAD_H / 2, &fonts::DejaVu18, k == 39 ? C_BG : (special ? C_CYAN : C_TEXT), textdatum_t::middle_center);
  }
  drawKeys("CANCEL", "", "APT", 2);
}

void padPress(int k) {
  if (k < 0) return;
  if (k < 36 || k == 36) {                       // a character (or blank)
    entryChars[entrySel] = k < 36 ? PAD_KEYS[k] : ' ';
    entryMsg[0] = 0;
    if (entrySel < 3) entrySel++; else entryPad = false;   // fourth letter typed: back to LOAD
  } else if (k == 37) { if (entrySel > 0) entrySel--; }
  else if (k == 38) { if (entrySel < 3) entrySel++; }
  else entryPad = false;
  dirty = true;
}

void entryIdent(char* out) {
  int n = 0;
  for (int i = 0; i < 4; i++) if (entryChars[i] != ' ') out[n++] = entryChars[i];
  out[n] = 0;
}

void drawEntry() {
  if (entryPad) { drawKeypad(); return; }
  txt("ENTER ICAO", 8, 7, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  txt("tap a letter to type", 312, 12, &fonts::DejaVu9, C_LBL, textdatum_t::top_right);
  for (int i = 0; i < 4; i++) {
    Box u = entryCol(i, 0), s = entryCol(i, 1), d = entryCol(i, 2);
    cv.fillRoundRect(u.x, u.y, u.w, u.h, 5, C_TILE); cv.drawRoundRect(u.x, u.y, u.w, u.h, 5, C_EDGE2);
    cv.fillTriangle(u.x + u.w / 2, u.y + 9, u.x + u.w / 2 - 10, u.y + 23, u.x + u.w / 2 + 10, u.y + 23, C_SUB);
    cv.fillRoundRect(d.x, d.y, d.w, d.h, 5, C_TILE); cv.drawRoundRect(d.x, d.y, d.w, d.h, 5, C_EDGE2);
    cv.fillTriangle(d.x + d.w / 2, d.y + 23, d.x + d.w / 2 - 10, d.y + 9, d.x + d.w / 2 + 10, d.y + 9, C_SUB);
    bool sel = i == entrySel;
    cv.fillRoundRect(s.x, s.y, s.w, s.h, 6, C_TILE);
    cv.drawRoundRect(s.x, s.y, s.w, s.h, 6, sel ? C_CYAN : C_EDGE2);
    if (sel) cv.drawRoundRect(s.x + 1, s.y + 1, s.w - 2, s.h - 2, 5, C_CYAN);
    char ch[2] = { entryChars[i] == ' ' ? '.' : entryChars[i], 0 };
    txt(ch, s.x + s.w / 2, s.y + s.h / 2 + 2, &fonts::DejaVu40, sel ? C_CYAN : C_TEXT, textdatum_t::middle_center);
  }
  char id[6]; entryIdent(id);
  char msg[40] = "";
  if (entryMsg[0]) strlcpy(msg, entryMsg, sizeof msg);
  else {
    for (int i = 0; i < nTiles; i++) if (!strcmp(tiles[i].icao, id) && tiles[i].name[0]) strlcpy(msg, tiles[i].name, sizeof msg);
    if (!msg[0]) strlcpy(msg, strlen(id) >= 3 ? "Checked on LOAD" : "Enter 3-4 characters", sizeof msg);
  }
  char m2[40]; fitText(msg, m2, sizeof m2, 168, &fonts::DejaVu12);
  txt(m2, 8, 190, &fonts::DejaVu12, entryMsg[0] ? C_AMBER : C_SUB, textdatum_t::top_left);
  bool ok = strlen(id) >= 3;
  cv.fillRoundRect(B_LOAD.x, B_LOAD.y, B_LOAD.w, B_LOAD.h, 6, ok ? C_CYAN : C_EDGE2);
  char lb[16]; snprintf(lb, sizeof lb, "LOAD %s", id);
  txt(lb, B_LOAD.x + B_LOAD.w / 2, B_LOAD.y + B_LOAD.h / 2 + 1, textW(lb, &fonts::DejaVu18) < B_LOAD.w - 10 ? &fonts::DejaVu18 : &fonts::DejaVu12,
      ok ? C_BG : C_LBL, textdatum_t::middle_center);
  drawKeys("CANCEL", "", "APT", 2);
}

Box cadBox(int i) { return { 6 + i * 106, 50, 96, 58 }; }
Box briBox(int i) { return { 6 + i * 106, 136, 96, 40 }; }

// Brightness: AUTO follows the sun at the airport you're looking at.
int autoBri() {
  if (!timeSynced()) return 170;
  float lat = !isnan(ap.lat) ? ap.lat : 40.21f, lon = !isnan(ap.lon) ? ap.lon : -79.83f;   // default: KFWQ
  float el = sunElevDeg(time(nullptr), lat, lon);
  if (el > 2.0f) return 170;                          // day
  if (el > -6.0f) return 80;                          // civil twilight
  return 28;                                          // night: keep your night vision
}

void applyBri() {
  int b = briMode == 1 ? 220 : briMode == 2 ? 28 : autoBri();
  if (b != curBri) { curBri = b; M5.Display.setBrightness(b); }
}

void drawCadence() {
  txt(airportLoaded ? "SETTINGS" : "HOW OFTEN TO REFRESH?", 8, 7, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  txt("WEATHER REFRESH", 8, 36, &fonts::DejaVu9, C_LBL, textdatum_t::top_left);
  for (int i = 0; i < 3; i++) {
    Box b = cadBox(i);
    bool on = CADENCES[i] == cadence;
    cv.fillRoundRect(b.x, b.y, b.w, b.h, 8, C_TILE);
    cv.drawRoundRect(b.x, b.y, b.w, b.h, 8, on ? C_CYAN : C_EDGE2);
    if (on) cv.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 7, C_CYAN);
    char n[4]; snprintf(n, sizeof n, "%d", CADENCES[i]);
    txt(n, b.x + b.w / 2 - 12, b.y + b.h / 2 + 1, &fonts::DejaVu24, on ? C_CYAN : C_TEXT, textdatum_t::middle_center);
    txt("MIN", b.x + b.w / 2 + 22, b.y + b.h / 2 + 4, &fonts::DejaVu9, C_LBL, textdatum_t::middle_center);
  }
  txt("SCREEN", 8, 122, &fonts::DejaVu9, C_LBL, textdatum_t::top_left);
  const char* bl[3] = {"AUTO", "DAY", "NIGHT"};
  for (int i = 0; i < 3; i++) {
    Box b = briBox(i);
    bool on = i == briMode;
    cv.fillRoundRect(b.x, b.y, b.w, b.h, 8, C_TILE);
    cv.drawRoundRect(b.x, b.y, b.w, b.h, 8, on ? C_CYAN : C_EDGE2);
    if (on) cv.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 7, C_CYAN);
    txt(bl[i], b.x + b.w / 2, b.y + b.h / 2 + 1, &fonts::DejaVu12, on ? C_CYAN : C_TEXT, textdatum_t::middle_center);
  }
  txt(briMode == 0 ? "Auto dims at dusk using the sun at your airport." : "METARs post hourly; specials any time.",
      8, 186, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);
  char v[32]; snprintf(v, sizeof v, "%s  v%s", TAIL, VERSION);
  txt(v, 312, 203, &fonts::DejaVu9, C_DIM, textdatum_t::top_right);
  if (pendingIcao[0] && !airportLoaded) { char d[24]; snprintf(d, sizeof d, "Destination %s", pendingIcao); txt(d, 8, 203, &fonts::DejaVu9, C_LBL, textdatum_t::top_left); }
  drawKeys(airportLoaded ? "BACK" : "", "", "", -1);
}

void drawLoading() {
  txt(ap.icao, 160, 50, &fonts::DejaVu40, C_TEXT, textdatum_t::middle_center);
  for (int i = 0; i < 3; i++) if (loadLines[i][0]) txt(loadLines[i], 60, 102 + i * 22, &fonts::DejaVu12, C_SUB, textdatum_t::top_left);
  if (loadPct >= 0 && loadPct < 100) {
    cv.drawRoundRect(60, 178, 200, 8, 4, C_EDGE2);
    cv.fillRoundRect(60, 178, 2 + loadPct * 198 / 100, 8, 4, C_CYAN);
  }
}

// ============================ Wi-Fi ============================
// Saved networks live in flash ("wn" = count, "ws<i>"/"wp<i>" = name/password).
void loadNets() {
  nNets = 0;
  int n = prefs.getUChar("wn", 0);
  for (int i = 0; i < n && nNets < MAX_NETS; i++) {
    char k[6];
    snprintf(k, sizeof k, "ws%d", i); String s = prefs.getString(k, "");
    snprintf(k, sizeof k, "wp%d", i); String p = prefs.getString(k, "");
    if (!s.length()) continue;
    strlcpy(nets[nNets].ssid, s.c_str(), sizeof nets[nNets].ssid);
    strlcpy(nets[nNets].pass, p.c_str(), sizeof nets[nNets].pass);
    nNets++;
  }
}

void saveNets() {
  prefs.putUChar("wn", nNets);
  for (int i = 0; i < nNets; i++) {
    char k[6];
    snprintf(k, sizeof k, "ws%d", i); prefs.putString(k, nets[i].ssid);
    snprintf(k, sizeof k, "wp%d", i); prefs.putString(k, nets[i].pass);
  }
}

bool hasBuiltin() { return WIFI_SSID[0] != 0; }
int  knownCount() { return nNets + (hasBuiltin() ? 1 : 0); }

// Password for a known network, or nullptr if we don't know it.
const char* passFor(const char* ssid) {
  for (int i = 0; i < nNets; i++) if (!strcmp(nets[i].ssid, ssid)) return nets[i].pass;
  if (hasBuiltin() && !strcmp(ssid, WIFI_SSID)) return WIFI_PASS;
  return nullptr;
}

void wifiStartScan() {
  WiFi.disconnect();                   // a join in progress blocks scanning
  WiFi.scanDelete();
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) { wState = W_IDLE; wTimer = millis() + 5000; return; }
  wState = W_SCANNING; wTimer = millis();
}

// Runs every loop: scan, join the strongest saved network, re-scan when it drops.
void wifiTick() {
  if (wState == W_SETUP) return;
  uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (wState != W_UP) {
      wState = W_UP; wFailCount = 0; wFailed[0] = 0; dirty = true;
      if (airportLoaded && !lastFetchOk) nextFetchMs = now;   // fetch right away, don't wait out the retry
    }
    return;
  }
  switch (wState) {
    case W_UP:                           // dropped: give auto-reconnect 20 s first
      wState = W_JOINING; wFromDrop = true; wTimer = now; dirty = true;
      break;

    case W_IDLE:
    case W_NONE_SEEN:
    case W_NONE_SAVED:
      if (knownCount() == 0) { if (wState != W_NONE_SAVED) { wState = W_NONE_SAVED; dirty = true; } break; }
      if (wState == W_NONE_SAVED || (int32_t)(now - wTimer) >= 0) wifiStartScan();
      break;

    case W_SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (now - wTimer > 15000) { WiFi.scanDelete(); wState = W_IDLE; wTimer = now + 2000; }
        break;
      }
      if (n < 0) { wState = W_IDLE; wTimer = now + 5000; break; }
      int best = -1, bestRssi = -1000;
      for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (!s.length() || !passFor(s.c_str())) continue;
        int r = WiFi.RSSI(i);
        if (wFailCount >= 2 && !strcmp(s.c_str(), wFailed)) r -= 40;   // try the others first
        if (r > bestRssi) { bestRssi = r; best = i; }
      }
      if (best < 0) {
        WiFi.scanDelete();
        if (wState != W_NONE_SEEN) dirty = true;
        wState = W_NONE_SEEN; wTimer = now + 15000;
        break;
      }
      strlcpy(wSsid, WiFi.SSID(best).c_str(), sizeof wSsid);
      WiFi.scanDelete();
      WiFi.begin(wSsid, passFor(wSsid));
      wState = W_JOINING; wFromDrop = false; wTimer = now; dirty = true;
      break;
    }

    case W_JOINING:
      if (now - wTimer > 20000) {
        if (!wFromDrop) {                // a fresh attempt that didn't work: likely the password
          if (!strcmp(wFailed, wSsid)) wFailCount++;
          else { strlcpy(wFailed, wSsid, sizeof wFailed); wFailCount = 1; }
        }
        wFromDrop = false;
        wifiStartScan(); dirty = true;
      }
      break;

    default: break;
  }
}

const char* signalWord(int rssi) { return rssi >= -60 ? "Strong" : rssi >= -72 ? "Good" : rssi >= -82 ? "Weak" : "Very weak"; }

// ---------------------------- phone setup ----------------------------
struct ScanItem { char ssid[33]; int rssi; bool open; };
ScanItem   scanList[16];
int        nScan        = 0;
WebServer* web          = nullptr;
DNSServer* dns          = nullptr;
char       apPass[9]    = "";
char       setupMsg[48] = "";
uint32_t   setupActiveMs = 0;
int        lastStations = 0;

void doSyncScan() {
  int n = WiFi.scanNetworks(false, false);
  nScan = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    int r = WiFi.RSSI(i), dup = -1;
    for (int j = 0; j < nScan; j++) if (!strcmp(scanList[j].ssid, s.c_str())) dup = j;
    if (dup >= 0) { if (r > scanList[dup].rssi) scanList[dup].rssi = r; continue; }
    if (nScan >= 16) continue;
    strlcpy(scanList[nScan].ssid, s.c_str(), sizeof scanList[nScan].ssid);
    scanList[nScan].rssi = r;
    scanList[nScan].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    nScan++;
  }
  for (int i = 1; i < nScan; i++) {             // strongest first
    ScanItem k = scanList[i]; int j = i - 1;
    while (j >= 0 && scanList[j].rssi < k.rssi) { scanList[j + 1] = scanList[j]; j--; }
    scanList[j + 1] = k;
  }
  WiFi.scanDelete();
}

String htmlEsc(const char* s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      case '\'': o += "&#39;"; break;
      default: o += *s;
    }
  }
  return o;
}

const char PAGE_HEAD[] =
  "<!doctype html><html lang=en><head><meta charset=utf-8>"
  "<meta name=viewport content='width=device-width,initial-scale=1'><title>METAR Panel Wi-Fi</title><style>"
  ":root{color-scheme:dark}body{margin:0;background:#05070A;color:#F2F5F7;font:16px -apple-system,system-ui,sans-serif}"
  "main{max-width:480px;margin:0 auto;padding:22px 16px 40px}"
  "h1{font-size:21px;margin:0 0 4px;letter-spacing:.04em}"
  ".sub{color:#A7B1BC;margin:0;font-size:14px;line-height:1.45}"
  "h2{font-size:12px;letter-spacing:.14em;color:#8D99A6;margin:26px 0 8px;text-transform:uppercase}"
  ".card{background:#10141A;border:1px solid #232A32;border-radius:12px;overflow:hidden}"
  ".row,.net,.field{display:flex;align-items:center;gap:12px;padding:12px 14px;border-top:1px solid #1E252C}"
  ".card>:first-child{border-top:0}"
  ".name{overflow-wrap:anywhere;flex:1;min-width:0}"
  ".tag{font-size:12px;color:#8D99A6;margin-top:2px}"
  ".on{color:#2FBF71}"
  "input[type=radio]{width:22px;height:22px;accent-color:#4FD1E8;margin:0;flex:none}"
  ".sig{color:#8D99A6;font-size:13px;flex:none}"
  ".field{display:block}.field span{font-size:13px;color:#A7B1BC}"
  "input[type=text],input[type=password]{display:block;width:100%;box-sizing:border-box;font-size:17px;padding:12px;"
  "border-radius:10px;border:1px solid #2C343D;background:#05070A;color:#F2F5F7;margin-top:6px}"
  "button{font:600 16px -apple-system,system-ui,sans-serif;border-radius:10px;border:0;padding:14px 16px;cursor:pointer}"
  ".primary{width:100%;background:#4FD1E8;color:#04161A;margin-top:14px}"
  ".ghost{background:none;border:1px solid #2C343D;color:#C9D1D9;padding:9px 14px;font-size:14px;min-height:40px}"
  ".note{padding:12px 14px;border-radius:10px;margin:18px 0 0;line-height:1.4}"
  ".ok{background:#0E2A1B;border:1px solid #2FBF71}.err{background:#2A1012;border:1px solid #E5484D}"
  "a{color:#4FD1E8}form{margin:0}"
  "</style></head><body><main>"
  "<h1>METAR PANEL &middot; Wi-Fi</h1>"
  "<p class=sub>The panel joins the strongest saved network it can see. Add your Starlink and any others here.</p>";

void sendPage(const char* notice, bool ok) {
  setupActiveMs = millis();
  String h = PAGE_HEAD;
  if (notice && *notice) { h += "<p class='note "; h += ok ? "ok" : "err"; h += "'>"; h += htmlEsc(notice); h += "</p>"; }

  h += "<h2>Saved networks ("; h += knownCount(); h += " of "; h += MAX_NETS; h += ")</h2><div class=card>";
  if (knownCount() == 0) h += "<div class=row><span class=sub>None yet.</span></div>";
  bool up = WiFi.status() == WL_CONNECTED;
  for (int i = 0; i < nNets; i++) {
    bool cur = up && !strcmp(nets[i].ssid, wSsid);
    h += "<div class=row><div class=name>"; h += htmlEsc(nets[i].ssid);
    h += "<div class='tag"; h += cur ? " on'>Connected" : "'>"; if (!cur) h += nets[i].pass[0] ? "Saved" : "Saved, no password";
    h += "</div></div><form method=post action=/forget><input type=hidden name=i value="; h += i;
    h += "><button class=ghost>Forget</button></form></div>";
  }
  if (hasBuiltin()) {
    h += "<div class=row><div class=name>"; h += htmlEsc(WIFI_SSID);
    h += "<div class=tag>Built into the firmware</div></div></div>";
  }
  h += "</div>";

  h += "<h2>Add or update a network</h2><form method=post action=/save><div class=card>";
  for (int i = 0; i < nScan; i++) {
    String e = htmlEsc(scanList[i].ssid);
    bool saved = passFor(scanList[i].ssid) != nullptr;
    h += "<label class=net><input type=radio name=pick value=\""; h += e; h += "\"";
    if (i == 0) h += " checked";
    h += "><span class=name>"; h += e;
    if (saved || scanList[i].open) { h += "<div class=tag>"; h += saved ? "Saved" : ""; h += (saved && scanList[i].open) ? ", " : ""; h += scanList[i].open ? "Open" : ""; h += "</div>"; }
    h += "</span><span class=sig>"; h += signalWord(scanList[i].rssi); h += "</span></label>";
  }
  h += "<label class=net><input type=radio name=pick value=''";
  if (nScan == 0) h += " checked";
  h += "><span class=name>Other (type the name below)</span></label>";
  h += "<div class=field><span>Or type the network name</span>"
       "<input type=text name=ssid maxlength=32 autocapitalize=none autocorrect=off spellcheck=false></div>";
  h += "<div class=field><span>Password (leave blank if the network is open)</span>"
       "<input type=password name=pass maxlength=64 autocomplete=off></div>";
  h += "</div><button class=primary>Save network</button></form>";
  h += "<p class=sub style='margin-top:18px'>Don't see it? <a href=/scan>Scan again</a>. "
       "When you're done, tap <b>DONE</b> on the panel.</p>";
  h += "<p class=sub style='margin-top:10px'>Networks with a &ldquo;click to accept&rdquo; login page (many hotels) won't work. Use your phone's hotspot there.</p>";
  h += "</main></body></html>";
  web->send(200, "text/html; charset=utf-8", h);
}

void handleRoot() { sendPage("", true); }

void handleScan() {
  // Scanning needs the station side on for a moment; the phone may blip, then the page loads.
  WiFi.mode(WIFI_AP_STA);
  doSyncScan();
  WiFi.mode(WIFI_AP);
  sendPage("", true);
}

void handleRedirect() {
  setupActiveMs = millis();
  web->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  web->send(302, "text/plain", "");
}

void handleSave() {
  String pick = web->arg("pick");
  String typed = web->arg("ssid");
  String pass = web->arg("pass");
  char ssid[40];
  strlcpy(ssid, typed.c_str(), sizeof ssid);
  trim(ssid);                                   // a typed name wins over the radio pick
  if (!ssid[0]) strlcpy(ssid, pick.c_str(), sizeof ssid);
  char msg[96];
  if (!ssid[0] || strlen(ssid) > 32) { sendPage("Pick a network or type its name (up to 32 characters).", false); return; }
  if (pass.length() && (pass.length() < 8 || pass.length() > 64)) { sendPage("Wi-Fi passwords are 8 to 63 characters. Leave it blank only for open networks.", false); return; }
  int idx = -1;
  for (int i = 0; i < nNets; i++) if (!strcmp(nets[i].ssid, ssid)) idx = i;
  if (idx < 0) {
    if (nNets >= MAX_NETS) { sendPage("8 networks are saved already. Forget one first.", false); return; }
    idx = nNets++;
    strlcpy(nets[idx].ssid, ssid, sizeof nets[idx].ssid);
  }
  strlcpy(nets[idx].pass, pass.c_str(), sizeof nets[idx].pass);
  saveNets();
  if (!strcmp(wFailed, ssid)) { wFailed[0] = 0; wFailCount = 0; }
  snprintf(setupMsg, sizeof setupMsg, "Saved: %s", ssid);
  dirty = true;
  snprintf(msg, sizeof msg, "Saved %s. Add another, or tap DONE on the panel to connect.", ssid);
  sendPage(msg, true);
}

void handleForget() {
  int i = web->arg("i").toInt();
  if (i < 0 || i >= nNets) { sendPage("", true); return; }
  char msg[64]; snprintf(msg, sizeof msg, "Forgot %s.", nets[i].ssid);
  snprintf(setupMsg, sizeof setupMsg, "Forgot: %s", nets[i].ssid);
  for (int j = i; j < nNets - 1; j++) nets[j] = nets[j + 1];
  nNets--;
  saveNets();
  dirty = true;
  sendPage(msg, true);
}

Screen wifiBack = SCR_PICK;

Screen setupReturn = SCR_WIFI;

void openWifi() { if (screen != SCR_WIFI) wifiBack = screen; screen = SCR_WIFI; dirty = true; }

void startSetup(Screen ret) {
  setupReturn = ret;
  screen = SCR_SETUP; setupMsg[0] = 0; lastStations = 0;
  wState = W_SETUP;                                 // background task stops starting jobs
  fgNetBegin();                                     // ...and one in flight finishes or bails
  fgNetEnd();
  cv.fillScreen(C_BG);
  txt("Scanning for networks...", 160, 120, &fonts::DejaVu12, C_SUB, textdatum_t::middle_center);
  pushChanged();
  WiFi.setAutoReconnect(false);
  if (WiFi.status() != WL_CONNECTED) WiFi.disconnect();   // stop join attempts hopping channels
  doSyncScan();
  // One password per panel, kept forever: a phone that saved METAR-PANEL once
  // keeps working (a new password each time made phones fail on their saved one).
  String pw = prefs.getString("appw", "");
  if (pw.length() != 8) {
    snprintf(apPass, sizeof apPass, "%08lu", (unsigned long)(esp_random() % 100000000UL));
    prefs.putString("appw", apPass);
  } else strlcpy(apPass, pw.c_str(), sizeof apPass);
  // Hotspot only, fixed channel: with the station side idle the hotspot never
  // hops channels chasing another network, which is what makes phones drop or refuse it.
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_NAME, apPass, 6);
  dns = new DNSServer();
  dns->start(53, "*", WiFi.softAPIP());                    // every name -> the panel, so the page pops up
  web = new WebServer(80);
  web->on("/", HTTP_GET, handleRoot);
  web->on("/scan", HTTP_GET, handleScan);
  web->on("/save", HTTP_POST, handleSave);
  web->on("/forget", HTTP_POST, handleForget);
  web->onNotFound(handleRedirect);
  web->begin();
  setupActiveMs = millis();
  dirty = true;
}

void exitSetup() {
  if (web) { web->stop(); delete web; web = nullptr; }
  if (dns) { dns->stop(); delete dns; dns = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // Forgot the network we're on? Drop it so we move to another.
  if (WiFi.status() == WL_CONNECTED && !passFor(wSsid)) WiFi.disconnect();
  wState = WiFi.status() == WL_CONNECTED ? W_UP : W_IDLE;
  wTimer = millis(); wFailCount = 0; wFailed[0] = 0;
  screen = setupReturn; dirty = true;
}

void setupTick() {
  if (!web) return;
  dns->processNextRequest();
  web->handleClient();
  int st = WiFi.softAPgetStationNum();
  if (st != lastStations) { lastStations = st; setupActiveMs = millis(); dirty = true; }
  if (st == 0 && millis() - setupActiveMs > 600000UL) exitSetup();      // 10 min with nobody there
}

const Box B_ADD  = {6, 186, 308, 36};
const Box B_DONE = {200, 180, 114, 40};

void drawWifi() {
  txt("WI-FI", 8, 7, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  bool up = WiFi.status() == WL_CONNECTED;
  uint16_t col = up ? C_VFR : C_AMBER;
  char l1[48], l2[64] = "";
  switch (up ? W_UP : wState) {
    case W_UP:         snprintf(l1, sizeof l1, "Connected: %s", wSsid);
                       snprintf(l2, sizeof l2, "Signal %s (%d dBm)", signalWord(WiFi.RSSI()), (int)WiFi.RSSI()); break;
    case W_JOINING:    snprintf(l1, sizeof l1, "Joining %s...", wSsid); break;
    case W_SCANNING:   strlcpy(l1, "Looking for saved networks...", sizeof l1); break;
    case W_NONE_SAVED: strlcpy(l1, "No networks saved", sizeof l1);
                       strlcpy(l2, "Add your Starlink from your phone", sizeof l2); break;
    case W_NONE_SEEN:  strlcpy(l1, "No saved network in range", sizeof l1);
                       strlcpy(l2, "Checking again every 15 seconds", sizeof l2); break;
    default:           strlcpy(l1, "Starting Wi-Fi...", sizeof l1); break;
  }
  if (!up && wFailCount > 0 && wFailed[0]) snprintf(l2, sizeof l2, "Couldn't join %s. Wrong password?", wFailed);
  Box s = {6, 34, 308, 50};
  tileBg(s, up ? C_EDGE : C_AMBER);
  cv.fillCircle(s.x + 14, s.y + 17, 4, col);
  char f1[48]; fitText(l1, f1, sizeof f1, s.w - 36, &fonts::DejaVu12);
  txt(f1, s.x + 26, s.y + 10, &fonts::DejaVu12, up ? C_TEXT : C_AMBER, textdatum_t::top_left);
  char f2[64]; fitText(l2, f2, sizeof f2, s.w - 36, &fonts::DejaVu9);
  txt(f2, s.x + 26, s.y + 30, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);

  char hdr[32]; snprintf(hdr, sizeof hdr, "SAVED NETWORKS  %d/%d", knownCount(), MAX_NETS);
  txt(hdr, 8, 92, &fonts::DejaVu9, C_LBL, textdatum_t::top_left);
  // SD frequency database status, top right of the list.
  char db[32]; uint16_t dbCol = C_LBL;
  if (!sdOk) { strlcpy(db, "NO SD CARD", sizeof db); dbCol = C_DIM; }
  else if (dbPct >= 0) { snprintf(db, sizeof db, "FREQ DB  DOWNLOADING %d%%", (int)dbPct); dbCol = C_CYAN; }
  else if (!dbStamp) { strlcpy(db, "FREQ DB  WAITING FOR WI-FI", sizeof db); dbCol = C_AMBER; }
  else if (dbStamp > 1) { time_t t = dbStamp; struct tm g; gmtime_r(&t, &g); snprintf(db, sizeof db, "FREQ DB  %d/%d", g.tm_mon + 1, g.tm_mday); }
  else strlcpy(db, "FREQ DB  READY", sizeof db);
  txt(db, 312, 92, &fonts::DejaVu9, dbCol, textdatum_t::top_right);
  if (knownCount() == 0) {
    txt("None yet. Save the plane's Starlink and", 8, 110, &fonts::DejaVu12, C_SUB, textdatum_t::top_left);
    txt("your home network from your phone.", 8, 128, &fonts::DejaVu12, C_SUB, textdatum_t::top_left);
  }
  int k = 0;
  auto row = [&](const char* name) {
    if (k >= 8) return;
    int x = 8 + (k % 2) * 156, y = 108 + (k / 2) * 19;
    bool cur = up && !strcmp(name, wSsid);
    if (cur) cv.fillCircle(x + 3, y + 7, 3, C_VFR);
    char nm[36]; fitText(name, nm, sizeof nm, 138, &fonts::DejaVu12);
    txt(nm, x + (cur ? 11 : 0), y, &fonts::DejaVu12, cur ? C_TEXT : C_SUB, textdatum_t::top_left);
    k++;
  };
  for (int i = 0; i < nNets; i++) row(nets[i].ssid);
  if (hasBuiltin()) row(WIFI_SSID);

  cv.drawRoundRect(B_ADD.x, B_ADD.y, B_ADD.w, B_ADD.h, 6, C_CYAN);
  cv.drawRoundRect(B_ADD.x + 1, B_ADD.y + 1, B_ADD.w - 2, B_ADD.h - 2, 5, C_CYAN);
  txt("ADD / FORGET FROM PHONE  >", B_ADD.x + B_ADD.w / 2, B_ADD.y + B_ADD.h / 2 + 1, &fonts::DejaVu12, C_CYAN, textdatum_t::middle_center);
  drawKeys("BACK", "", "APT", -1);
}

void drawSetup() {
  txt("PHONE SETUP", 8, 7, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  bool phone = lastStations > 0;
  char qr[80];
  if (phone) snprintf(qr, sizeof qr, "http://%s/", WiFi.softAPIP().toString().c_str());
  else snprintf(qr, sizeof qr, "WIFI:T:WPA;S:%s;P:%s;;", AP_NAME, apPass);
  cv.qrcode(qr, 6, 32, 140, 1);

  int x = 156;
  // Step 1
  if (phone) {
    cv.fillCircle(x + 8, 42, 8, C_VFR);
    txt("OK", x + 8, 43, &fonts::DejaVu9, C_BG, textdatum_t::middle_center);
    txt("Phone connected", x + 22, 36, &fonts::DejaVu12, C_VFR, textdatum_t::top_left);
  } else {
    cv.drawCircle(x + 8, 42, 8, C_CYAN);
    txt("1", x + 8, 43, &fonts::DejaVu9, C_CYAN, textdatum_t::middle_center);
    txt("Join the panel", x + 22, 36, &fonts::DejaVu12, C_TEXT, textdatum_t::top_left);
    txt("Scan the code with your", x, 56, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);
    txt("camera, or join", x, 68, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);
    txt(AP_NAME " in Wi-Fi.", x, 80, &fonts::DejaVu9, C_TEXT, textdatum_t::top_left);
    txt("Password:", x, 94, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);
    txt(apPass, x + 58, 90, &fonts::DejaVu18, C_TEXT, textdatum_t::top_left);
  }
  // Step 2
  int y2 = phone ? 64 : 122;
  cv.drawCircle(x + 8, y2 + 6, 8, phone ? C_CYAN : C_DIM);
  txt("2", x + 8, y2 + 7, &fonts::DejaVu9, phone ? C_CYAN : C_DIM, textdatum_t::middle_center);
  txt("Setup page opens", x + 22, y2, &fonts::DejaVu12, phone ? C_TEXT : C_LBL, textdatum_t::top_left);
  txt(phone ? "If it doesn't, scan the" : "If it doesn't, go to", x, y2 + 20, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);
  txt(phone ? "code or open" : "", x, y2 + 32, &fonts::DejaVu9, C_SUB, textdatum_t::top_left);
  txt(WiFi.softAPIP().toString().c_str(), x, phone ? y2 + 46 : y2 + 34, &fonts::DejaVu12, C_CYAN, textdatum_t::top_left);

  char m[48];
  if (setupMsg[0]) strlcpy(m, setupMsg, sizeof m);
  else snprintf(m, sizeof m, "%d saved", knownCount());
  char m2[48]; fitText(m, m2, sizeof m2, B_DONE.x - 14, &fonts::DejaVu12);
  txt(m2, 8, 192, &fonts::DejaVu12, setupMsg[0] ? C_VFR : C_SUB, textdatum_t::top_left);
  cv.fillRoundRect(B_DONE.x, B_DONE.y, B_DONE.w, B_DONE.h, 6, C_CYAN);
  txt("DONE", B_DONE.x + B_DONE.w / 2, B_DONE.y + B_DONE.h / 2 + 1, &fonts::DejaVu18, C_BG, textdatum_t::middle_center);
  drawKeys("", "DONE", "", 1);
}

// Only send the parts of the frame that changed. Each frame is drawn in memory,
// compared with what's already on the glass, and only changed bands of rows
// (each trimmed to its changed columns) go over the wire. A tapped arrow or a
// ticking clock is a few thousand pixels instead of all 76,800.
uint16_t* glass = nullptr;          // copy of what's on the screen right now (PSRAM)
bool      glassValid = false;

void pushChanged() {
  const int W = 320, H = 240, WW = W / 2;       // rows compared as 32-bit words (2 pixels)
  uint16_t* cur = (uint16_t*)cv.getBuffer();
  if (!glass || !cur || !glassValid) {
    BUS_LOCK(); cv.pushSprite(0, 0); BUS_UNLOCK();
    if (glass && cur) { memcpy(glass, cur, W * H * 2); glassValid = true; }
    return;
  }
  const uint32_t* a = (const uint32_t*)cur;
  const uint32_t* b = (const uint32_t*)glass;
  int y = 0;
  while (y < H) {
    if (!memcmp(a + y * WW, b + y * WW, WW * 4)) { y++; continue; }
    // A band: changed rows, allowing small unchanged gaps so text stays one push.
    int y0 = y, y1 = y, gap = 0;
    for (y = y0 + 1; y < H && gap <= 8; y++) {
      if (memcmp(a + y * WW, b + y * WW, WW * 4)) { y1 = y; gap = 0; } else gap++;
    }
    int x0 = WW, x1 = -1;
    for (int r = y0; r <= y1; r++) {
      const uint32_t* ra = a + r * WW; const uint32_t* rb = b + r * WW;
      int l = 0; while (l < WW && ra[l] == rb[l]) l++;
      if (l == WW) continue;
      int rr = WW - 1; while (rr > l && ra[rr] == rb[rr]) rr--;
      if (l < x0) x0 = l;
      if (rr > x1) x1 = rr;
    }
    int px = x0 * 2, pw = (x1 - x0 + 1) * 2, ph = y1 - y0 + 1;
    BUS_LOCK();
    M5.Display.setClipRect(px, y0, pw, ph);
    cv.pushSprite(0, 0);
    M5.Display.clearClipRect();
    BUS_UNLOCK();
    for (int r = y0; r <= y1; r++) memcpy(glass + r * W + px, cur + r * W + px, pw * 2);
    y = y1 + 1;
  }
}

void drawNow() {
  DATA_LOCK();                                   // the background task may be committing new data
  cv.fillScreen(C_BG);
  switch (screen) {
    case SCR_PICK:    drawPicker();  break;
    case SCR_ENTRY:   drawEntry();   break;
    case SCR_CADENCE: drawCadence(); break;
    case SCR_LOADING: drawLoading(); break;
    case SCR_WX:      drawWx();      break;
    case SCR_FREQ:    drawFreq();    break;
    case SCR_WIFI:    drawWifi();    break;
    case SCR_SETUP:   drawSetup();   break;
  }
  DATA_UNLOCK();
  pushChanged();                                 // takes BUS_LOCK: the SD card shares these wires
  dirty = false;
  lastDrawMs = millis();
}

// ============================ input ============================
void onTap(int x, int y) {
  switch (screen) {
    case SCR_PICK:
      for (int i = 0; i < nTiles; i++) if (inBox(tileBox(i), x, y)) { chooseAirport(tiles[i].icao); return; }
      if (inBox(B_TYPE, x, y)) openEntry();
      break;

    case SCR_ENTRY: {
      if (entryPad) { padPress(padKeyAt(x, y)); return; }
      // Arrows are handled on finger-down in handleInput(); here: letter boxes and LOAD.
      // Tapping a letter box opens the keypad at that position.
      for (int i = 0; i < 4; i++) if (inBox(entryCol(i, 1), x, y)) { entrySel = i; entryPad = true; dirty = true; return; }
      if (inBox(B_LOAD, x, y)) {
        char id[6]; entryIdent(id);
        if (strlen(id) >= 3) chooseAirport(id);
      }
      break;
    }

    case SCR_CADENCE:
      for (int i = 0; i < 3; i++) if (inBox(briBox(i), x, y)) { briMode = i; prefs.putUChar("bri", i); applyBri(); dirty = true; return; }
      for (int i = 0; i < 3; i++) if (inBox(cadBox(i), x, y)) { setCadence(CADENCES[i]); return; }
      break;

    case SCR_WX:
      if (!wx.valid && WiFi.status() != WL_CONNECTED && inBox(B_ADDWIFI, x, y)) { startSetup(SCR_WX); return; }
      if (wx.valid && inBox(B_WIND, x, y) && ap.nRwy > 0) {       // cycle runway ends, then back to best
        windSel++; if (windSel >= 2 * ap.nRwy) windSel = -1; dirty = true; return;
      }
      if (wx.valid && inBox(B_RAW, x, y) && taf.valid) {         // forecast strip <-> raw METAR
        showTaf = !showTaf; prefs.putBool("tafv", showTaf); dirty = true; return;
      }
      if (inBox(B_STATUS, x, y)) {
        if (WiFi.status() != WL_CONNECTED) openWifi();       // offline: the status line goes to Wi-Fi
        else { screen = SCR_CADENCE; dirty = true; }
      }
      break;

    case SCR_FREQ:
      if (ap.nFq == 0 && WiFi.status() != WL_CONNECTED && inBox(B_ADDWIFI, x, y)) { startSetup(SCR_FREQ); return; }
      if (inBox(B_LIST, x, y)) { freqPage++; dirty = true; }
      break;

    default: break;
  }
}

void handleInput() {
  bool busy = screen == SCR_LOADING;
  if (busy) return;

  // Everything acts on finger-DOWN, not on release: it feels instant, and a tap
  // that slides a little or lingers still counts. Holds still work on top of that.
  auto td = M5.Touch.getDetail();
  bool tapped = td.wasPressed() && td.y < 240;
  bool keyA = M5.BtnA.wasPressed(), keyB = M5.BtnB.wasPressed(), keyC = M5.BtnC.wasPressed();

  if (screen == SCR_ENTRY) {
    if (keyA) { entryHeldCol = -1; if (entryPad) { entryPad = false; dirty = true; } else openPicker(); return; }
    // Arrows act the moment a finger lands (a tap that slides or lingers still counts),
    // and repeat while held: slowly at first, then faster.
    if (entryPad) { /* keypad keys are handled in onTap */ }
    else if (td.wasPressed() && td.y < 240) {
      int col = -1, step = entryArrowAt(td.x, td.y, &col);
      if (step) { entryStep(col, step); entryHeldCol = col; entryHeldStep = step; entryRepeatMs = millis() + 550; entryRepeats = 0; }
      else entryHeldCol = -1;
    } else if (entryHeldCol >= 0) {
      int col = -1;
      bool still = td.isPressed() && entryArrowAt(td.x, td.y, &col) == entryHeldStep && col == entryHeldCol;
      if (!still) entryHeldCol = -1;
      else if ((int32_t)(millis() - entryRepeatMs) >= 0) {
        entryStep(entryHeldCol, entryHeldStep);
        entryRepeatMs = millis() + (++entryRepeats < 6 ? 260 : 140);
      }
    }
  }

  if (screen == SCR_SETUP) {                       // only DONE leaves phone setup
    if (keyB || (tapped && inBox(B_DONE, td.x, td.y))) exitSetup();
    return;
  }
  if (screen == SCR_WIFI) {
    if (keyA) { if (wifiBack == SCR_PICK || !airportLoaded) openPicker(); else { screen = wifiBack; dirty = true; } }
    else if (keyC) openPicker();
    else if (tapped && inBox(B_ADD, td.x, td.y)) startSetup(SCR_WIFI);
    return;
  }
  if (screen == SCR_PICK && keyB) {                // nothing saved yet: straight to phone setup
    if (knownCount() == 0) startSetup(SCR_PICK); else openWifi();
    return;
  }

  // WX: press = weather page; keep holding = refresh now (in the background).
  if (keyA && airportLoaded) { screen = SCR_WX; dirty = true; }
  if (M5.BtnA.wasHold() && airportLoaded) { screen = SCR_WX; needWx = true; dirty = true; }

  // FREQ: press = frequency page; keep holding = re-download them.
  if (keyB && airportLoaded) { screen = SCR_FREQ; freqPage = 0; dirty = true; }
  if (M5.BtnB.wasHold() && airportLoaded) {
    screen = SCR_LOADING; loadPct = -1;
    for (int i = 0; i < 3; i++) loadLines[i][0] = 0;
    setLoad(0, "Re-downloading frequencies"); drawNow();
    fgNetBegin();
    loadFreqs(ap, true);
    fgNetEnd();
    freqPage = 0; screen = SCR_FREQ; dirty = true;
  }

  if (keyC) openPicker();

  if (tapped) onTap(td.x, td.y);
}

// ============================ main ============================
// Boot screen: a runway in perspective, the panel's name and the tail number.
void drawSplash() {
  cv.fillScreen(C_BG);
  // Runway: trapezoid narrowing toward the horizon, dashed centerline, threshold bars.
  const int hzY = 112, baseY = 238, topHalf = 10, baseHalf = 92, cx = 160;
  cv.fillTriangle(cx - topHalf, hzY, cx + topHalf, hzY, cx - baseHalf, baseY, RGB565(0x1B2027));
  cv.fillTriangle(cx + topHalf, hzY, cx - baseHalf, baseY, cx + baseHalf, baseY, RGB565(0x1B2027));
  cv.drawLine(cx - topHalf, hzY, cx - baseHalf, baseY, C_SUB);
  cv.drawLine(cx + topHalf, hzY, cx + baseHalf, baseY, C_SUB);
  for (int i = 0; i < 6; i++) {                   // centerline dashes, growing toward you
    float a = (float)i / 6, b = a + 0.08f + a * 0.04f;
    int y0 = hzY + (int)((baseY - hzY) * a * a), y1 = hzY + (int)((baseY - hzY) * b * b);
    cv.drawWideLine(cx, y0, cx, y1, 0.6f + a * 1.6f, C_TEXT);
  }
  for (int i = -3; i <= 3; i++) if (i) cv.fillRect(cx + i * 22 - 6, 226, 12, 10, C_TEXT);   // threshold piano keys
  for (int i = 0; i < 3; i++) {                   // approach lights fading into the haze
    int y = hzY - 5 - i * 5;
    cv.fillCircle(cx - 14 + i * 2, y, 1, C_AMBER); cv.fillCircle(cx + 14 - i * 2, y, 1, C_AMBER);
  }
  txt("METAR PANEL", 160, 34, &fonts::DejaVu24, C_TEXT, textdatum_t::middle_center);
  char t[32]; snprintf(t, sizeof t, "%s  -  %s", TAIL, HOME_ICAO);
  txt(t, 160, 62, &fonts::DejaVu12, C_CYAN, textdatum_t::middle_center);
  char v[32]; snprintf(v, sizeof v, "v%s  supplemental use only", VERSION);
  txt(v, 160, 84, &fonts::DejaVu9, C_LBL, textdatum_t::middle_center);
  pushChanged();
}

void setup() {
  netMutex  = xSemaphoreCreateRecursiveMutex();
  dataMutex = xSemaphoreCreateRecursiveMutex();
  busMutex  = xSemaphoreCreateRecursiveMutex();
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(160);
  cv.setColorDepth(16);
  cv.setPsram(true);
  cv.createSprite(320, 240);
  glass = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);   // null = always full pushes
  drawSplash();
  uint32_t splashMs = millis();

  prefs.begin("metarpanel", false);
  cadence = prefs.getUChar("cad", DEFAULT_CADENCE);
  briMode = prefs.getUChar("bri", 0);
  showTaf = prefs.getBool("tafv", true);
  String d = prefs.getString("dest", HOME_ICAO);
  strlcpy(lastDest, d.c_str(), sizeof lastDest);

  sdInit();
  loadNets();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  wState = W_IDLE; wTimer = millis();   // first scan starts on the first loop

  applyBri();
  while (millis() - splashMs < 1400) { M5.update(); delay(20); }   // let the boot screen be seen
  openPicker();     // always ask where we're going
  drawNow();
#ifndef NET_INLINE
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);   // core 0; UI runs on core 1
#endif
}

void loop() {
  M5.update();
  handleInput();
  wifiTick();
  setupTick();

  if (!ntpStarted && WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }
  // Hand work to the background task; the screen keeps responding meanwhile.
  bool online = WiFi.status() == WL_CONNECTED;
  if (screen == SCR_PICK && !tilesFetched && online && (int32_t)(millis() - tilesRetryMs) >= 0) {
    tilesFetched = true; needTiles = true;
  }
  if (airportLoaded && !needWx && !netBusy && (int32_t)(millis() - nextFetchMs) >= 0) {
    needWx = true;
    nextFetchMs = millis() + RETRY_MS;                 // the job sets the real next time
  }
  if (freqsPending && airportLoaded && online) {
    freqsPending = false;                              // one try per connection; hold FREQ to retry
    needFreqs = true;
  }
  // SD frequency database: get it once, then refresh monthly, when the panel is idle.
  if (sdOk && online && !needDb && dbPct < 0 && screen != SCR_LOADING && screen != SCR_SETUP &&
      (int32_t)(millis() - dbRetryMs) >= 0) {
    if (dbStamp == 1 && timeSynced()) { dbStamp = (uint32_t)time(nullptr); prefs.putULong("dbt", dbStamp); }
    bool stale = dbStamp == 0 || (timeSynced() && dbStamp > 1 && (uint32_t)time(nullptr) - dbStamp > DB_MAX_AGE_S);
    if (stale) needDb = true;
    dbRetryMs = millis() + 60000UL;                    // re-check once a minute
  }
#ifdef NET_INLINE
  netStep();                                           // desktop simulator: no second core
#endif

  // Nothing on screen changes faster than once a minute (clock, obs age, countdown),
  // so tick once a minute instead of every second. Wi-Fi signal: every 3 s on that screen.
  static uint32_t lastMinute = 0;
  uint32_t minute = timeSynced() ? (uint32_t)(time(nullptr) / 60) : millis() / 60000;
  if (minute != lastMinute) { lastMinute = minute; dirty = true; applyBri(); }
  if (screen == SCR_WIFI && millis() - lastDrawMs > 3000) dirty = true;
  if (dirty) drawNow();
  delay(10);
}
