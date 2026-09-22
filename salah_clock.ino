/*
  ================================================================
   ESP32 SMART SALAH CLOCK + ESP-NOW MASTER HUB   -   v3
  ================================================================
   OLED : SH1106 128x64 I2C   (SDA = GPIO 21, SCL = GPIO 22)

   WHAT IS NEW IN v3
   - Smartwatch style card carousel every N minutes (default 3):
       Card 1  NEXT PRAYER in big type + countdown
       Card 2  TEMPERATURE + HUMIDITY
       Card 3  MESSAGE QUEUE (newest first, auto cycling)
     Cards slide in and out with eased motion, 30 fps.
   - ESP-NOW master hub
       * receives messages and sensor data from other ESP32 nodes
       * keeps the last 15 messages in a queue (newest first)
       * can broadcast messages back out to every node
       * broadcasts a sync packet (time, prayer times, weather)
         so nodes can show the same data
   - Weather: Open-Meteo (no API key). A sensor node sending
     telemetry over ESP-NOW overrides the internet value.
   - Dim panel rows: SAFE AREA calibration.
       Many SH1106 modules have unreadable rows at the top, and the
       last rows sit awkwardly close to the bezel. "Top safe rows"
       (default 10) and "Bottom safe rows" (default 8) fence off both
       ends. Content is then spread evenly inside what is left, never
       pushed against either edge, so nothing lands where you cannot
       read it. Use the web panel button "Show safe area" to calibrate.
   - One consistent 5 px side margin, one header rule, one footer
     line on every screen.

   WEB PANEL
     http://salah-clock.local  or the IP shown on boot

   ESP-NOW NOTES
     The hub stays on the WiFi channel of your router. Every node
     must use that same channel. The channel is shown in the web
     panel under "Nodes". See NODE_EXAMPLE.md in this folder for a
     ready to flash sender/receiver sketch.

   REQUIRED LIBRARIES (Library Manager)
     - U8g2
     - ArduinoJson (v6.15+ or v7)

   BOARD SETTINGS
     Board           : any ESP32 dev board (Arduino core 2.x or 3.x)
     Partition Scheme: Huge APP (3MB No OTA / 1MB SPIFFS)
                       The sketch also fits the default scheme, but
                       only just (95%). Huge APP leaves room to grow.
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <time.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Update.h>
#include "espnow_packet.h"

// ArduinoJson v6 / v7 compatibility
#if ARDUINOJSON_VERSION_MAJOR >= 7
  #define JSON_DOC(name, size) JsonDocument name
#else
  #define JSON_DOC(name, size) DynamicJsonDocument name(size)
#endif

// =====================================================
// USER CONFIGURATION
// =====================================================

// Your WiFi details live in secrets.h, which git ignores, so they never
// reach the repo. Copy secrets.example.h to secrets.h and edit it.
// Without that file the sketch still builds, using the placeholders below.
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID     "YOUR_WIFI_NAME"
  #define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

const char* HOSTNAME = "salah-clock";           // -> http://salah-clock.local

// ---- firmware identity and over-the-air updates ----
#define FW_VERSION "3.1.0"
#define OTA_REPO   "AhmadMahi/salah-clock-hub"
#define OTA_ASSET  "salah_clock_hub.bin"

// India Standard Time
const char* TZ_INFO = "IST-5:30";
const char* TZ_NAME = "Asia/Kolkata";

// Used when IP lookup fails (or when auto-location is off)
const char* DEFAULT_CITY = "Bengaluru";
const float DEFAULT_LAT  = 12.9716f;
const float DEFAULT_LON  = 77.5946f;

#define OLED_SDA 21
#define OLED_SCL 22

// =====================================================
// DISPLAY + LAYOUT
// =====================================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

const int SCREEN_W  = 128;
const int SCREEN_H  = 64;
const int MARGIN    = 5;                         // side margin on every screen
const int CONTENT_W = SCREEN_W - 2 * MARGIN;     // 118 px usable width

#define FONT_SMALL u8g2_font_5x8_tr
#define FONT_BODY u8g2_font_6x10_tr
#define COUNT(a)  (sizeof(a) / sizeof((a)[0]))

// Font candidates, largest first (text picks the first one that fits)
const uint8_t* const FONTS_BIG[]   = { u8g2_font_helvB14_tr, u8g2_font_helvB10_tr, u8g2_font_6x10_tr };
const uint8_t* const FONTS_TITLE[] = { u8g2_font_helvB10_tr, u8g2_font_6x10_tr };
const uint8_t* const FONTS_MID[]   = { u8g2_font_helvB10_tr, u8g2_font_6x10_tr };

// -------- SAFE AREA (the blurry-rows fix) --------
// Content never touches rows above topSafe or below (63 - botSafe).
int topSafe = 10;      // 0..28   dim rows at the top of the panel
int botSafe = 8;       // 0..16   dim rows at the bottom
// Content is spread evenly between the two, never pushed against either edge.

inline int areaTop() { return topSafe; }                       // first usable row
inline int areaBot() { return SCREEN_H - 1 - botSafe; }        // last usable row
inline int areaH()   { return areaBot() - areaTop() + 1; }
inline int yHead()   { return areaTop() + 8; }                 // header text baseline
inline int yRule()   { return areaTop() + 11; }                // divider under the header
inline int yBody()   { return areaTop() + 15; }                // first row of the body
inline int yFoot()   { return areaBot() - 1; }                 // footer text baseline

// =====================================================
// TIMING CONSTANTS
// =====================================================

const unsigned long CARD_SLIDE_MS   = 320;    // card transition
const unsigned long BLINK_PHASE_MS  = 6000;   // blinking part of an alert
const unsigned long ANIM_PHASE_MS   = 9000;   // Fajr / Maghrib animation
const unsigned long DUA_PHASE_MS    = 7000;   // "please read duas" screen
const unsigned long VERSE_ROTATE_MS = 8000;   // verse changes every 8 s
const unsigned long MSG_CYCLE_MS    = 2600;   // one message inside the message card
const unsigned long MSG_SLIDE_MS    = 320;
const unsigned long FRAME_MS        = 33;     // ~30 fps for animated screens
const unsigned long SYNC_PERIOD_MS  = 60000;  // ESP-NOW sync broadcast
const unsigned long WEATHER_OK_MS   = 600000; // refetch weather every 10 min
const unsigned long WEATHER_RETRY_MS= 120000;
const unsigned long SENSOR_FRESH_MS = 900000; // node telemetry valid for 15 min

// =====================================================
// SETTINGS (saved in flash)
// =====================================================

bool showClock       = true;    // main clock on the screen
bool showNextPrayer  = true;    // next-prayer card in the carousel
bool showWeatherCard = true;    // temperature / humidity card
bool showMsgCard     = true;    // message queue card
bool showVerse       = true;    // Quran verse during alert
bool prayerAlerts    = true;    // master switch for prayer-time alerts
bool blinkDisplay    = true;    // blink during alert
bool fajrReminder    = true;    // sunrise animation + morning duas
bool maghribReminder = true;    // sunset animation + evening duas
bool customMessages  = true;    // allow messages at all
bool popupMessages   = true;    // show a new message the moment it arrives
bool useIpLocation   = true;    // auto-detect city from IP
bool weatherEnabled  = true;    // fetch weather from the internet
bool useFahrenheit   = false;   // display unit
bool espNowEnabled   = true;    // ESP-NOW hub on/off
bool relayMessages   = true;    // rebroadcast node messages to the other nodes
bool shareSync       = true;    // broadcast time + prayer times to nodes
bool otaAuto         = true;    // check GitHub for new firmware on boot and daily

uint8_t alertMask    = 0x1F;    // bit0..4 = Fajr, Dhuhr, Asr, Maghrib, Isha
int slotMinutes      = 3;       // carousel every N minutes
int cardSeconds      = 4;       // seconds per card
int messageSeconds   = 6;       // minimum time a popup message stays on screen
int alertSeconds     = 60;      // total length of a prayer alert
int prayerMethod     = 1;       // 1 = Karachi (common in India)
int asrSchool        = 0;       // 0 = Shafi/Maliki/Hanbali, 1 = Hanafi
int brightness       = 255;     // OLED contrast 10..255

String locCity;
float  locLat = DEFAULT_LAT;
float  locLon = DEFAULT_LON;

Preferences prefs;
WebServer   server(80);

// =====================================================
// PRAYER DATA
// =====================================================

const char* PRAYER_NAMES[5] = { "Fajr", "Dhuhr", "Asr", "Maghrib", "Isha" };

int  prayerMin[5]      = { -1, -1, -1, -1, -1 };   // minutes since midnight
bool prayerOk          = false;
int  prayerDayKey      = -1;
int  alertFiredDay[5]  = { -1, -1, -1, -1, -1 };

bool prayerRefreshRequested = false;
bool ipLookupDone           = false;
unsigned long nextPrayerTry = 0;
unsigned long nextLocTry    = 0;

// Short Quran verses shown during the alert.
const char* VERSES[] = {
  "Establish prayer for My remembrance. (20:14)",
  "Maintain your prayers with care. (2:238)",
  "Seek help through patience and prayer. (2:45)",
  "Prayer is decreed for believers at fixed times. (4:103)",
  "Prayer restrains from indecency and wrongdoing. (29:45)",
  "In the remembrance of Allah hearts find rest. (13:28)",
  "Successful are the believers, those humble in prayer. (23:1-2)"
};
const int VERSE_COUNT = sizeof(VERSES) / sizeof(VERSES[0]);

// =====================================================
// WEATHER
// =====================================================

float wxTemp        = NAN;      // degrees Celsius
float wxHum         = NAN;      // percent
bool  wxFromNode    = false;    // true when a sensor node is feeding us
String wxSource     = "";       // node name when wxFromNode
unsigned long wxStamp    = 0;   // millis of the last good value
unsigned long nextWxTry  = 0;

bool weatherValid() {
  if (isnan(wxTemp) && isnan(wxHum)) return false;
  if (wxFromNode && millis() - wxStamp > SENSOR_FRESH_MS) return false;
  return true;
}

// =====================================================
// MESSAGE QUEUE (newest first, max 15)
// =====================================================

const int MAX_MSGS   = 15;
const int MSG_MAXLEN = 100;

enum { SRC_WEB = 0, SRC_NOW = 1, SRC_SYS = 2 };

struct MsgEntry {
  String   text;
  String   from;
  time_t   at;          // 0 when the clock was not synced yet
  unsigned long ms;
  uint8_t  src;
  bool     unread;
};

MsgEntry msgs[MAX_MSGS];
int  msgCount  = 0;
int  msgUnread = 0;
unsigned long msgLastArrival = 0;

// wrapped lines of the message being shown
const int MAX_MSG_LINES = 3;

// =====================================================
// ESP-NOW
// =====================================================

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// NowPacket, NOW_MAGIC and the PKT_* types live in espnow_packet.h so the
// same file can be dropped into every node sketch unchanged.

struct NodeInfo {
  uint8_t  mac[6];
  char     name[16];
  unsigned long lastMs;
  uint32_t packets;
  int8_t   rssi;
  bool     used;
};
const int MAX_NODES = 8;
NodeInfo nodes[MAX_NODES];

// Raw receive ring, filled from the ESP-NOW callback (WiFi task context)
struct RxItem {
  uint8_t mac[6];
  uint8_t len;
  int8_t  rssi;
  uint8_t data[200];
};
const int RX_QUEUE = 6;
RxItem   rxq[RX_QUEUE];
volatile uint8_t rxHead = 0, rxTail = 0;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

bool     espNowReady   = false;
uint32_t nowSeq        = 0;
uint32_t nowRxCount    = 0;
uint32_t nowTxCount    = 0;
uint32_t nowDropCount  = 0;
unsigned long lastSyncMs = 0;
String   hubName = "hub";

// =====================================================
// RUNTIME STATE
// =====================================================

enum { CARD_CLOCK = 0, CARD_NEXT, CARD_WEATHER, CARD_MSG, CARD_COUNT };

enum { MODE_CLOCK = 0, MODE_CARDS, MODE_ALERT, MODE_CALIB, MODE_BLANK };

int  mode            = MODE_CLOCK;
int  curCard         = CARD_CLOCK;
unsigned long cardStart = 0;
unsigned long lastSlotMs = 0;
unsigned long lastFrame  = 0;
int  lastDrawn       = -99;
int  lastSecond      = -1;

// carousel playlist
int  playlist[CARD_COUNT];
int  playCount = 0;
int  playIndex = 0;

// slide transition
bool  trActive   = false;
int   trFrom     = CARD_CLOCK;
int   trTo       = CARD_CLOCK;
int   trDir      = 1;            // +1 = new card comes from the right
unsigned long trStart = 0;

bool alertActive         = false;
int  alertPrayer         = 0;
int  alertVerse          = 0;
unsigned long alertStart = 0;

unsigned long calibUntil = 0;

int    cachedVerse   = -1;
String verseLines[4];
int    verseLineCount = 0;

bool mdnsStarted = false;
int  gOX = 0;                    // global X offset used while sliding cards

// Boot checklist rows: state 0 = waiting, 1 = working, 2 = ok, 3 = failed
struct BootRow {
  const char* label;
  uint8_t state;
  String value;
};
BootRow bootRows[5] = {
  { "WiFi",    0, "" },
  { "Time",    0, "" },
  { "City",    0, "" },
  { "Prayers", 0, "" },
  { "Mesh",    0, "" }
};

// =====================================================
// SMALL HELPERS
// =====================================================

bool due(unsigned long t) {
  return (long)(millis() - t) >= 0;
}

// Non-blocking "what time is it" (false until NTP has synced)
bool getNow(struct tm& t) {
  time_t now = time(nullptr);
  if (now < 1700000000L) return false;
  localtime_r(&now, &t);
  return true;
}

int dayKey(const struct tm& t) {
  return (t.tm_year + 1900) * 1000 + t.tm_yday;
}

String fmt12(int mins) {
  int h = mins / 60;
  int m = mins % 60;
  bool pm = h >= 12;
  int dh = h % 12;
  if (dh == 0) dh = 12;
  char b[12];
  snprintf(b, sizeof(b), "%02d:%02d %s", dh, m, pm ? "PM" : "AM");
  return String(b);
}

String fmtCountdown(int m) {
  if (m >= 60) return String(m / 60) + "h " + String(m % 60) + "m";
  return String(m) + "m";
}

// "3m ago", "2h ago", "now"
String fmtAge(unsigned long ms) {
  unsigned long s = (millis() - ms) / 1000UL;
  if (s < 45)    return "now";
  if (s < 3600)  return String(s / 60) + "m ago";
  if (s < 86400) return String(s / 3600) + "h ago";
  return String(s / 86400) + "d ago";
}

// "05:12" or "05:12 (IST)" -> minutes since midnight, -1 if invalid
int parseHHMM(const char* s) {
  int h = 0, m = 0;
  if (sscanf(s, "%d:%d", &h, &m) != 2) return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

// Index of next prayer + minutes until it starts
int findNextPrayer(int nowMin, int& minsUntil) {
  for (int i = 0; i < 5; i++) {
    if (prayerMin[i] > nowMin) {
      minsUntil = prayerMin[i] - nowMin;
      return i;
    }
  }
  minsUntil = 1440 - nowMin + prayerMin[0];    // tomorrow's Fajr
  return 0;
}

float toDisplayTemp(float c) {
  return useFahrenheit ? (c * 9.0f / 5.0f + 32.0f) : c;
}

String macStr(const uint8_t* m) {
  char b[18];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

// Cubic ease-out: fast start, soft landing
float easeOut(float p) {
  if (p < 0) p = 0;
  if (p > 1) p = 1;
  float q = 1.0f - p;
  return 1.0f - q * q * q;
}

// Smooth both ends
float easeInOut(float p) {
  if (p < 0) p = 0;
  if (p > 1) p = 1;
  return p * p * (3.0f - 2.0f * p);
}

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 32)  { o += ' '; }
    else                       { o += c; }
  }
  return o;
}

void jStr(String& o, const char* k, const String& v) {
  o += '"'; o += k; o += "\":\""; o += jsonEscape(v); o += "\",";
}
void jBool(String& o, const char* k, bool v) {
  o += '"'; o += k; o += "\":"; o += (v ? "true," : "false,");
}
void jNum(String& o, const char* k, long v) {
  o += '"'; o += k; o += "\":"; o += String(v); o += ',';
}
void jFloat(String& o, const char* k, float v, int dec = 4) {
  o += '"'; o += k; o += "\":";
  if (isnan(v)) o += "null";
  else          o += String(v, dec);
  o += ',';
}
void jEnd(String& o) {
  if (o.endsWith(",")) o.remove(o.length() - 1);
}

// =====================================================
// PREFERENCES
// =====================================================

void applyBrightness() {
  u8g2.setContrast((uint8_t)constrain(brightness, 10, 255));
}

void loadSettings() {
  prefs.begin("salah3", false);

  showClock       = prefs.getBool("clock",   true);
  showNextPrayer  = prefs.getBool("next",    true);
  showWeatherCard = prefs.getBool("wxcard",  true);
  showMsgCard     = prefs.getBool("msgcard", true);
  showVerse       = prefs.getBool("verse",   true);
  prayerAlerts    = prefs.getBool("alerts",  true);
  blinkDisplay    = prefs.getBool("blink",   true);
  fajrReminder    = prefs.getBool("fajr",    true);
  maghribReminder = prefs.getBool("maghrib", true);
  customMessages  = prefs.getBool("custom",  true);
  popupMessages   = prefs.getBool("popup",   true);
  useIpLocation   = prefs.getBool("ipLoc",   true);
  weatherEnabled  = prefs.getBool("wx",      true);
  useFahrenheit   = prefs.getBool("fahr",    false);
  espNowEnabled   = prefs.getBool("now",     true);
  relayMessages   = prefs.getBool("relay",   true);
  shareSync       = prefs.getBool("sync",    true);
  otaAuto         = prefs.getBool("ota",     true);

  alertMask       = prefs.getUChar("mask", 0x1F);
  slotMinutes     = prefs.getInt("slot",     3);
  cardSeconds     = prefs.getInt("cardsec",  4);
  messageSeconds  = prefs.getInt("msgsec",   6);
  alertSeconds    = prefs.getInt("alertsec", 60);
  prayerMethod    = prefs.getInt("method",   1);
  asrSchool       = prefs.getInt("school",   0);
  topSafe         = prefs.getInt("topsafe",  10);
  botSafe         = prefs.getInt("botsafe",  8);
  brightness      = prefs.getInt("bright",   255);

  locCity  = prefs.getString("city", DEFAULT_CITY);
  locLat   = prefs.getFloat("lat", DEFAULT_LAT);
  locLon   = prefs.getFloat("lon", DEFAULT_LON);
  hubName  = prefs.getString("hubname", "hub");

  slotMinutes    = constrain(slotMinutes, 1, 30);
  cardSeconds    = constrain(cardSeconds, 2, 10);
  messageSeconds = constrain(messageSeconds, 2, 60);
  alertSeconds   = constrain(alertSeconds, 30, 300);
  prayerMethod   = constrain(prayerMethod, 0, 15);
  asrSchool      = constrain(asrSchool, 0, 1);
  topSafe        = constrain(topSafe, 0, 28);
  botSafe        = constrain(botSafe, 0, 16);
  brightness     = constrain(brightness, 10, 255);
  if (locCity.length() == 0) locCity = DEFAULT_CITY;
  if (hubName.length() == 0) hubName = "hub";
  if (hubName.length() > 15) hubName = hubName.substring(0, 15);
}

void saveLocation() {
  prefs.putString("city", locCity);
  prefs.putFloat("lat", locLat);
  prefs.putFloat("lon", locLon);
}

void saveSettings() {
  prefs.putBool("clock",   showClock);
  prefs.putBool("next",    showNextPrayer);
  prefs.putBool("wxcard",  showWeatherCard);
  prefs.putBool("msgcard", showMsgCard);
  prefs.putBool("verse",   showVerse);
  prefs.putBool("alerts",  prayerAlerts);
  prefs.putBool("blink",   blinkDisplay);
  prefs.putBool("fajr",    fajrReminder);
  prefs.putBool("maghrib", maghribReminder);
  prefs.putBool("custom",  customMessages);
  prefs.putBool("popup",   popupMessages);
  prefs.putBool("ipLoc",   useIpLocation);
  prefs.putBool("wx",      weatherEnabled);
  prefs.putBool("fahr",    useFahrenheit);
  prefs.putBool("now",     espNowEnabled);
  prefs.putBool("relay",   relayMessages);
  prefs.putBool("sync",    shareSync);
  prefs.putBool("ota",     otaAuto);

  prefs.putUChar("mask", alertMask);
  prefs.putInt("slot",     slotMinutes);
  prefs.putInt("cardsec",  cardSeconds);
  prefs.putInt("msgsec",   messageSeconds);
  prefs.putInt("alertsec", alertSeconds);
  prefs.putInt("method",   prayerMethod);
  prefs.putInt("school",   asrSchool);
  prefs.putInt("topsafe",  topSafe);
  prefs.putInt("botsafe",  botSafe);
  prefs.putInt("bright",   brightness);
  prefs.putString("hubname", hubName);

  saveLocation();
  Serial.println("Settings saved.");
}

// =====================================================
// MESSAGE QUEUE
// =====================================================

void pushMessage(const String& textIn, const String& fromIn, uint8_t src) {
  String text = textIn;
  text.replace("\r", " ");
  text.replace("\n", " ");
  text.trim();
  if (text.length() == 0) return;
  if (text.length() > MSG_MAXLEN) text = text.substring(0, MSG_MAXLEN);

  String from = fromIn;
  from.trim();
  if (from.length() == 0) from = (src == SRC_NOW ? "node" : "panel");
  if (from.length() > 15) from = from.substring(0, 15);

  // drop an exact duplicate that arrived in the last 5 seconds (ESP-NOW retries)
  if (msgCount > 0 && msgs[0].text == text && msgs[0].from == from &&
      millis() - msgs[0].ms < 5000UL) {
    return;
  }

  int keep = min(msgCount, MAX_MSGS - 1);
  for (int i = keep; i > 0; i--) msgs[i] = msgs[i - 1];

  struct tm t;
  msgs[0].text   = text;
  msgs[0].from   = from;
  msgs[0].at     = getNow(t) ? time(nullptr) : 0;
  msgs[0].ms     = millis();
  msgs[0].src    = src;
  msgs[0].unread = true;

  if (msgCount < MAX_MSGS) msgCount++;

  msgUnread = 0;
  for (int i = 0; i < msgCount; i++) if (msgs[i].unread) msgUnread++;

  msgLastArrival = millis();
  Serial.printf("MSG [%s] %s\n", from.c_str(), text.c_str());
}

void clearMessages() {
  for (int i = 0; i < MAX_MSGS; i++) {
    msgs[i].text = "";
    msgs[i].from = "";
    msgs[i].unread = false;
  }
  msgCount  = 0;
  msgUnread = 0;
}

void deleteMessage(int idx) {
  if (idx < 0 || idx >= msgCount) return;
  if (msgs[idx].unread && msgUnread > 0) msgUnread--;
  for (int i = idx; i < msgCount - 1; i++) msgs[i] = msgs[i + 1];
  msgCount--;
  msgs[msgCount].text = "";
  msgs[msgCount].from = "";
  msgs[msgCount].unread = false;
}

void markAllRead() {
  for (int i = 0; i < msgCount; i++) msgs[i].unread = false;
  msgUnread = 0;
}

// =====================================================
// NETWORK: WIFI, TIME, LOCATION, PRAYER TIMES, WEATHER
// =====================================================

void startMDNS() {
  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.printf("mDNS: http://%s.local\n", HOSTNAME);
  }
}

void maintainWiFi() {
  static unsigned long lastTry = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (!mdnsStarted) startMDNS();
    return;
  }

  mdnsStarted = false;

  if (millis() - lastTry > 20000UL) {
    lastTry = millis();
    Serial.println("WiFi lost, reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void maintainTime() {
  static unsigned long lastKick = 0;
  struct tm t;
  if (getNow(t) || WiFi.status() != WL_CONNECTED) return;

  if (millis() - lastKick > 30000UL) {
    lastKick = millis();
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  }
}

bool httpGet(const String& url, bool secure, String& out, int timeoutMs) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  int code = -1;

  if (secure) {
    WiFiClientSecure client;
    client.setInsecure();
    if (!http.begin(client, url)) return false;
    code = http.GET();
    if (code == HTTP_CODE_OK) out = http.getString();
    http.end();
  } else {
    WiFiClient client;
    if (!http.begin(client, url)) return false;
    code = http.GET();
    if (code == HTTP_CODE_OK) out = http.getString();
    http.end();
  }

  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d  %s\n", code, url.c_str());
    return false;
  }
  return true;
}

// City + coordinates from the public IP address.
bool lookupLocationByIP() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String body;
  String city = "";
  float lat = 0, lon = 0;
  bool got = false;

  if (httpGet("http://ip-api.com/json/?fields=status,city,lat,lon", false, body, 6000)) {
    JSON_DOC(doc, 512);
    if (!deserializeJson(doc, body) && strcmp(doc["status"] | "", "success") == 0) {
      lat  = doc["lat"] | 0.0f;
      lon  = doc["lon"] | 0.0f;
      city = String((const char*)(doc["city"] | ""));
      got  = (lat != 0.0f || lon != 0.0f);
    }
  }

  if (!got && httpGet("https://ipwho.is/?fields=success,city,latitude,longitude", true, body, 6000)) {
    JSON_DOC(doc, 512);
    if (!deserializeJson(doc, body) && (doc["success"] | false)) {
      lat  = doc["latitude"]  | 0.0f;
      lon  = doc["longitude"] | 0.0f;
      city = String((const char*)(doc["city"] | ""));
      got  = (lat != 0.0f || lon != 0.0f);
    }
  }

  if (!got) {
    Serial.println("IP location failed, keeping saved location.");
    return false;
  }

  if (city.length() == 0) city = "Unknown";
  if (city.length() > 24) city = city.substring(0, 24);

  bool changed = (city != locCity) || fabsf(lat - locLat) > 0.01f || fabsf(lon - locLon) > 0.01f;

  locCity = city;
  locLat  = lat;
  locLon  = lon;
  saveLocation();
  if (changed) {
    prayerRefreshRequested = true;
    nextWxTry = 0;
  }

  Serial.printf("Location: %s (%.4f, %.4f)\n", locCity.c_str(), locLat, locLon);
  return true;
}

bool fetchPrayerTimes() {
  if (WiFi.status() != WL_CONNECTED) return false;

  struct tm t;
  if (!getNow(t)) return false;

  char date[16];
  strftime(date, sizeof(date), "%d-%m-%Y", &t);

  String url = "https://api.aladhan.com/v1/timings/" + String(date) +
               "?latitude="  + String(locLat, 4) +
               "&longitude=" + String(locLon, 4) +
               "&method="    + String(prayerMethod) +
               "&school="    + String(asrSchool) +
               "&timezonestring=" + TZ_NAME;

  Serial.println("Fetching prayer times: " + url);

  String payload;
  if (!httpGet(url, true, payload, 8000)) return false;

  JSON_DOC(filter, 256);
  filter["data"]["timings"] = true;

  JSON_DOC(doc, 2048);
  DeserializationError err = deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.print("JSON error: ");
    Serial.println(err.c_str());
    return false;
  }

  JsonObject timings = doc["data"]["timings"];
  if (timings.isNull()) {
    Serial.println("Prayer timings missing in response.");
    return false;
  }

  int tmp[5];
  for (int i = 0; i < 5; i++) {
    const char* s = timings[PRAYER_NAMES[i]] | "";
    tmp[i] = parseHHMM(s);
    if (tmp[i] < 0) {
      Serial.printf("Bad time for %s\n", PRAYER_NAMES[i]);
      return false;
    }
  }

  for (int i = 0; i < 5; i++) prayerMin[i] = tmp[i];
  prayerOk     = true;
  prayerDayKey = dayKey(t);

  Serial.println("Prayer times updated:");
  for (int i = 0; i < 5; i++) {
    Serial.printf("  %-8s %s\n", PRAYER_NAMES[i], fmt12(prayerMin[i]).c_str());
  }
  return true;
}

// Open-Meteo: free, no API key needed.
bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(locLat, 4) +
               "&longitude=" + String(locLon, 4) +
               "&current=temperature_2m,relative_humidity_2m";

  String payload;
  if (!httpGet(url, true, payload, 8000)) return false;

  JSON_DOC(filter, 256);
  filter["current"] = true;

  JSON_DOC(doc, 1024);
  if (deserializeJson(doc, payload, DeserializationOption::Filter(filter))) return false;

  JsonObject cur = doc["current"];
  if (cur.isNull()) return false;

  float t = cur["temperature_2m"]      | NAN;
  float h = cur["relative_humidity_2m"] | NAN;
  if (isnan(t) && isnan(h)) return false;

  // a live sensor node always wins over the internet forecast
  if (!wxFromNode || millis() - wxStamp > SENSOR_FRESH_MS) {
    wxTemp     = t;
    wxHum      = h;
    wxFromNode = false;
    wxSource   = "online";
    wxStamp    = millis();
    Serial.printf("Weather: %.1f C  %.0f %%\n", t, h);
  }
  return true;
}

// Keeps location, prayer times and weather fresh
void maintainData() {
  if (WiFi.status() != WL_CONNECTED) return;

  struct tm t;
  if (!getNow(t)) return;

  if (useIpLocation && !ipLookupDone && due(nextLocTry)) {
    if (lookupLocationByIP()) ipLookupDone = true;
    else nextLocTry = millis() + 300000UL;
  }

  bool need = !prayerOk || prayerDayKey != dayKey(t) || prayerRefreshRequested;
  if (need && due(nextPrayerTry)) {
    if (fetchPrayerTimes()) prayerRefreshRequested = false;
    else nextPrayerTry = millis() + (prayerOk ? 300000UL : 60000UL);
  }

  if (weatherEnabled && due(nextWxTry)) {
    nextWxTry = millis() + (fetchWeather() ? WEATHER_OK_MS : WEATHER_RETRY_MS);
  }
}

// =====================================================
// ESP-NOW HUB
// =====================================================

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
  int8_t rssi = (info->rx_ctrl != nullptr) ? (int8_t)info->rx_ctrl->rssi : 0;
#else
void onNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  int8_t rssi = 0;
#endif
  if (len <= 0 || len > (int)sizeof(rxq[0].data)) return;

  portENTER_CRITICAL_ISR(&rxMux);
  uint8_t next = (rxHead + 1) % RX_QUEUE;
  if (next == rxTail) {
    nowDropCount++;                       // queue full, drop the oldest arrival
  } else {
    memcpy(rxq[rxHead].mac, mac, 6);
    rxq[rxHead].len  = (uint8_t)len;
    rxq[rxHead].rssi = rssi;
    memcpy(rxq[rxHead].data, data, len);
    rxHead = next;
  }
  portEXIT_CRITICAL_ISR(&rxMux);
}

void initEspNow() {
  if (espNowReady || !espNowEnabled) return;

  WiFi.setSleep(false);                   // modem sleep would drop packets

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed.");
    return;
  }
  esp_now_register_recv_cb(onNowRecv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;                       // 0 = follow the current WiFi channel
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  esp_now_add_peer(&peer);

  espNowReady = true;
  Serial.printf("ESP-NOW hub ready on channel %d, MAC %s\n",
                WiFi.channel(), WiFi.macAddress().c_str());
}

void stopEspNow() {
  if (!espNowReady) return;
  esp_now_unregister_recv_cb();
  esp_now_deinit();
  espNowReady = false;
  Serial.println("ESP-NOW stopped.");
}

void fillPacket(NowPacket& p, uint8_t type) {
  memset(&p, 0, sizeof(p));
  p.magic   = NOW_MAGIC;
  p.version = NOW_VERSION;
  p.type    = type;
  p.seq     = ++nowSeq;
  strncpy(p.from, hubName.c_str(), sizeof(p.from) - 1);
  p.temp = NAN;
  p.hum  = NAN;
  for (int i = 0; i < 5; i++) p.prayer[i] = 0xFFFF;
}

bool nowSend(const NowPacket& p) {
  if (!espNowReady) return false;
  esp_err_t r = esp_now_send(BROADCAST_MAC, (const uint8_t*)&p, sizeof(p));
  if (r == ESP_OK) { nowTxCount++; return true; }
  Serial.printf("ESP-NOW send failed: %d\n", (int)r);
  return false;
}

bool broadcastMessage(const String& text, const String& from) {
  NowPacket p;
  fillPacket(p, PKT_MSG);
  if (from.length()) strncpy(p.from, from.c_str(), sizeof(p.from) - 1);
  strncpy(p.text, text.c_str(), sizeof(p.text) - 1);
  return nowSend(p);
}

// Time + prayer times + weather, so every node can show the same data
void broadcastSync() {
  if (!espNowReady || !shareSync) return;

  NowPacket p;
  fillPacket(p, PKT_SYNC);
  strncpy(p.text, locCity.c_str(), sizeof(p.text) - 1);
  if (prayerOk) for (int i = 0; i < 5; i++) p.prayer[i] = (uint16_t)prayerMin[i];
  if (weatherValid()) { p.temp = wxTemp; p.hum = wxHum; }

  struct tm t;
  p.epoch = getNow(t) ? (uint32_t)time(nullptr) : 0;

  nowSend(p);
}

int findNode(const uint8_t* mac) {
  for (int i = 0; i < MAX_NODES; i++) {
    if (nodes[i].used && memcmp(nodes[i].mac, mac, 6) == 0) return i;
  }
  return -1;
}

int touchNode(const uint8_t* mac, const char* name, int8_t rssi) {
  int i = findNode(mac);
  if (i < 0) {
    for (int k = 0; k < MAX_NODES; k++) {
      if (!nodes[k].used) { i = k; break; }
    }
  }
  if (i < 0) {                             // table full: replace the oldest
    i = 0;
    for (int k = 1; k < MAX_NODES; k++) {
      if (nodes[k].lastMs < nodes[i].lastMs) i = k;
    }
    nodes[i].packets = 0;
  }

  if (!nodes[i].used || memcmp(nodes[i].mac, mac, 6) != 0) {
    memcpy(nodes[i].mac, mac, 6);
    nodes[i].packets = 0;
  }
  nodes[i].used   = true;
  nodes[i].lastMs = millis();
  nodes[i].rssi   = rssi;
  nodes[i].packets++;
  if (name && name[0]) {
    strncpy(nodes[i].name, name, sizeof(nodes[i].name) - 1);
    nodes[i].name[sizeof(nodes[i].name) - 1] = 0;
  } else if (nodes[i].name[0] == 0) {
    strncpy(nodes[i].name, "node", sizeof(nodes[i].name) - 1);
  }
  return i;
}

int nodeCount() {
  int n = 0;
  for (int i = 0; i < MAX_NODES; i++) if (nodes[i].used) n++;
  return n;
}

void handleNowMessage(const String& text, const String& from);   // forward

// Runs in loop(), not in the callback
void processEspNow() {
  while (true) {
    RxItem item;
    portENTER_CRITICAL(&rxMux);
    bool have = (rxTail != rxHead);
    if (have) {
      item = rxq[rxTail];
      rxTail = (rxTail + 1) % RX_QUEUE;
    }
    portEXIT_CRITICAL(&rxMux);
    if (!have) break;

    nowRxCount++;

    // ---- structured packet
    if (item.len == sizeof(NowPacket)) {
      NowPacket p;
      memcpy(&p, item.data, sizeof(p));
      if (p.magic == NOW_MAGIC) {
        p.from[sizeof(p.from) - 1] = 0;
        p.text[sizeof(p.text) - 1] = 0;
        int ni = touchNode(item.mac, p.from, item.rssi);
        String from = String(nodes[ni].name);

        switch (p.type) {
          case PKT_MSG:
            handleNowMessage(String(p.text), from);
            break;

          case PKT_TELEM:
            if (!isnan(p.temp) || !isnan(p.hum)) {
              if (!isnan(p.temp)) wxTemp = p.temp;
              if (!isnan(p.hum))  wxHum  = p.hum;
              wxFromNode = true;
              wxSource   = from;
              wxStamp    = millis();
            }
            if (p.text[0]) handleNowMessage(String(p.text), from);
            break;

          case PKT_PING:
          default:
            break;
        }
        continue;
      }
    }

    // ---- plain text fallback: any node that just sends a string
    bool printable = true;
    for (int i = 0; i < item.len; i++) {
      uint8_t c = item.data[i];
      if (c == 0) break;
      if (c < 9 || c > 126) { printable = false; break; }
    }
    if (printable && item.len > 0) {
      char buf[101];
      int n = min((int)item.len, (int)sizeof(buf) - 1);
      memcpy(buf, item.data, n);
      buf[n] = 0;
      int ni = touchNode(item.mac, "node", item.rssi);
      handleNowMessage(String(buf), String(nodes[ni].name));
    }
  }
}

void maintainEspNow() {
  if (espNowEnabled && !espNowReady && WiFi.status() == WL_CONNECTED) initEspNow();
  if (!espNowEnabled && espNowReady) stopEspNow();

  processEspNow();

  if (espNowReady && shareSync && millis() - lastSyncMs >= SYNC_PERIOD_MS) {
    lastSyncMs = millis();
    broadcastSync();
  }
}

// =====================================================
// DRAWING PRIMITIVES (gOX = horizontal slide offset)
// =====================================================

void pStr(int x, int y, const char* s)        { u8g2.drawStr(x + gOX, y, s); }
void pStr(int x, int y, const String& s)      { u8g2.drawStr(x + gOX, y, s.c_str()); }
void pBox(int x, int y, int w, int h)         { u8g2.drawBox(x + gOX, y, w, h); }
void pRBox(int x, int y, int w, int h, int r) { u8g2.drawRBox(x + gOX, y, w, h, r); }
void pFrame(int x, int y, int w, int h)       { u8g2.drawFrame(x + gOX, y, w, h); }
void pRFrame(int x, int y, int w, int h, int r){ u8g2.drawRFrame(x + gOX, y, w, h, r); }
void pHLine(int x, int y, int w)              { u8g2.drawHLine(x + gOX, y, w); }
void pVLine(int x, int y, int h)              { u8g2.drawVLine(x + gOX, y, h); }
void pPixel(int x, int y)                     { u8g2.drawPixel(x + gOX, y); }
void pDisc(int x, int y, int r)               { u8g2.drawDisc(x + gOX, y, r); }
void pCircle(int x, int y, int r)             { u8g2.drawCircle(x + gOX, y, r); }
void pLine(int x0, int y0, int x1, int y1)    { u8g2.drawLine(x0 + gOX, y0, x1 + gOX, y1); }
void pTriangle(int x0, int y0, int x1, int y1, int x2, int y2) {
  u8g2.drawTriangle(x0 + gOX, y0, x1 + gOX, y1, x2 + gOX, y2);
}

int strW(const String& s) { return u8g2.getStrWidth(s.c_str()); }

void pCenter(const String& s, int y) {
  int x = (SCREEN_W - strW(s)) / 2;
  if (x < MARGIN) x = MARGIN;
  pStr(x, y, s);
}

void pRight(const String& s, int y) {
  pStr(SCREEN_W - MARGIN - strW(s), y, s);
}

// Trims a string until it fits, adding a dot when it was cut
String clip(const String& in, int maxW) {
  String s = in;
  if (strW(s) <= maxW) return s;
  while (s.length() > 1 && strW(s + ".") > maxW) s.remove(s.length() - 1);
  return s + ".";
}

// Picks the biggest font that fits inside the margins, then centers
void pCenterFit(const String& text, int y, const uint8_t* const* fonts, int count, int maxW = CONTENT_W) {
  for (int i = 0; i < count; i++) {
    u8g2.setFont(fonts[i]);
    if (strW(text) <= maxW) break;
  }
  pCenter(clip(text, maxW), y);
}

// Word-wraps text into lines that fit maxWidth using the CURRENT font
int wrapText(const String& text, String* lines, int maxLines, int maxWidth) {
  int count = 0;
  String current = "";
  int i = 0;
  int n = text.length();

  while (i < n && count < maxLines) {
    int j = i;
    while (j < n && text.charAt(j) != ' ') j++;
    String word = text.substring(i, j);
    i = j + 1;
    if (word.length() == 0) continue;

    while (strW(word) > maxWidth) {                // very long word
      int k = word.length() - 1;
      while (k > 1 && strW(word.substring(0, k)) > maxWidth) k--;
      if (current.length() > 0) {
        lines[count++] = current;
        current = "";
        if (count >= maxLines) return count;
      }
      lines[count++] = word.substring(0, k);
      word = word.substring(k);
      if (count >= maxLines) return count;
    }

    String candidate = current.length() ? (current + " " + word) : word;
    if (strW(candidate) <= maxWidth) {
      current = candidate;
    } else {
      lines[count++] = current;
      current = word;
    }
  }

  if (current.length() > 0 && count < maxLines) lines[count++] = current;
  return count;
}

bool frameDue(int tag, unsigned long interval) {
  unsigned long now = millis();
  if (tag != lastDrawn || now - lastFrame >= interval) {
    lastDrawn = tag;
    lastFrame = now;
    return true;
  }
  return false;
}

// =====================================================
// ICONS
// =====================================================

// 11 x 8, y = bottom row
void icoWifi(int x, int y, int bars) {
  for (int i = 0; i < 4; i++) {
    int h  = 2 + i * 2;
    int bx = x + i * 3;
    if (i < bars) pBox(bx, y - h, 2, h);
    else          pPixel(bx, y - 1);
  }
}

// 9 x 7, y = top row
void icoMail(int x, int y) {
  pFrame(x, y, 9, 7);
  pLine(x + 1, y + 1, x + 4, y + 3);
  pLine(x + 7, y + 1, x + 4, y + 3);
}

// 7 x 9, y = top row
void icoDrop(int x, int y) {
  pTriangle(x + 3, y, x, y + 5, x + 6, y + 5);
  pDisc(x + 3, y + 5, 3);
}

// 5 x 10, y = top row
void icoTherm(int x, int y) {
  pRFrame(x + 1, y, 3, 7, 1);
  pDisc(x + 2, y + 7, 2);
  pVLine(x + 2, y + 2, 4);
}

// 9 x 9, y = top row
void icoMoon(int x, int y) {
  pDisc(x + 4, y + 4, 4);
  u8g2.setDrawColor(0);
  pDisc(x + 6, y + 3, 3);
  u8g2.setDrawColor(1);
}

// degree sign: x = left edge, cy = centre row of the little ring
void icoDeg(int x, int cy, int r) {
  pCircle(x + r, cy, r);
}

int wifiBars() {
  if (WiFi.status() != WL_CONNECTED) return 0;
  int r = WiFi.RSSI();
  if (r > -55) return 4;
  if (r > -67) return 3;
  if (r > -78) return 2;
  return 1;
}

// =====================================================
// OVER-THE-AIR UPDATE (GitHub Releases)
// =====================================================
//  On boot, and once a day after that, the clock asks GitHub for the
//  latest release of OTA_REPO. If its tag is newer than FW_VERSION it
//  downloads OTA_ASSET into the spare app partition and reboots into
//  it. A failed update changes nothing: the running firmware stays.
//
//  This needs a partition scheme WITH an OTA slot:
//  Tools > Partition Scheme > "Minimal SPIFFS (1.9MB APP with OTA)".

String  otaStatus    = "not checked yet";
String  otaLatest    = "";
bool    otaRequested = false;
bool    otaBusy      = false;
unsigned long nextOtaCheck = 0;

// "v3.1.0" -> 3001000, so versions compare as plain numbers
long verNum(const String& v) {
  int a = 0, b = 0, c = 0;
  const char* p = v.c_str();
  while (*p && !isdigit((unsigned char)*p)) p++;
  sscanf(p, "%d.%d.%d", &a, &b, &c);
  return (long)a * 1000000L + (long)b * 1000L + (long)c;
}

void drawOtaScreen(const String& title, const String& sub, int pct) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_SMALL);
  pCenter(title, yHead());
  pHLine(MARGIN, yRule(), CONTENT_W);

  u8g2.setFont(FONT_BODY);
  pCenter(clip(sub, CONTENT_W), yRule() + (areaBot() - yRule()) / 2);

  if (pct >= 0) {
    int by = areaBot() - 8;
    u8g2.drawFrame(MARGIN, by, CONTENT_W, 8);
    int w = (CONTENT_W - 4) * constrain(pct, 0, 100) / 100;
    if (w > 0) u8g2.drawBox(MARGIN + 2, by + 2, w, 4);
  }
  u8g2.sendBuffer();
}

void otaProgress(size_t done, size_t total) {
  static int lastPct = -1;
  int pct = total ? (int)((done * 100ULL) / total) : 0;
  if (pct == lastPct) return;
  lastPct = pct;
  drawOtaScreen("INSTALLING", otaLatest, pct);
}

// Reads the latest release: its tag and the download URL of our .bin
bool otaFindLatest(String& tag, String& url) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.setUserAgent("salah-clock");

  String api = "https://api.github.com/repos/" OTA_REPO "/releases/latest";
  if (!http.begin(client, api)) { otaStatus = "cannot reach GitHub"; return false; }
  http.addHeader("Accept", "application/vnd.github+json");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaStatus = (code == 404) ? "no release published yet" : ("GitHub HTTP " + String(code));
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JSON_DOC(filter, 256);
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;

  JSON_DOC(doc, 4096);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    otaStatus = "could not read the release";
    return false;
  }

  tag = String((const char*)(doc["tag_name"] | ""));
  url = "";
  for (JsonObject a : doc["assets"].as<JsonArray>()) {
    if (String((const char*)(a["name"] | "")) == OTA_ASSET) {
      url = String((const char*)(a["browser_download_url"] | ""));
      break;
    }
  }
  if (url.length() == 0) {                       // fall back to any .bin in the release
    for (JsonObject a : doc["assets"].as<JsonArray>()) {
      String n = String((const char*)(a["name"] | ""));
      if (n.endsWith(".bin")) { url = String((const char*)(a["browser_download_url"] | "")); break; }
    }
  }
  if (tag.length() == 0 || url.length() == 0) {
    otaStatus = "release has no firmware file";
    return false;
  }
  return true;
}

bool otaApply(const String& url) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setUserAgent("salah-clock");

  if (!http.begin(client, url)) { otaStatus = "cannot open the download"; return false; }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    otaStatus = "download HTTP " + String(code);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) { otaStatus = "firmware size unknown"; http.end(); return false; }

  if (!Update.begin(len)) {
    otaStatus = String("no room: ") + Update.errorString();
    http.end();
    return false;
  }
  Update.onProgress(otaProgress);

  size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();

  if (written != (size_t)len) {
    otaStatus = "download cut short";
    Update.abort();
    return false;
  }
  if (!Update.end(true)) {
    otaStatus = String("rejected: ") + Update.errorString();
    return false;
  }
  return true;
}

// Returns true only when an update was installed (the board then reboots)
bool otaCheck(bool onScreen) {
  if (WiFi.status() != WL_CONNECTED) { otaStatus = "offline"; return false; }

  otaBusy = true;
  if (onScreen) drawOtaScreen("FIRMWARE", "checking...", -1);

  String tag, url;
  if (!otaFindLatest(tag, url)) {
    Serial.println("OTA: " + otaStatus);
    otaBusy = false;
    return false;
  }
  otaLatest = tag;

  if (verNum(tag) <= verNum(FW_VERSION)) {
    otaStatus = "up to date";
    Serial.printf("OTA: running %s, latest %s\n", FW_VERSION, tag.c_str());
    otaBusy = false;
    return false;
  }

  Serial.printf("OTA: updating %s -> %s\n", FW_VERSION, tag.c_str());
  if (onScreen) drawOtaScreen("INSTALLING", tag, 0);

  if (!otaApply(url)) {
    Serial.println("OTA failed: " + otaStatus);
    if (onScreen) { drawOtaScreen("UPDATE FAILED", otaStatus, -1); delay(2500); }
    otaBusy = false;
    return false;
  }

  otaStatus = "installed " + tag;
  Serial.println("OTA: installed, rebooting");
  drawOtaScreen("UPDATED", tag, 100);
  delay(1500);
  ESP.restart();
  return true;
}

void maintainOta() {
  if (otaRequested) {
    otaRequested = false;
    otaCheck(true);
    return;
  }
  if (!otaAuto || alertActive) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!due(nextOtaCheck)) return;
  nextOtaCheck = millis() + 86400000UL;          // once a day
  otaCheck(true);
}

// =====================================================
// WAKE-UP ANIMATION
// =====================================================

void drawEye(int cx, int cy, int h, int px, int py) {
  const int EW = 30;
  u8g2.setDrawColor(1);

  if (h < 3) {
    u8g2.drawRBox(cx - EW / 2, cy - 1, EW, 3, 1);
    return;
  }

  int r = min(8, (h - 1) / 2);
  u8g2.drawRBox(cx - EW / 2, cy - h / 2, EW, h, r);

  if (h >= 14) {
    u8g2.setDrawColor(0);
    u8g2.drawDisc(cx + px, cy + py, 5);
    u8g2.setDrawColor(1);
    u8g2.drawPixel(cx + px - 2, cy + py - 2);
  }
}

void drawHappyEye(int cx, int cy) {
  u8g2.setDrawColor(1);
  for (int r = 9; r <= 11; r++) {
    u8g2.drawCircle(cx, cy + 7, r, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  }
}

// zLift 0..1 animates the floating z's; titleWipe 0..1 wipes the name in
void eyesFrame(int h, int px, int py, bool happy, float zLift, float titleWipe) {
  int cy = areaTop() + (areaH() * 4) / 10;
  if (cy < 18) cy = 18;

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  if (happy) {
    drawHappyEye(34, cy);
    drawHappyEye(94, cy);
  } else {
    drawEye(34, cy, h, px, py);
    drawEye(94, cy, h, px, py);
  }

  if (zLift > 0) {
    u8g2.setFont(FONT_SMALL);
    int base = cy - 4;
    float a = fmodf(zLift, 1.0f);
    u8g2.drawStr(112, base - (int)(a * 8.0f), "z");
    u8g2.setFont(FONT_BODY);
    u8g2.drawStr(114, base - 9 - (int)(a * 10.0f), "Z");
  }

  if (titleWipe > 0) {
    u8g2.setFont(u8g2_font_helvB10_tr);
    String s = "SALAH CLOCK";
    int w = u8g2.getStrWidth(s.c_str());
    int x = (SCREEN_W - w) / 2;
    int y = areaBot() - 1;
    u8g2.setClipWindow(x, y - 12, x + (int)(w * easeOut(titleWipe)) + 1, y + 3);
    u8g2.drawStr(x, y, s.c_str());
    u8g2.setMaxClipWindow();
  }

  u8g2.sendBuffer();
}

// Frame-timed so the motion is even on any board speed
void wakeUpAnimation() {
  unsigned long t0 = millis();

  // 1) asleep, z's drifting up (1.8 s)
  while (millis() - t0 < 1800) {
    float el = (millis() - t0) / 1000.0f;
    eyesFrame(0, 0, 0, false, el > 0.35f ? el * 1.2f : 0.0f, 0);
    delay(FRAME_MS);
  }

  // 2) groggy flutter, then a smooth eased open (1.4 s)
  t0 = millis();
  while (millis() - t0 < 1400) {
    float p = (millis() - t0) / 1400.0f;
    int h;
    if (p < 0.35f) {
      float f = sinf(p * 28.0f);                       // flutter
      h = (int)(4.0f + 3.0f * f);
      if (h < 0) h = 0;
    } else {
      h = (int)(30.0f * easeOut((p - 0.35f) / 0.65f));
    }
    eyesFrame(h, 0, 0, false, 0, 0);
    delay(FRAME_MS);
  }

  // 3) look around on a smooth sine path (1.6 s)
  t0 = millis();
  while (millis() - t0 < 1600) {
    float p = (millis() - t0) / 1600.0f;
    int px = (int)(8.0f * sinf(p * 6.2832f));
    int py = (int)(2.5f * sinf(p * 12.566f));
    eyesFrame(30, px, py, false, 0, 0);
    delay(FRAME_MS);
  }

  // 4) two quick blinks
  for (int b = 0; b < 2; b++) {
    t0 = millis();
    while (millis() - t0 < 260) {
      float p = (millis() - t0) / 260.0f;
      int h = (int)(30.0f * fabsf(cosf(p * 3.1416f)));
      eyesFrame(max(h, 1), 0, 0, false, 0, 0);
      delay(FRAME_MS);
    }
    delay(180);
  }

  // 5) smile + the name wiping in
  t0 = millis();
  while (millis() - t0 < 1500) {
    float p = (millis() - t0) / 700.0f;
    eyesFrame(0, 0, 0, true, 0, p);
    delay(FRAME_MS);
  }

  u8g2.clearBuffer();
  u8g2.sendBuffer();
  delay(120);
}

// =====================================================
// BOOT CHECKLIST
// =====================================================

void drawBootScreen() {
  const int rows = 5;
  int sp = (areaH() - 8) / (rows - 1);      // last row lands on the safe bottom edge
  if (sp < 7) sp = 7;

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(sp >= 10 ? FONT_BODY : FONT_SMALL);

  for (int i = 0; i < rows; i++) {
    int y = areaTop() + 7 + i * sp;    // glyph top sits exactly on areaTop()
    if (y > areaBot()) break;

    u8g2.drawStr(MARGIN, y, bootRows[i].label);

    String v = "";
    switch (bootRows[i].state) {
      case 1: v = String("...").substring(0, 1 + (millis() / 250) % 3); break;
      case 2: v = bootRows[i].value.length() ? bootRows[i].value : String("OK"); break;
      case 3: v = "FAIL"; break;
      default: v = "-"; break;
    }

    int maxW = CONTENT_W - u8g2.getStrWidth(bootRows[i].label) - 8;
    while (v.length() > 1 && u8g2.getStrWidth(v.c_str()) > maxW) v.remove(v.length() - 1);
    u8g2.drawStr(SCREEN_W - MARGIN - u8g2.getStrWidth(v.c_str()), y, v.c_str());
  }
  u8g2.sendBuffer();
}

void showWebInfoScreen() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_SMALL);
  pCenter("OPEN ON YOUR PHONE", yHead());
  u8g2.drawHLine(MARGIN, yRule(), CONTENT_W);

  pCenterFit(WiFi.localIP().toString(), yBody() + 11, FONTS_MID, COUNT(FONTS_MID));

  u8g2.setFont(FONT_SMALL);
  pCenter(String(HOSTNAME) + ".local", yFoot());
  u8g2.sendBuffer();
  delay(3200);
}

void runBootSequence() {
  // ---- WiFi
  bootRows[0].state = 1;
  drawBootScreen();

  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long s = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - s < 20000UL) {
    drawBootScreen();
    delay(120);
  }
  bool wifiOk = (WiFi.status() == WL_CONNECTED);
  bootRows[0].state = wifiOk ? 2 : 3;
  if (wifiOk) startMDNS();
  drawBootScreen();
  delay(250);

  // ---- Time
  bootRows[1].state = 1;
  drawBootScreen();
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  struct tm t;
  s = millis();
  while (!getNow(t) && wifiOk && millis() - s < 15000UL) {
    drawBootScreen();
    delay(120);
  }
  bootRows[1].state = getNow(t) ? 2 : 3;
  drawBootScreen();
  delay(250);

  // ---- Firmware (may reboot into a new build and never come back here)
  if (otaAuto && wifiOk) {
    otaCheck(true);
    nextOtaCheck = millis() + 86400000UL;
  }

  // ---- City
  bootRows[2].state = 1;
  drawBootScreen();
  if (useIpLocation && wifiOk) {
    ipLookupDone = lookupLocationByIP();
    if (!ipLookupDone) nextLocTry = millis() + 300000UL;
  }
  bootRows[2].value = locCity;
  bootRows[2].state = 2;
  drawBootScreen();
  delay(300);

  // ---- Prayer times (+ a first weather read)
  bootRows[3].state = 1;
  drawBootScreen();
  bool ok = fetchPrayerTimes();
  if (!ok) nextPrayerTry = millis() + 60000UL;
  bootRows[3].state = ok ? 2 : 3;
  drawBootScreen();
  if (weatherEnabled && wifiOk) {
    nextWxTry = millis() + (fetchWeather() ? WEATHER_OK_MS : WEATHER_RETRY_MS);
  }

  // ---- ESP-NOW mesh
  bootRows[4].state = 1;
  drawBootScreen();
  if (espNowEnabled) {
    initEspNow();
    bootRows[4].state = espNowReady ? 2 : 3;
    bootRows[4].value = espNowReady ? ("CH " + String(WiFi.channel())) : "";
  } else {
    bootRows[4].state = 2;
    bootRows[4].value = "OFF";
  }
  drawBootScreen();
  delay(800);

  if (wifiOk) showWebInfoScreen();
}

// =====================================================
// CARD: CLOCK  (smartwatch face)
// =====================================================
//   status strip : wifi + temp + humidity            mail + unread
//   big time     : HH:MM   with PM and seconds stacked beside it
//   footer       : date on the left, next prayer on the right
//   hairline     : minute progress along the bottom edge

// Builds the line that scrolls along the bottom of the clock face.
// Rebuilt once a second, which is plenty - the scroll itself is smooth.
String buildTicker() {
  String o = "";
  const char* SEP = "   |   ";

  if (weatherValid() && !isnan(wxTemp)) {
    o += String((int)roundf(toDisplayTemp(wxTemp)));
    o += useFahrenheit ? " F" : " C";
  }
  if (weatherValid() && !isnan(wxHum)) {
    if (o.length()) o += SEP;
    o += String((int)roundf(wxHum)) + "% humidity";
  }

  struct tm t;
  if (prayerOk && getNow(t)) {
    int until = 0;
    int idx = findNextPrayer(t.tm_hour * 60 + t.tm_min, until);
    if (o.length()) o += SEP;
    o += String(PRAYER_NAMES[idx]) + " " + fmt12(prayerMin[idx]) + " in " + fmtCountdown(until);
  }

  if (msgCount > 0) {
    if (o.length()) o += SEP;
    if (msgUnread > 0) o += String(msgUnread) + " new message" + (msgUnread == 1 ? "" : "s");
    else               o += String(msgCount) + " message" + (msgCount == 1 ? "" : "s");
  }

  if (locCity.length()) {
    if (o.length()) o += SEP;
    o += locCity;
  }

  if (o.length() == 0) o = "Salah Clock";
  return o;
}

// One scrolling line inside a clipped strip. Short text just sits centred.
void drawTicker(int baseline) {
  static String   text;
  static unsigned long built = 0;

  if (text.length() == 0 || millis() - built > 1000UL) {
    text  = buildTicker();
    built = millis();
  }

  u8g2.setFont(FONT_SMALL);
  int tw = strW(text);

  int cx0 = MARGIN + gOX;
  int cx1 = SCREEN_W - MARGIN + gOX;
  if (cx1 <= 0 || cx0 >= SCREEN_W) return;            // card is off screen
  if (cx0 < 0) cx0 = 0;
  if (cx1 > SCREEN_W) cx1 = SCREEN_W;

  u8g2.setClipWindow(cx0, baseline - 7, cx1, baseline + 2);

  if (tw <= CONTENT_W) {
    pCenter(text, baseline);
  } else {
    const int GAP   = 22;                             // blank run between repeats
    const int SPEED = 24;                             // pixels per second
    int span = tw + GAP;
    int off  = (int)((millis() / 1000.0f * SPEED)) % span;
    pStr(MARGIN - off, baseline, text);
    pStr(MARGIN - off + span, baseline, text);        // second copy makes the wrap seamless
  }

  u8g2.setMaxClipWindow();
}

// ---------------------------------------------------------------
//  Clock face
//    big time            top of the band, PM and seconds beside it
//    date  |  signal     one small row
//    ticker              temperature, next salah, messages, city
// ---------------------------------------------------------------
void drawCardClock(int dx) {
  gOX = dx;

  struct tm t;
  if (!getNow(t)) {
    u8g2.setFont(FONT_BODY);
    pCenter("SYNCING TIME", areaTop() + areaH() / 2);
    u8g2.setFont(FONT_SMALL);
    pCenter(WiFi.status() == WL_CONNECTED ? "contacting NTP..." : "waiting for WiFi",
            areaTop() + areaH() / 2 + 12);
    gOX = 0;
    return;
  }

  char hh[4], mm[4], ss[4], ap[4];
  strftime(hh, sizeof(hh), "%I", &t);
  strftime(mm, sizeof(mm), "%M", &t);
  strftime(ss, sizeof(ss), "%S", &t);
  strftime(ap, sizeof(ap), "%p", &t);

  bool hasTicker = areaH() >= 34;               // three rows only when they fit
  int yScroll = areaBot() - 1;                  // scrolling line
  int yInfo   = hasTicker ? areaBot() - 11 : areaBot() - 1;
  int avail   = yInfo - 9 - areaTop();          // room left for the big numerals

  const uint8_t* bigFont = u8g2_font_logisoso24_tn;
  int fh = 24;
  if (avail < 24) { bigFont = u8g2_font_logisoso20_tn; fh = 20; }
  if (avail < 20) { bigFont = u8g2_font_helvB14_tr;    fh = 14; }

  // ---------- big time, sitting at the top of the band ----------
  u8g2.setFont(bigFont);
  int wHH = u8g2.getStrWidth(hh);
  int wC  = u8g2.getStrWidth(":");
  int wMM = u8g2.getStrWidth(mm);
  int bw  = wHH + wC + wMM;

  u8g2.setFont(FONT_SMALL);
  int side  = max(strW(String(ap)), strW(String(ss)));
  int total = bw + 5 + side;
  int bx = (SCREEN_W - total) / 2;
  if (bx < MARGIN) bx = MARGIN;

  int bBase = areaTop() + fh;

  u8g2.setFont(bigFont);
  pStr(bx, bBase, hh);
  if ((t.tm_sec % 2) == 0) pStr(bx + wHH, bBase, ":");   // colon pulses once a second
  pStr(bx + wHH + wC, bBase, mm);

  u8g2.setFont(FONT_SMALL);
  pStr(bx + bw + 5, bBase - fh + 8, ap);
  if (fh >= 20) pStr(bx + bw + 5, bBase, ss);            // seconds only when there is room

  // ---------- date on the left, signal on the right ----------
  u8g2.setFont(FONT_SMALL);
  char db[16];
  strftime(db, sizeof(db), "%a %d %b", &t);
  String date = String(db);
  date.toUpperCase();
  pStr(MARGIN, yInfo, date);

  bool up = (WiFi.status() == WL_CONNECTED);
  String rs = up ? String(WiFi.RSSI()) : String("--");
  int rw = strW(rs);
  int gx = SCREEN_W - MARGIN - rw - 14;                  // icon first, then the number
  icoWifi(gx, yInfo, wifiBars());
  pStr(SCREEN_W - MARGIN - rw, yInfo, rs);

  // ---------- scrolling info line ----------
  if (hasTicker) drawTicker(yScroll);

  gOX = 0;
}

// =====================================================
// CARD: NEXT PRAYER  (big)
// =====================================================

void drawCardNext(int dx) {
  gOX = dx;

  struct tm t;
  int nowMin = 0;
  bool haveTime = getNow(t);
  if (haveTime) nowMin = t.tm_hour * 60 + t.tm_min;

  u8g2.setFont(FONT_SMALL);

  if (!prayerOk) {
    pStr(MARGIN, yHead(), "NEXT PRAYER");
    pHLine(MARGIN, yRule(), CONTENT_W);
    u8g2.setFont(FONT_BODY);
    pCenter("Times not loaded", areaTop() + areaH() / 2 + 4);
    u8g2.setFont(FONT_SMALL);
    pCenter("check the web panel", areaTop() + areaH() / 2 + 15);
    gOX = 0;
    return;
  }

  int until = 0;
  int idx = findNextPrayer(nowMin, until);

  // header: label left, countdown right
  pStr(MARGIN, yHead(), "NEXT PRAYER");
  String cd = fmtCountdown(until);
  pRight(cd, yHead());
  pHLine(MARGIN, yRule(), CONTENT_W);

  // body: name big, time under it
  int bodyTop = yRule() + 3;
  int bodyH   = areaBot() - bodyTop;

  String name = PRAYER_NAMES[idx];
  name.toUpperCase();

  if (bodyH >= 28) {
    pCenterFit(name, bodyTop + 16, FONTS_BIG, COUNT(FONTS_BIG));
    u8g2.setFont(u8g2_font_helvB10_tr);
    pCenter(fmt12(prayerMin[idx]), areaBot() - 1);
  } else {
    pCenterFit(name, bodyTop + 12, FONTS_TITLE, COUNT(FONTS_TITLE));
    u8g2.setFont(FONT_SMALL);
    pCenter(fmt12(prayerMin[idx]), areaBot() - 1);
  }

  gOX = 0;
}

// =====================================================
// CARD: WEATHER
// =====================================================

void drawCardWeather(int dx) {
  gOX = dx;

  u8g2.setFont(FONT_SMALL);
  pStr(MARGIN, yHead(), "WEATHER");
  String src = wxFromNode ? clip(wxSource, 44) : clip(locCity, 60);
  pRight(src, yHead());
  pHLine(MARGIN, yRule(), CONTENT_W);

  if (!weatherValid()) {
    u8g2.setFont(FONT_BODY);
    pCenter("No reading yet", areaTop() + areaH() / 2 + 4);
    u8g2.setFont(FONT_SMALL);
    pCenter(weatherEnabled ? "waiting for data..." : "weather is off",
            areaTop() + areaH() / 2 + 15);
    gOX = 0;
    return;
  }

  int bodyTop = yRule() + 3;
  int bodyH   = areaBot() - bodyTop;
  bool roomy  = bodyH >= 30;

  String tv = isnan(wxTemp) ? String("--") : String((int)roundf(toDisplayTemp(wxTemp)));
  String hv = isnan(wxHum)  ? String("--") : String((int)roundf(wxHum)) + "%";
  const char* unit = useFahrenheit ? "F" : "C";

  if (roomy) {
    // ---- big temperature: thermometer, digits, degree ring, unit - centred as one block
    u8g2.setFont(u8g2_font_logisoso20_tn);
    int tw = strW(tv);
    u8g2.setFont(FONT_BODY);
    int uw = strW(String(unit));

    const int ICON_W = 5, GAP = 6, DEG_W = 7;
    int total = ICON_W + GAP + tw + DEG_W + uw;
    int x0 = (SCREEN_W - total) / 2;
    if (x0 < MARGIN) x0 = MARGIN;

    int tBase = bodyTop + 19;
    int dx0   = x0 + ICON_W + GAP;

    icoTherm(x0, tBase - 14);
    u8g2.setFont(u8g2_font_logisoso20_tn);
    pStr(dx0, tBase, tv);
    icoDeg(dx0 + tw + 2, tBase - 16, 2);
    u8g2.setFont(FONT_BODY);
    pStr(dx0 + tw + DEG_W, tBase, unit);

    // ---- humidity row: a drop plus the reading
    int hy = areaBot() - 9;
    u8g2.setFont(FONT_BODY);
    int hw = strW(hv);
    int hx = (SCREEN_W - (hw + 11)) / 2;
    if (hx < MARGIN) hx = MARGIN;

    icoDrop(hx, hy);
    pStr(hx + 11, hy + 8, hv);
  } else {
    // ---- tight panels: one centred line "28 C   57%"
    u8g2.setFont(u8g2_font_helvB10_tr);
    int wT = strW(tv);
    int wU = strW(String(unit) + "   ");
    int wH = strW(hv);
    int tot = wT + 6 + wU + wH;
    int x0 = (SCREEN_W - tot) / 2;
    if (x0 < MARGIN) x0 = MARGIN;

    int base = bodyTop + bodyH / 2 + 5;
    pStr(x0, base, tv);
    icoDeg(x0 + wT + 1, base - 8, 2);
    pStr(x0 + wT + 6, base, String(unit) + "   ");
    pStr(x0 + wT + 6 + wU, base, hv);
  }

  gOX = 0;
}

// =====================================================
// CARD: MESSAGE QUEUE
// =====================================================

// Draws one message inside the body box (bodyTop may sit off screen while sliding)
void drawOneMessage(int idx, int bodyTop, int bodyBot) {
  if (idx < 0 || idx >= msgCount) return;

  // sender + age on the first line
  u8g2.setFont(FONT_SMALL);
  int line1 = bodyTop + 7;
  String who = msgs[idx].from;
  if (msgs[idx].src == SRC_NOW) who = "> " + who;
  String age = fmtAge(msgs[idx].ms);
  pStr(MARGIN, line1, clip(who, CONTENT_W - strW(age) - 6));
  pRight(age, line1);

  // body text, up to 2 lines, pinned to the bottom of the card so the
  // last line can never fall past the safe area
  int avail = bodyBot - line1 - 2;
  bool roomy = avail >= 22;
  u8g2.setFont(roomy ? FONT_BODY : FONT_SMALL);

  String lines[MAX_MSG_LINES];
  int n = wrapText(msgs[idx].text, lines, 2, CONTENT_W);
  int step = roomy ? 11 : 9;
  int last = bodyBot - 2;
  if (n == 1) last = line1 + (bodyBot - 2 - line1 + step) / 2;   // a short message sits centred
  for (int i = 0; i < n; i++) pStr(MARGIN, last - (n - 1 - i) * step, lines[i]);
}

void drawCardMessages(int dx, unsigned long el) {
  gOX = dx;

  u8g2.setFont(FONT_SMALL);
  icoMail(MARGIN, yHead() - 7);
  pStr(MARGIN + 12, yHead(), "MESSAGES");

  int bodyTop = yRule() + 3;
  int bodyBot = areaBot();

  if (msgCount == 0) {
    pHLine(MARGIN, yRule(), CONTENT_W);
    u8g2.setFont(FONT_BODY);
    pCenter("Inbox is empty", bodyTop + (bodyBot - bodyTop) / 2);
    u8g2.setFont(FONT_SMALL);
    pCenter("send one from any node", bodyBot - 1);
    gOX = 0;
    return;
  }

  pHLine(MARGIN, yRule(), CONTENT_W);

  // cycle through the queue, newest first, sliding upward
  unsigned long span = MSG_CYCLE_MS * (unsigned long)msgCount;
  unsigned long cyc  = el % span;
  int  i    = (int)(cyc / MSG_CYCLE_MS);
  unsigned long in = cyc % MSG_CYCLE_MS;

  pRight(String(i + 1) + "/" + String(msgCount), yHead());

  int h = bodyBot - bodyTop + 1;
  u8g2.setClipWindow(0, bodyTop, SCREEN_W, bodyBot + 1);

  if (msgCount > 1 && in > MSG_CYCLE_MS - MSG_SLIDE_MS) {
    float p = easeInOut((float)(in - (MSG_CYCLE_MS - MSG_SLIDE_MS)) / (float)MSG_SLIDE_MS);
    int shift = (int)(p * h);
    drawOneMessage(i, bodyTop - shift, bodyBot - shift);
    drawOneMessage((i + 1) % msgCount, bodyTop + h - shift, bodyBot + h - shift);
  } else {
    drawOneMessage(i, bodyTop, bodyBot);
  }

  u8g2.setMaxClipWindow();
  gOX = 0;
}

// =====================================================
// CARD DISPATCH + TRANSITIONS
// =====================================================

void drawCard(int id, int dx, unsigned long el) {
  switch (id) {
    case CARD_CLOCK:   drawCardClock(dx);        break;
    case CARD_NEXT:    drawCardNext(dx);         break;
    case CARD_WEATHER: drawCardWeather(dx);      break;
    case CARD_MSG:     drawCardMessages(dx, el); break;
  }
}

void startTransition(int from, int to, int dir) {
  if (from == to) return;
  trActive = true;
  trFrom   = from;
  trTo     = to;
  trDir    = dir;
  trStart  = millis();
  curCard  = to;
  cardStart = millis();
}

// true while a transition is still running
bool drawTransition() {
  unsigned long el = millis() - trStart;
  if (el >= CARD_SLIDE_MS) {
    trActive = false;
    return false;
  }
  float p = easeInOut((float)el / (float)CARD_SLIDE_MS);
  int shift = (int)(p * SCREEN_W);

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  drawCard(trFrom, -shift * trDir, el);
  drawCard(trTo, (SCREEN_W - shift) * trDir, 0);
  u8g2.sendBuffer();
  return true;
}

// Which cards are worth showing right now
void buildPlaylist() {
  playCount = 0;
  if (showNextPrayer && prayerOk)          playlist[playCount++] = CARD_NEXT;
  if (showWeatherCard && weatherValid())   playlist[playCount++] = CARD_WEATHER;
  if (showMsgCard && customMessages && msgCount > 0) playlist[playCount++] = CARD_MSG;
  playIndex = 0;
}

// =====================================================
// PRAYER ALERT SCREENS
// =====================================================

bool alertSpecial() {
  return (alertPrayer == 0 && fajrReminder) || (alertPrayer == 3 && maghribReminder);
}

void drawBottomTime(int y) {
  struct tm t;
  if (!getNow(t)) return;
  char b[16];
  strftime(b, sizeof(b), "%I:%M:%S %p", &t);
  u8g2.setFont(FONT_SMALL);
  pCenter(String(b), y);
}

// Phase 1: pulsing "TIME FOR SALAH"
void drawAlertBlink(unsigned long el) {
  bool inv = blinkDisplay && ((el / 400UL) % 2 == 0);

  u8g2.clearBuffer();
  if (inv) {
    u8g2.setDrawColor(1);
    u8g2.drawBox(0, 0, SCREEN_W, SCREEN_H);
    u8g2.setDrawColor(0);
  } else {
    u8g2.setDrawColor(1);
  }

  u8g2.setFont(FONT_SMALL);
  pCenter("TIME FOR SALAH", yHead());
  pHLine(MARGIN, yRule(), CONTENT_W);

  String name = PRAYER_NAMES[alertPrayer];
  name.toUpperCase();

  int bodyTop = yRule() + 3;
  int bodyH   = areaBot() - bodyTop;
  if (bodyH >= 30) pCenterFit(name, bodyTop + 16, FONTS_BIG, COUNT(FONTS_BIG));
  else             pCenterFit(name, bodyTop + 12, FONTS_TITLE, COUNT(FONTS_TITLE));

  drawBottomTime(areaBot() - 1);

  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

// 23 px tall so it fits the scene even on a deeply trimmed panel
void drawMosque(int cx, int groundY) {
  u8g2.drawBox(cx - 10, groundY - 7, 20, 7);                                       // hall
  u8g2.drawDisc(cx, groundY - 7, 7, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT); // dome
  u8g2.drawVLine(cx, groundY - 17, 3);                                             // dome tip
  u8g2.drawBox(cx + 13, groundY - 18, 3, 18);                                      // minaret
  u8g2.drawTriangle(cx + 12, groundY - 18, cx + 17, groundY - 18, cx + 14, groundY - 23);
}

// Phase 2: sunrise (Fajr) or sunset (Maghrib)
void drawSkyScene(bool sunrise, unsigned long el) {
  int ground = areaBot() - 1;          // the scene uses the full band, no clock line
  int skyTop = yRule() + 2;
  if (ground < skyTop + 14) ground = skyTop + 14;

  const int sunX = 32;
  static const int8_t starDX[6] = { 14, 52, 62, 100, 118, 44 };
  static const int8_t starDY[6] = {  2,  0,  9,   0,  12,  16 };

  float t = (float)el / 7000.0f;
  if (t > 1.0f) t = 1.0f;
  float e = easeInOut(t);

  int sunLow  = ground + 8;
  int sunHigh = skyTop + 9;
  int sunY = sunrise ? (int)(sunLow - e * (sunLow - sunHigh))
                     : (int)(sunHigh + e * (sunLow - sunHigh));

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // nothing in the scene may spill into the blurry rows
  u8g2.setClipWindow(0, skyTop, SCREEN_W, areaBot() + 1);

  int stars = sunrise ? 6 - (int)(t * 6.5f) : (int)(t * 6.5f);
  stars = constrain(stars, 0, 6);
  for (int i = 0; i < stars; i++) {
    if (((el / 350UL) + i) % 4 != 0) u8g2.drawPixel(starDX[i], skyTop + starDY[i]);
  }

  if (!sunrise && t > 0.35f) icoMoon(104, skyTop);

  u8g2.drawDisc(sunX, sunY, 8);
  if (sunY < ground - 2) {
    float rot = (float)el / 1500.0f;
    for (int k = 0; k < 8; k++) {
      float a = k * 0.7853982f + rot;
      int r2 = 14 + (int)(((el / 300UL) + k) % 2) * 2;
      u8g2.drawLine(sunX + (int)(cosf(a) * 11), sunY + (int)(sinf(a) * 11),
                    sunX + (int)(cosf(a) * r2),  sunY + (int)(sinf(a) * r2));
    }
  }

  // ground hides everything below the horizon
  u8g2.setDrawColor(0);
  u8g2.drawBox(0, ground + 1, SCREEN_W, SCREEN_H - ground - 1);
  u8g2.setDrawColor(1);
  u8g2.drawHLine(MARGIN, ground, CONTENT_W);
  drawMosque(76, ground);

  u8g2.setMaxClipWindow();

  String title = String(alertPrayer == 0 ? "FAJR" : "MAGHRIB") + " TIME";
  u8g2.setFont(FONT_SMALL);
  pCenter(title, yHead());

  u8g2.sendBuffer();
}

// Phase 3: dua reminder (Fajr / Maghrib only)
void drawDuaReminder(unsigned long el) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_SMALL);

  String title = String(alertPrayer == 0 ? "FAJR" : "MAGHRIB") + " TIME";
  pCenter(title, yHead());
  pHLine(MARGIN, yRule(), CONTENT_W);

  int top = yRule() + 3;
  bool withTime = (areaBot() - yRule()) >= 30;
  int bot = areaBot() - (withTime ? 10 : 1);
  bool big = (bot - top) >= 21;
  int l2 = bot;                             // second line sits on the bottom edge
  int l1 = l2 - (big ? 13 : 11);

  // the two lines glide in from the right, one after the other
  int s1 = (int)((1.0f - easeOut((float)el / 450.0f)) * 40.0f);
  int s2 = (int)((1.0f - easeOut((float)max(0L, (long)el - 180L) / 450.0f)) * 40.0f);

  if (big) {
    gOX = s1;
    pCenterFit("Please read your", l1, FONTS_MID, COUNT(FONTS_MID));
    gOX = s2;
    pCenterFit(alertPrayer == 0 ? "morning duas" : "evening duas", l2, FONTS_MID, COUNT(FONTS_MID));
  } else {
    u8g2.setFont(FONT_BODY);
    gOX = s1;
    pCenter("Please read your", l1);
    gOX = s2;
    pCenter(alertPrayer == 0 ? "morning duas" : "evening duas", l2);
  }
  gOX = 0;

  if (withTime) drawBottomTime(areaBot() - 1);
  u8g2.sendBuffer();
}

// Phase 4: steady screen with a Quran verse
void drawAlertVerse(unsigned long el) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  u8g2.setFont(FONT_SMALL);

  String name = PRAYER_NAMES[alertPrayer];
  name.toUpperCase();
  pCenter("TIME FOR " + name, yHead());
  pHLine(MARGIN, yRule(), CONTENT_W);

  int bodyTop = yRule() + 3;
  int bodyBot = areaBot() - 10;
  int bodyH   = bodyBot - bodyTop;

  if (showVerse) {
    int idx = (alertVerse + (int)(el / VERSE_ROTATE_MS)) % VERSE_COUNT;
    u8g2.setFont(FONT_SMALL);

    const int step = 8;
    int maxLines = constrain((bodyH + 3) / step, 1, 3);

    if (idx != cachedVerse || verseLineCount > maxLines) {
      cachedVerse = idx;
      verseLineCount = wrapText(String(VERSES[idx]), verseLines, maxLines, CONTENT_W);
    }

    int block  = verseLineCount * step;
    int startY = bodyTop + (bodyH - block) / 2 + 7;
    if (startY < bodyTop + 7) startY = bodyTop + 7;
    int last = startY + (verseLineCount - 1) * step;
    if (last > bodyBot) startY -= (last - bodyBot);
    for (int i = 0; i < verseLineCount; i++) pCenter(verseLines[i], startY + i * step);
  } else {
    u8g2.setFont(u8g2_font_helvB10_tr);
    pCenter("PLEASE PRAY", bodyTop + bodyH / 2 + 4);
  }

  drawBottomTime(areaBot() - 1);
  u8g2.sendBuffer();
}

void drawAlertScreen(unsigned long el) {
  if (el < BLINK_PHASE_MS) { drawAlertBlink(el); return; }
  el -= BLINK_PHASE_MS;

  if (alertSpecial()) {
    if (el < ANIM_PHASE_MS) { drawSkyScene(alertPrayer == 0, el); return; }
    el -= ANIM_PHASE_MS;
    if (el < DUA_PHASE_MS)  { drawDuaReminder(el); return; }
    el -= DUA_PHASE_MS;
  }
  drawAlertVerse(el);
}

// =====================================================
// SAFE AREA CALIBRATION SCREEN
// =====================================================

void drawCalibration() {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  // ruler down the left edge, a tick every 2 rows and a number every 10
  for (int y = 0; y < SCREEN_H; y += 2) u8g2.drawPixel(0, y);
  u8g2.setFont(FONT_SMALL);
  for (int y = 0; y < SCREEN_H; y += 10) {
    u8g2.drawHLine(0, y, 4);
    if (y + 7 <= SCREEN_H - 1) u8g2.drawStr(6, y + 7, String(y).c_str());
  }

  // the safe area itself
  u8g2.drawFrame(MARGIN - 2, areaTop(), CONTENT_W + 4, areaH());
  u8g2.setFont(FONT_SMALL);
  u8g2.drawStr(MARGIN + 14, areaTop() + 9, "SAFE AREA");
  String s = "top " + String(topSafe) + "  bottom " + String(botSafe);
  u8g2.drawStr(MARGIN + 14, areaTop() + 19, s.c_str());
  u8g2.drawStr(MARGIN + 14, areaBot() - 1, "all text sits here");

  u8g2.sendBuffer();
}

// =====================================================
// SCREEN STATE MACHINE
// =====================================================

void returnToClock() {
  if (curCard != CARD_CLOCK) startTransition(curCard, CARD_CLOCK, -1);
  else                       curCard = CARD_CLOCK;
  mode       = MODE_CLOCK;
  lastSlotMs = millis();
  lastDrawn  = -99;
}

void startAlert(int idx) {
  alertPrayer  = idx;
  alertActive  = true;
  alertStart   = millis();
  alertVerse   = (alertVerse + 1) % VERSE_COUNT;
  cachedVerse  = -1;
  trActive     = false;
  mode         = MODE_ALERT;
  lastDrawn    = -99;
  Serial.printf("ALERT: %s\n", PRAYER_NAMES[idx]);
}

// A message just arrived: jump straight to the message card
void handleNowMessage(const String& text, const String& from) {
  if (!customMessages) return;
  pushMessage(text, from, SRC_NOW);

  if (relayMessages && espNowReady) {
    broadcastMessage(text, from);          // pass it on to the other nodes
  }

  if (popupMessages && !alertActive) {
    trActive = false;
    mode = MODE_CARDS;
    playCount = 1;
    playlist[0] = CARD_MSG;
    playIndex = 0;
    startTransition(curCard, CARD_MSG, 1);
    cardStart = millis();
  }
}

void checkPrayerAlerts() {
  if (!prayerAlerts || !prayerOk || alertActive) return;

  struct tm t;
  if (!getNow(t)) return;

  int nowMin = t.tm_hour * 60 + t.tm_min;
  int dk = dayKey(t);

  for (int i = 0; i < 5; i++) {
    if (!(alertMask & (1 << i))) continue;
    if (alertFiredDay[i] == dk) continue;

    int diff = nowMin - prayerMin[i];
    if (diff >= 0 && diff <= 1) {
      alertFiredDay[i] = dk;
      startAlert(i);
      return;
    }
  }
}

void updateDisplay() {
  unsigned long now = millis();

  // ---- 1. calibration overlay wins over everything except alerts
  if (mode == MODE_CALIB) {
    if (now < calibUntil && !alertActive) {
      if (frameDue(900, 250)) drawCalibration();
      return;
    }
    mode = MODE_CLOCK;
    lastDrawn = -99;
  }

  // ---- 2. prayer alert
  if (alertActive) {
    if (now - alertStart < (unsigned long)alertSeconds * 1000UL) {
      if (frameDue(800, FRAME_MS)) drawAlertScreen(now - alertStart);
      return;
    }
    alertActive = false;
    mode = MODE_CLOCK;
    lastSlotMs = now;
    lastDrawn = -99;
  }

  // ---- 3. card slide in progress
  if (trActive) {
    if (frameDue(700, FRAME_MS)) {
      if (!drawTransition()) lastDrawn = -99;
    }
    return;
  }

  // ---- 4. carousel timing
  if (mode == MODE_CARDS) {
    unsigned long hold = (unsigned long)cardSeconds * 1000UL;
    if (curCard == CARD_MSG) {
      hold = max(hold, (unsigned long)messageSeconds * 1000UL);
      if (msgCount > 1) hold = max(hold, MSG_CYCLE_MS * (unsigned long)min(msgCount, 4));
    }
    if (now - cardStart >= hold) {
      if (curCard == CARD_MSG) markAllRead();      // the queue has been seen
      playIndex++;
      if (playIndex < playCount) startTransition(curCard, playlist[playIndex], 1);
      else                       returnToClock();
      return;
    }
  } else if (mode == MODE_CLOCK) {
    if (now - lastSlotMs >= (unsigned long)slotMinutes * 60000UL) {
      buildPlaylist();
      if (playCount > 0) {
        mode = MODE_CARDS;
        startTransition(CARD_CLOCK, playlist[0], 1);
        return;
      }
      lastSlotMs = now;
    }
  }

  // ---- 5. blank screen option
  if (mode == MODE_CLOCK && !showClock) {
    if (lastDrawn != 950) {
      u8g2.clearBuffer();
      u8g2.sendBuffer();
      lastDrawn = 950;
    }
    return;
  }

  // ---- 6. draw the current card
  unsigned long el = now - cardStart;
  unsigned long interval = 200;
  if (curCard == CARD_CLOCK)   interval = 50;    // smooth ticker scroll
  if (curCard == CARD_MSG)     interval = FRAME_MS;
  if (curCard == CARD_WEATHER) interval = 500;
  if (curCard == CARD_NEXT)    interval = 500;

  if (curCard == CARD_CLOCK) {
    struct tm t;
    bool haveTime = getNow(t);
    bool secChanged = haveTime && t.tm_sec != lastSecond;
    if (frameDue(600 + curCard, interval) || secChanged) {
      if (haveTime) lastSecond = t.tm_sec;
      u8g2.clearBuffer();
      u8g2.setDrawColor(1);
      drawCard(CARD_CLOCK, 0, el);
      u8g2.sendBuffer();
    }
    return;
  }

  if (frameDue(600 + curCard, interval)) {
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    drawCard(curCard, 0, el);
    u8g2.sendBuffer();
  }
}

// =====================================================
// WEB PANEL
// =====================================================

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Salah Clock Hub</title>
<style>
:root{--bg:#eef2f5;--card:#fff;--fg:#16222e;--muted:#5f6f7e;--line:#d8e0e6;--band:#10202f;--bandfg:#eaf1f6;--acc:#0f766e;--accfg:#fff;--warn:#b45309;--knob:#fff;--off:#9aa8b4}
@media(prefers-color-scheme:dark){:root{--bg:#0b1218;--card:#111c25;--fg:#e5edf3;--muted:#8a9aa8;--line:#1f2d3a;--band:#0f1b27;--bandfg:#eaf1f6;--acc:#2dd4bf;--accfg:#04201c;--warn:#fbbf24;--off:#4b5b69}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 "Segoe UI",system-ui,-apple-system,Roboto,sans-serif}
.band{background:var(--band);color:var(--bandfg);padding:22px 18px 18px}
.wrap{max-width:560px;margin:0 auto}
.band h1{margin:0;font-size:15px;font-weight:500;opacity:.75}
.time{font-size:44px;font-weight:600;letter-spacing:-.02em;line-height:1.1;margin:6px 0 2px;font-variant-numeric:tabular-nums}
.sub{font-size:13px;opacity:.75}
.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:10px}
.chip{background:rgba(255,255,255,.1);border-radius:999px;padding:3px 10px;font-size:12.5px}
#conn{color:var(--warn);font-size:13px;min-height:18px;margin-top:6px}
main{padding:6px 18px 96px}
section{padding:18px 0 6px;border-bottom:1px solid var(--line)}
section:last-of-type{border-bottom:0}
h2{font-size:17px;margin:0 0 4px}
.hint{color:var(--muted);font-size:13px;margin:0 0 8px}
.pt{display:flex;justify-content:space-between;padding:9px 12px;margin:0 -12px;border-radius:8px;font-variant-numeric:tabular-nums}
.pt.next{background:var(--acc);color:var(--accfg);font-weight:600}
.row{display:flex;align-items:center;justify-content:space-between;gap:14px;padding:11px 0}
.row+.row{border-top:1px solid var(--line)}
.row small{display:block;color:var(--muted);font-size:12.5px;line-height:1.3;margin-top:1px}
.field{display:block;padding:8px 0}
.field span{display:block;font-size:13px;color:var(--muted);margin-bottom:4px}
input[type=text],input[type=number],select,textarea{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:transparent;color:var(--fg);font:inherit}
input[type=number]{width:92px;text-align:right}
input[type=range]{width:100%;accent-color:var(--acc)}
input:disabled{opacity:.5}
select{background:var(--bg)}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
button{font:inherit;font-weight:600;padding:11px 16px;border:0;border-radius:8px;background:var(--acc);color:var(--accfg);cursor:pointer}
button.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}
button.mini{padding:5px 10px;font-size:13px}
button:focus-visible,input:focus-visible,select:focus-visible,textarea:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin:10px 0 12px}
input[type=checkbox]{appearance:none;-webkit-appearance:none;flex:none;width:46px;height:28px;margin:0;border-radius:14px;background:var(--off);position:relative;cursor:pointer;transition:background .15s}
input[type=checkbox]::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:var(--knob);transition:left .15s}
input[type=checkbox]:checked{background:var(--acc)}
input[type=checkbox]:checked::after{left:21px}
.msg{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px;margin-bottom:8px}
.msg .meta{display:flex;justify-content:space-between;gap:8px;font-size:12.5px;color:var(--muted);margin-bottom:3px}
.msg.unread{border-left:3px solid var(--acc)}
.msg .txt{word-break:break-word}
.node{display:flex;justify-content:space-between;gap:10px;padding:8px 0;font-size:14px;border-top:1px solid var(--line)}
.node span:last-child{color:var(--muted);font-size:12.5px;text-align:right;white-space:nowrap}
.savebar{position:fixed;left:0;right:0;bottom:0;padding:12px 18px;background:var(--bg);border-top:1px solid var(--line)}
.savebar button{display:block;width:100%;max-width:524px;margin:0 auto}
#toast{position:fixed;left:50%;bottom:82px;transform:translateX(-50%);background:var(--fg);color:var(--bg);padding:10px 16px;border-radius:8px;font-size:14px;opacity:0;pointer-events:none;transition:opacity .2s;z-index:9}
#toast.show{opacity:1}
</style>
</head>
<body>

<div class="band"><div class="wrap">
  <h1>Salah Clock Hub</h1>
  <div class="time" id="clock">--:--</div>
  <div class="sub" id="date">Connecting to the clock...</div>
  <div class="chips" id="chips"></div>
  <div id="conn"></div>
</div></div>

<main class="wrap">

<section>
  <h2 id="locTitle">Prayer times</h2>
  <p class="hint" id="pnote"></p>
  <div id="plist"></div>
</section>

<section>
  <h2>Messages <span id="mcount" class="hint" style="font-weight:400"></span></h2>
  <p class="hint">The clock keeps the last 15 messages. Nodes can add to this queue over ESP-NOW.</p>
  <label class="field"><span>New message (up to 100 characters)</span><input type="text" id="msg" maxlength="100" placeholder="Dinner is ready"></label>
  <div class="btns">
    <button onclick="sendMsg(0)">Show on clock</button>
    <button class="ghost" onclick="sendMsg(1)">Show and send to nodes</button>
  </div>
  <div class="btns">
    <button class="ghost mini" onclick="act('/api/msgread',{},'Marked as read')">Mark all read</button>
    <button class="ghost mini" onclick="act('/api/clear',{},'Queue cleared')">Clear queue</button>
  </div>
  <div id="mlist"></div>
</section>

<section>
  <h2>Nodes (ESP-NOW)</h2>
  <p class="hint" id="nowinfo"></p>
  <label class="row"><div>ESP-NOW hub<small>Receive from and send to other ESP32 boards</small></div><input type="checkbox" data-k="now"></label>
  <label class="row"><div>Relay node messages<small>Pass a message from one node on to all the others</small></div><input type="checkbox" data-k="relay"></label>
  <label class="row"><div>Share time and prayer times<small>Broadcast a sync packet every minute</small></div><input type="checkbox" data-k="sync"></label>
  <label class="field"><span>Hub name (sent with every packet)</span><input type="text" data-k="hubname" maxlength="15"></label>
  <div id="nlist"></div>
</section>

<section>
  <h2>Screen</h2>
  <label class="row"><div>Show the clock<small>When off, the screen stays blank between slots</small></div><input type="checkbox" data-k="clock"></label>
  <label class="row"><div>Next prayer card</div><input type="checkbox" data-k="next"></label>
  <label class="row"><div>Temperature card</div><input type="checkbox" data-k="wxcard"></label>
  <label class="row"><div>Message card</div><input type="checkbox" data-k="msgcard"></label>
  <label class="row"><div>Run the cards every<small>Minutes between each run (default 3)</small></div><input type="number" data-k="slot" min="1" max="30"></label>
  <label class="row"><div>Seconds per card<small>2 to 10</small></div><input type="number" data-k="cardsec" min="2" max="10"></label>
  <label class="row"><div>Pop up new messages<small>Jump to the message card the moment one arrives</small></div><input type="checkbox" data-k="popup"></label>
  <label class="row"><div>Allow messages at all</div><input type="checkbox" data-k="custom"></label>
  <label class="row"><div>Minimum popup time<small>Seconds (2 to 60)</small></div><input type="number" data-k="msgsec" min="2" max="60"></label>
</section>

<section>
  <h2>Firmware</h2>
  <p class="hint" id="fwnote"></p>
  <label class="row"><div>Update automatically<small>Checks GitHub on every boot and once a day. Installs a newer release on its own.</small></div><input type="checkbox" data-k="ota"></label>
  <div class="btns"><button class="ghost" onclick="act('/api/ota',{},'Checking GitHub for new firmware')">Check for updates now</button></div>
</section>

<section>
  <h2>Panel calibration</h2>
  <p class="hint">Nothing is ever drawn inside these rows, and the rest of the screen is filled evenly between them. Raise "top safe rows" until every line is sharp, and keep a few "bottom safe rows" so no text sits on the bezel.</p>
  <label class="field"><span>Top safe rows: <b id="tsv"></b> px</span><input type="range" data-k="topsafe" min="0" max="28" step="1" oninput="tsv.textContent=this.value"></label>
  <label class="field"><span>Bottom safe rows: <b id="bsv"></b> px</span><input type="range" data-k="botsafe" min="0" max="16" step="1" oninput="bsv.textContent=this.value"></label>
  <label class="field"><span>Brightness: <b id="brv"></b></span><input type="range" data-k="bright" min="10" max="255" step="5" oninput="brv.textContent=this.value"></label>
  <div class="btns"><button class="ghost" onclick="act('/api/calib',{},'Showing the safe area on the clock')">Show safe area on the clock</button></div>
</section>

<section>
  <h2>Weather</h2>
  <p class="hint" id="wxnote"></p>
  <label class="row"><div>Fetch weather online<small>Open-Meteo, no account needed. A sensor node always wins.</small></div><input type="checkbox" data-k="wx"></label>
  <label class="row"><div>Show Fahrenheit</div><input type="checkbox" data-k="fahr"></label>
  <div class="btns"><button class="ghost" onclick="act('/api/refresh',{},'Refreshing')">Refresh now</button></div>
</section>

<section>
  <h2>Prayer alerts</h2>
  <label class="row"><div>Alert at prayer time</div><input type="checkbox" data-k="alerts"></label>
  <label class="row"><div>Blink the display</div><input type="checkbox" data-k="blink"></label>
  <label class="row"><div>Show a Quran verse</div><input type="checkbox" data-k="verse"></label>
  <label class="row"><div>Fajr reminder<small>Sunrise animation, then a reminder to read morning duas</small></div><input type="checkbox" data-k="fajr"></label>
  <label class="row"><div>Maghrib reminder<small>Sunset animation, then a reminder to read evening duas</small></div><input type="checkbox" data-k="maghrib"></label>
  <label class="row"><div>Alert length<small>Seconds (30 to 300)</small></div><input type="number" data-k="alertsec" min="30" max="300"></label>
  <p class="hint" style="margin-top:12px">Alert for these prayers</p>
  <label class="row"><div>Fajr</div><input type="checkbox" data-k="a0"></label>
  <label class="row"><div>Dhuhr</div><input type="checkbox" data-k="a1"></label>
  <label class="row"><div>Asr</div><input type="checkbox" data-k="a2"></label>
  <label class="row"><div>Maghrib</div><input type="checkbox" data-k="a3"></label>
  <label class="row"><div>Isha</div><input type="checkbox" data-k="a4"></label>
</section>

<section>
  <h2>Location and calculation</h2>
  <label class="row"><div>Detect my city automatically<small>Uses your internet address. Turn off to set the place yourself.</small></div><input type="checkbox" data-k="autoloc"></label>
  <label class="field"><span>City name</span><input type="text" data-k="city" maxlength="24"></label>
  <div class="two">
    <label class="field"><span>Latitude</span><input type="text" inputmode="decimal" data-k="lat"></label>
    <label class="field"><span>Longitude</span><input type="text" inputmode="decimal" data-k="lon"></label>
  </div>
  <label class="field"><span>Calculation method</span>
    <select data-k="method">
      <option value="1">University of Islamic Sciences, Karachi</option>
      <option value="2">ISNA (North America)</option>
      <option value="3">Muslim World League</option>
      <option value="4">Umm al-Qura, Makkah</option>
      <option value="5">Egyptian General Authority</option>
      <option value="15">Moonsighting Committee</option>
    </select>
  </label>
  <label class="field"><span>Asr calculation</span>
    <select data-k="school">
      <option value="0">Standard (Shafi, Maliki, Hanbali)</option>
      <option value="1">Hanafi</option>
    </select>
  </label>
</section>

<section>
  <h2>Actions</h2>
  <div class="btns">
    <button onclick="act('/api/refresh',{},'Refreshing location, prayer times and weather')">Refresh everything</button>
    <button class="ghost" onclick="act('/api/dismiss',{},'Alert dismissed')">Dismiss alert</button>
  </div>
  <label class="field"><span>Preview an alert on the clock</span>
    <select id="tp">
      <option value="0">Fajr (sunrise)</option>
      <option value="1">Dhuhr</option>
      <option value="2">Asr</option>
      <option value="3">Maghrib (sunset)</option>
      <option value="4">Isha</option>
    </select>
  </label>
  <div class="btns"><button class="ghost" onclick="act('/api/test',{p:$('#tp').value},'Alert started on the clock')">Preview alert</button></div>
</section>

</main>

<div class="savebar"><button onclick="saveSettings()">Save settings</button></div>
<div id="toast"></div>

<script>
const $=s=>document.querySelector(s);
function toast(m){const t=$('#toast');t.textContent=m;t.classList.add('show');clearTimeout(toast.h);toast.h=setTimeout(()=>t.classList.remove('show'),2200)}
async function post(url,data){
  const r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data||{})});
  let j={};try{j=await r.json()}catch(e){}
  if(!r.ok||j.ok===false)throw new Error(j.error||('Error '+r.status));
  return j;
}
function fill(s){
  document.querySelectorAll('[data-k]').forEach(el=>{
    const v=s[el.dataset.k];if(v===undefined)return;
    if(el.type==='checkbox')el.checked=!!v;else el.value=v;
  });
  $('#tsv').textContent=s.topsafe;$('#bsv').textContent=s.botsafe;$('#brv').textContent=s.bright;
  syncLoc();
}
function read(){
  const d={};
  document.querySelectorAll('[data-k]').forEach(el=>{d[el.dataset.k]=el.type==='checkbox'?(el.checked?'1':'0'):el.value});
  return d;
}
function syncLoc(){
  const a=$('[data-k=autoloc]').checked;
  ['city','lat','lon'].forEach(k=>{$('[data-k='+k+']').disabled=a});
}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
function render(s){
  $('#clock').textContent=s.time;
  $('#date').textContent=s.date;
  const ch=[];
  ch.push(s.wifi?('WiFi '+s.rssi+' dBm'):'WiFi offline');
  if(s.wifi)ch.push('channel '+s.channel);
  ch.push(s.espnow?('mesh on, '+s.nodes.length+' node'+(s.nodes.length==1?'':'s')):'mesh off');
  if(s.temp!==null)ch.push(s.tempDisp+' / '+(s.hum!==null?Math.round(s.hum)+'% RH':'--'));
  ch.push(s.msgCount+' message'+(s.msgCount==1?'':'s'));
  ch.push('fw '+s.fw);
  $('#chips').innerHTML=ch.map(c=>'<span class="chip">'+esc(c)+'</span>').join('');

  $('#locTitle').textContent='Prayer times in '+s.city;
  $('#plist').innerHTML=s.prayerOk
    ?s.prayers.map(p=>'<div class="pt'+(p.name===s.next.name?' next':'')+'"><span>'+esc(p.name)+'</span><span>'+esc(p.time)+'</span></div>').join('')
    :'<p class="hint">Prayer times have not loaded yet. Use Refresh everything below.</p>';
  $('#pnote').textContent=(s.prayerOk&&s.next.name)?('Next: '+s.next.name+' in '+s.next.left):'';

  $('#mcount').textContent=s.msgCount?('('+s.msgCount+' of 15, '+s.msgUnread+' unread)'):'';
  $('#mlist').innerHTML=s.messages.length
    ?s.messages.map((m,i)=>'<div class="msg'+(m.unread?' unread':'')+'"><div class="meta"><span>'+esc(m.from)+(m.src==1?' &middot; node':'')+'</span><span>'+esc(m.age)+' <button class="ghost mini" onclick="delMsg('+i+')">Delete</button></span></div><div class="txt">'+esc(m.text)+'</div></div>').join('')
    :'<p class="hint">No messages yet.</p>';

  $('#nowinfo').textContent=s.espnow
    ?('Hub MAC '+s.mac+' on channel '+s.channel+'. Every node must use this same channel. Received '+s.nowRx+', sent '+s.nowTx+'.')
    :'ESP-NOW is off.';
  $('#nlist').innerHTML=s.nodes.length
    ?s.nodes.map(n=>'<div class="node"><span>'+esc(n.name)+'<br><small style="color:var(--muted)">'+esc(n.mac)+'</small></span><span>'+esc(n.age)+'<br>'+n.rssi+' dBm, '+n.packets+' pkt</span></div>').join('')
    :'<p class="hint">No node has spoken to the hub yet.</p>';

  $('#fwnote').innerHTML='Running <b>'+esc(s.fw)+'</b>'
    +(s.otaLatest?(', latest on GitHub <b>'+esc(s.otaLatest)+'</b>'):'')
    +'. '+esc(s.otaStatus)+'.<br><small style="color:var(--muted)">'+esc(s.otaRepo)+'</small>';
  $('#wxnote').textContent=s.temp!==null
    ?('Now: '+s.tempDisp+(s.hum!==null?', '+Math.round(s.hum)+'% humidity':'')+' from '+s.wxSource+'.')
    :'No reading yet.';
}
async function load(first){
  try{
    const r=await fetch('/api/state',{cache:'no-store'});
    const s=await r.json();
    render(s);
    if(first)fill(s.settings);
    $('#conn').textContent='';
  }catch(e){$('#conn').textContent='Cannot reach the clock. Retrying...'}
}
async function saveSettings(){
  try{await post('/api/settings',read());toast('Settings saved');load(true)}
  catch(e){toast(e.message)}
}
async function sendMsg(share){
  const m=$('#msg').value.trim();
  if(!m){toast('Type a message first');return}
  try{await post('/api/message',{message:m,share:share?'1':'0'});toast(share?'Sent to clock and nodes':'Sent to clock');$('#msg').value='';load(false)}
  catch(e){toast(e.message)}
}
async function delMsg(i){
  try{await post('/api/msgdel',{i:i});load(false)}catch(e){toast(e.message)}
}
async function act(url,data,ok){
  try{await post(url,data);toast(ok);setTimeout(()=>load(false),700)}
  catch(e){toast(e.message)}
}
$('[data-k=autoloc]').addEventListener('change',syncLoc);
load(true);
setInterval(()=>load(false),4000);
</script>
</body>
</html>
)rawliteral";

void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendOk() {
  sendJson(200, "{\"ok\":true}");
}

void sendError(int code, const char* msg) {
  sendJson(code, String("{\"ok\":false,\"error\":\"") + msg + "\"}");
}

bool argBool(const char* name, bool cur) {
  if (!server.hasArg(name)) return cur;
  String v = server.arg(name);
  return v == "1" || v == "true" || v == "on";
}

int argInt(const char* name, int cur, int lo, int hi) {
  if (!server.hasArg(name)) return cur;
  return constrain((int)server.arg(name).toInt(), lo, hi);
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", MAIN_PAGE);
}

void handleState() {
  String o;
  o.reserve(4096);
  o += '{';

  struct tm t;
  bool haveTime = getNow(t);
  char b[24];

  if (haveTime) {
    strftime(b, sizeof(b), "%I:%M %p", &t);
    jStr(o, "time", String(b));
    strftime(b, sizeof(b), "%a, %d %b %Y", &t);
    jStr(o, "date", String(b));
  } else {
    jStr(o, "time", "--:--");
    jStr(o, "date", "Waiting for internet time...");
  }

  jBool(o, "wifi", WiFi.status() == WL_CONNECTED);
  jStr(o, "ssid", WiFi.SSID());
  jStr(o, "ip", WiFi.localIP().toString());
  jStr(o, "mac", WiFi.macAddress());
  jNum(o, "rssi", WiFi.RSSI());
  jNum(o, "channel", WiFi.channel());
  jNum(o, "uptime", (long)(millis() / 1000UL));
  jBool(o, "prayerOk", prayerOk);
  jBool(o, "alertActive", alertActive);
  jBool(o, "ipDone", ipLookupDone);
  jBool(o, "espnow", espNowReady);
  jStr(o, "fw", FW_VERSION);
  jStr(o, "otaLatest", otaLatest);
  jStr(o, "otaStatus", otaStatus);
  jStr(o, "otaRepo", OTA_REPO);
  jNum(o, "nowRx", (long)nowRxCount);
  jNum(o, "nowTx", (long)nowTxCount);
  jNum(o, "nowDrop", (long)nowDropCount);
  jStr(o, "city", locCity);
  jNum(o, "msgCount", msgCount);
  jNum(o, "msgUnread", msgUnread);

  bool wxOk = weatherValid();
  jFloat(o, "temp", wxOk ? wxTemp : NAN, 1);
  jFloat(o, "hum",  wxOk ? wxHum  : NAN, 0);
  jStr(o, "tempDisp", wxOk && !isnan(wxTemp)
        ? (String((int)roundf(toDisplayTemp(wxTemp))) + (useFahrenheit ? " F" : " C"))
        : String("--"));
  jStr(o, "wxSource", wxOk ? (wxFromNode ? ("node " + wxSource) : String("the internet")) : String("-"));

  // next prayer
  String nName = "", nTime = "", nLeft = "";
  if (prayerOk && haveTime) {
    int until = 0;
    int idx = findNextPrayer(t.tm_hour * 60 + t.tm_min, until);
    nName = PRAYER_NAMES[idx];
    nTime = fmt12(prayerMin[idx]);
    nLeft = fmtCountdown(until);
  }
  o += "\"next\":{";
  jStr(o, "name", nName);
  jStr(o, "time", nTime);
  jStr(o, "left", nLeft);
  jEnd(o);
  o += "},";

  // today's prayers
  o += "\"prayers\":[";
  for (int i = 0; i < 5; i++) {
    o += '{';
    jStr(o, "name", PRAYER_NAMES[i]);
    jStr(o, "time", prayerOk ? fmt12(prayerMin[i]) : String("--"));
    jEnd(o);
    o += "},";
  }
  jEnd(o);
  o += "],";

  // message queue
  o += "\"messages\":[";
  for (int i = 0; i < msgCount; i++) {
    o += '{';
    jStr(o, "text", msgs[i].text);
    jStr(o, "from", msgs[i].from);
    jStr(o, "age",  fmtAge(msgs[i].ms));
    jNum(o, "src",  msgs[i].src);
    jBool(o, "unread", msgs[i].unread);
    jEnd(o);
    o += "},";
  }
  jEnd(o);
  o += "],";

  // nodes
  o += "\"nodes\":[";
  for (int i = 0; i < MAX_NODES; i++) {
    if (!nodes[i].used) continue;
    o += '{';
    jStr(o, "name", String(nodes[i].name));
    jStr(o, "mac",  macStr(nodes[i].mac));
    jStr(o, "age",  fmtAge(nodes[i].lastMs));
    jNum(o, "rssi", nodes[i].rssi);
    jNum(o, "packets", (long)nodes[i].packets);
    jEnd(o);
    o += "},";
  }
  jEnd(o);
  o += "],";

  // settings
  o += "\"settings\":{";
  jBool(o, "clock",   showClock);
  jBool(o, "next",    showNextPrayer);
  jBool(o, "wxcard",  showWeatherCard);
  jBool(o, "msgcard", showMsgCard);
  jBool(o, "verse",   showVerse);
  jBool(o, "alerts",  prayerAlerts);
  jBool(o, "blink",   blinkDisplay);
  jBool(o, "fajr",    fajrReminder);
  jBool(o, "maghrib", maghribReminder);
  jBool(o, "custom",  customMessages);
  jBool(o, "popup",   popupMessages);
  jBool(o, "autoloc", useIpLocation);
  jBool(o, "wx",      weatherEnabled);
  jBool(o, "fahr",    useFahrenheit);
  jBool(o, "now",     espNowEnabled);
  jBool(o, "relay",   relayMessages);
  jBool(o, "sync",    shareSync);
  jBool(o, "ota",     otaAuto);
  jBool(o, "a0", alertMask & 1);
  jBool(o, "a1", alertMask & 2);
  jBool(o, "a2", alertMask & 4);
  jBool(o, "a3", alertMask & 8);
  jBool(o, "a4", alertMask & 16);
  jNum(o, "slot",     slotMinutes);
  jNum(o, "cardsec",  cardSeconds);
  jNum(o, "msgsec",   messageSeconds);
  jNum(o, "alertsec", alertSeconds);
  jNum(o, "method",   prayerMethod);
  jNum(o, "school",   asrSchool);
  jNum(o, "topsafe",  topSafe);
  jNum(o, "botsafe",  botSafe);
  jNum(o, "bright",   brightness);
  jStr(o, "hubname",  hubName);
  jStr(o, "city", locCity);
  jFloat(o, "lat", locLat);
  jFloat(o, "lon", locLon);
  jEnd(o);
  o += "}}";

  sendJson(200, o);
}

void handleSettings() {
  showClock       = argBool("clock",   showClock);
  showNextPrayer  = argBool("next",    showNextPrayer);
  showWeatherCard = argBool("wxcard",  showWeatherCard);
  showMsgCard     = argBool("msgcard", showMsgCard);
  showVerse       = argBool("verse",   showVerse);
  prayerAlerts    = argBool("alerts",  prayerAlerts);
  blinkDisplay    = argBool("blink",   blinkDisplay);
  fajrReminder    = argBool("fajr",    fajrReminder);
  maghribReminder = argBool("maghrib", maghribReminder);
  customMessages  = argBool("custom",  customMessages);
  popupMessages   = argBool("popup",   popupMessages);
  relayMessages   = argBool("relay",   relayMessages);
  shareSync       = argBool("sync",    shareSync);
  otaAuto         = argBool("ota",     otaAuto);

  bool wasWx = weatherEnabled;
  weatherEnabled = argBool("wx", weatherEnabled);
  useFahrenheit  = argBool("fahr", useFahrenheit);
  if (weatherEnabled && !wasWx) nextWxTry = 0;

  bool wasNow = espNowEnabled;
  espNowEnabled = argBool("now", espNowEnabled);
  if (!espNowEnabled && wasNow) stopEspNow();

  if (server.hasArg("hubname")) {
    String h = server.arg("hubname");
    h.trim();
    if (h.length() > 15) h = h.substring(0, 15);
    if (h.length() > 0) hubName = h;
  }

  for (int i = 0; i < 5; i++) {
    char key[3] = { 'a', (char)('0' + i), 0 };
    if (server.hasArg(key)) {
      bool on = argBool(key, (alertMask >> i) & 1);
      if (on) alertMask |= (1 << i);
      else    alertMask &= ~(1 << i);
    }
  }

  slotMinutes    = argInt("slot",     slotMinutes,    1, 30);
  cardSeconds    = argInt("cardsec",  cardSeconds,    2, 10);
  messageSeconds = argInt("msgsec",   messageSeconds, 2, 60);
  alertSeconds   = argInt("alertsec", alertSeconds,   30, 300);
  topSafe        = argInt("topsafe",  topSafe,        0, 28);
  botSafe        = argInt("botsafe",  botSafe,        0, 16);

  int newBright = argInt("bright", brightness, 10, 255);
  if (newBright != brightness) {
    brightness = newBright;
    applyBrightness();
  }

  bool calcChanged = false;
  int newMethod = argInt("method", prayerMethod, 0, 15);
  int newSchool = argInt("school", asrSchool, 0, 1);
  if (newMethod != prayerMethod || newSchool != asrSchool) calcChanged = true;
  prayerMethod = newMethod;
  asrSchool    = newSchool;

  // location
  bool newAuto = argBool("autoloc", useIpLocation);
  bool locChanged = false;

  if (!newAuto) {
    if (server.hasArg("city")) {
      String c = server.arg("city");
      c.trim();
      if (c.length() > 24) c = c.substring(0, 24);
      if (c.length() > 0 && c != locCity) { locCity = c; locChanged = true; }
    }
    if (server.hasArg("lat") && server.hasArg("lon")) {
      float la = server.arg("lat").toFloat();
      float lo = server.arg("lon").toFloat();
      bool valid = la >= -90 && la <= 90 && lo >= -180 && lo <= 180 && !(la == 0 && lo == 0);
      if (valid && (fabsf(la - locLat) > 0.0001f || fabsf(lo - locLon) > 0.0001f)) {
        locLat = la;
        locLon = lo;
        locChanged = true;
      }
    }
  }

  if (newAuto && !useIpLocation) {
    ipLookupDone = false;
    nextLocTry = 0;
  }
  useIpLocation = newAuto;

  if (calcChanged || locChanged) {
    prayerRefreshRequested = true;
    nextPrayerTry = 0;
    nextWxTry     = 0;
  }

  lastDrawn = -99;          // layout may have moved, force a redraw
  saveSettings();
  sendOk();
}

void handleMessage() {
  if (!customMessages) {
    sendError(403, "Messages are turned off. Enable them and save settings first.");
    return;
  }

  String m = server.arg("message");
  m.replace("\r", " ");
  m.replace("\n", " ");
  m.trim();

  if (m.length() == 0) {
    sendError(400, "Message is empty.");
    return;
  }
  if (m.length() > MSG_MAXLEN) m = m.substring(0, MSG_MAXLEN);

  String from = server.hasArg("from") ? server.arg("from") : String("panel");
  pushMessage(m, from, SRC_WEB);

  if (argBool("share", false)) broadcastMessage(m, hubName);

  if (popupMessages && !alertActive) {
    trActive = false;
    mode = MODE_CARDS;
    playCount = 1;
    playlist[0] = CARD_MSG;
    playIndex = 0;
    startTransition(curCard, CARD_MSG, 1);
  }

  sendOk();
}

// Broadcast only, without adding it to the local queue
void handleBroadcast() {
  String m = server.arg("message");
  m.trim();
  if (m.length() == 0) { sendError(400, "Message is empty."); return; }
  if (m.length() > MSG_MAXLEN) m = m.substring(0, MSG_MAXLEN);
  if (!espNowReady)     { sendError(503, "ESP-NOW is not running."); return; }
  broadcastMessage(m, hubName);
  sendOk();
}

void handleClear() {
  clearMessages();
  if (mode == MODE_CARDS && curCard == CARD_MSG) returnToClock();
  sendOk();
}

void handleMsgRead() {
  markAllRead();
  sendOk();
}

void handleMsgDel() {
  int i = argInt("i", -1, 0, MAX_MSGS - 1);
  if (i < 0 || i >= msgCount) { sendError(400, "No such message."); return; }
  deleteMessage(i);
  if (msgCount == 0 && mode == MODE_CARDS && curCard == CARD_MSG) returnToClock();
  sendOk();
}

void handleRefresh() {
  prayerRefreshRequested = true;
  nextPrayerTry = 0;
  nextWxTry     = 0;
  if (useIpLocation) {
    ipLookupDone = false;
    nextLocTry = 0;
  }
  sendOk();
}

void handleTest() {
  int p = argInt("p", 0, 0, 4);
  startAlert(p);
  sendOk();
}

void handleDismiss() {
  if (alertActive) {
    alertActive = false;
    returnToClock();
  }
  sendOk();
}

void handleOta() {
  if (WiFi.status() != WL_CONNECTED) { sendError(503, "The clock is offline."); return; }
  otaRequested = true;                 // the loop does the work, this reply goes out first
  sendOk();
}

void handleCalib() {
  mode = MODE_CALIB;
  calibUntil = millis() + 15000UL;
  lastDrawn = -99;
  sendOk();
}

void setupWebServer() {
  server.on("/",              HTTP_GET,  handleRoot);
  server.on("/api/state",     HTTP_GET,  handleState);
  server.on("/api/settings",  HTTP_POST, handleSettings);
  server.on("/api/message",   HTTP_POST, handleMessage);
  server.on("/api/broadcast", HTTP_POST, handleBroadcast);
  server.on("/api/clear",     HTTP_POST, handleClear);
  server.on("/api/msgread",   HTTP_POST, handleMsgRead);
  server.on("/api/msgdel",    HTTP_POST, handleMsgDel);
  server.on("/api/refresh",   HTTP_POST, handleRefresh);
  server.on("/api/test",      HTTP_POST, handleTest);
  server.on("/api/dismiss",   HTTP_POST, handleDismiss);
  server.on("/api/calib",     HTTP_POST, handleCalib);
  server.on("/api/ota",       HTTP_POST, handleOta);

  // handy for other services: http://salah-clock.local/api/message?message=Hi
  server.on("/api/message", HTTP_GET, handleMessage);

  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });

  server.begin();
  Serial.println("Web server started.");
}

// =====================================================
// SETUP + LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  memset(nodes, 0, sizeof(nodes));

  Wire.begin(OLED_SDA, OLED_SCL);
  u8g2.begin();
  u8g2.setBusClock(400000);

  loadSettings();
  applyBrightness();

  wakeUpAnimation();
  runBootSequence();
  setupWebServer();

  lastSlotMs = millis();
  cardStart  = millis();
  mode       = MODE_CLOCK;
  curCard    = CARD_CLOCK;
  lastDrawn  = -99;

  Serial.println("================================");
  Serial.printf(" SMART SALAH CLOCK HUB %s READY\n", FW_VERSION);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(" Web panel : http://");
    Serial.println(WiFi.localIP());
    Serial.printf(" ESP-NOW   : channel %d, MAC %s\n", WiFi.channel(), WiFi.macAddress().c_str());
  }
  Serial.println("================================");
}

void loop() {
  server.handleClient();

  maintainWiFi();
  maintainTime();
  maintainData();
  maintainEspNow();
  maintainOta();

  checkPrayerAlerts();
  updateDisplay();

  delay(2);
}
