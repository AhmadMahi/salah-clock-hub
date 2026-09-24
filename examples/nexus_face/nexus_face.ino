/*
  ================================================================
   NEXUS FACE  -  ESP32-C3 companion
  ================================================================
   You drive the whole thing by knocking on it.

     one knock ..... next
     two knocks .... go in
     three knocks .. back out

   SCREENS, in the order one knock walks through them
     CLOCK     real time, once it has the internet. Until then it
               keeps itself busy with a passing thought.
     WEATHER   temperature, conditions, humidity and wind
     MESSAGES  whatever you sent it, text steered by tilt
     FACE      animated eyes that react to you
     SETTINGS  two knocks to go in, one to pick, two to open an
               item, one to change it, three to come back out
     HOME      the start screen: uptime, counters, address

   Leave it alone for 30 seconds and the panel powers down and the
   processor throttles back. Pick it up, shake it or knock it and it
   comes straight back.

   NETWORK
     It joins your WiFi for the time, and runs its own hotspot at
     the same time so the panel is always reachable.
       hotspot   NEXUS-ROBOT  /  password
       panel     http://192.168.4.1  or its address on your network

   WIRING   everything on one I2C bus
     SDA GPIO8   SCL GPIO9
     OLED 0x3C   ADXL345 0x53   MPU6050 0x68
   Both sensors are optional; it uses whichever answers.

   LIBRARIES   FluxGarage RoboEyes, Adafruit GFX, Adafruit SSD1306
   Board       ESP32-C3.  Partition: Minimal SPIFFS (1.9MB APP)
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

#define SDA_PIN 8
#define SCL_PIN 9
#define OLED_ADDR 0x3C
// NB: RoboEyes defines SCRW and SCRH for west and its own use, so the
// screen size lives under different names.
#define SCRW 128
#define SCRH 64

#define DEF_WIFI_SSID "YOUR_WIFI_NAME"
#define DEF_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define DEF_TZ        "IST-5:30"          // India. See the panel to change.

const char* AP_SSID = "NEXUS-ROBOT";
const char* AP_PASS = "password";

Adafruit_SSD1306 oled(SCRW, SCRH, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(oled);
WebServer   web(80);
Preferences prefs;

// ---------------- sensors ----------------
#define A_DEVID 0x00
#define A_THRESH_TAP 0x1D
#define A_THRESH_FF 0x28
#define A_TIME_FF 0x29
#define A_DUR 0x21
#define A_LATENT 0x22
#define A_WINDOW 0x23
#define A_TAP_AXES 0x2A
#define A_POWER_CTL 0x2D
#define A_INT_ENABLE 0x2E
#define A_INT_SOURCE 0x30
#define A_DATA_FORMAT 0x31
#define A_DATAX0 0x32
#define INT_TAP1 0x40
#define INT_TAP2 0x20
#define INT_FF 0x04

#define M_WHOAMI 0x75
#define M_PWR1 0x6B
#define M_GYRO_CFG 0x1B
#define M_ACC_CFG 0x1C
#define M_ACCEL_H 0x3B

uint8_t adxl = 0, mpu = 0;
float ax, ay, az, amag = 1;
float mx, my, mz, gx, gy, gz, mtemp;

// ---------------- state ----------------
enum { SCR_CLOCK = 0, SCR_WEATHER, SCR_MSG, SCR_FACE, SCR_SETTINGS, SCR_HOME, SCR_COUNT };
const char* SCR_NAME[SCR_COUNT] = { "CLOCK", "WEATHER", "MESSAGES", "FACE", "SETTINGS", "HOME" };

// depth 0 = walking the screens, 1 = picking a setting, 2 = changing it
int app = SCR_CLOCK, navDepth = 0, setIdx = 0;

enum { SET_BRIGHT = 0, SET_SLEEP, SET_EYES, SET_WIFI, SET_REBOOT, SET_COUNT };
const char* SET_NAME[SET_COUNT] = { "BRIGHTNESS", "SLEEP AFTER", "EYE STYLE", "NETWORK", "REBOOT" };

struct EyeStyle { const char* name; byte w, h, r; int gap; bool cyc; byte mood; };
const EyeStyle STYLES[] = {
  { "round",   36, 36, 10, 12, false, DEFAULT },
  { "square",  38, 38,  2, 10, false, DEFAULT },
  { "wide",    48, 28, 12,  8, false, DEFAULT },
  { "sleepy",  36, 14,  6, 12, false, TIRED   },
  { "cross",   34, 34,  8, 14, false, ANGRY   },
  { "joy",     36, 36, 16, 12, false, HAPPY   },
  { "cyclops", 46, 46, 14,  0, true,  DEFAULT },
};
const int STYLE_COUNT = sizeof(STYLES) / sizeof(STYLES[0]);
bool asleep = false, screenOn = true, timeOk = false;
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, lastTiltStep = 0;
unsigned long reactUntil = 0, lastShake = 0;

uint32_t cTap = 0, cDbl = 0, cFall = 0, cShake = 0, cMsg = 0, cBoot = 0;
String   inbox[4];
int      inboxN = 0;

int  cfgSleepSec = 30;
int  cfgBright   = 160;         // OLED contrast, 10..255
int  cfgEyeStyle = 0;
String cfgSsid, cfgPass, cfgTz;

// ---- weather ----
float wTemp = NAN, wHum = NAN, wWind = NAN;
int   wCode = -1;
String wCity = "";
unsigned long nextWx = 0;
bool  wxOk = false;

// ---- knock counting: the ADXL only reports single taps, we group
//      them into one, two or three ourselves ----
uint8_t  tapBurst = 0;
unsigned long tapFirst = 0;
#define TAP_WINDOW_MS 480

// ---- something to say while it has no clock ----
const char* const QUIPS[] = {
  "waiting for the clock",
  "counting electrons",
  "i know nothing yet",
  "time is a construct",
  "ask me again shortly",
  "still booting my brain",
  "no signal, no idea",
};
const int QUIP_COUNT = sizeof(QUIPS) / sizeof(QUIPS[0]);

#define TILT 0.35f
#define SHAKE_G 0.60f
#define STEP_MS 380

// ================================================================
//  I2C
// ================================================================
static inline void wReg(uint8_t a, uint8_t r, uint8_t v) {
  Wire.beginTransmission(a); Wire.write(r); Wire.write(v); Wire.endTransmission();
}
static uint8_t rReg(uint8_t a, uint8_t r) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false)) return 0;
  if (Wire.requestFrom((int)a, 1) != 1) return 0;
  return Wire.read();
}
static bool rBlk(uint8_t a, uint8_t r, uint8_t* b, uint8_t n) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false)) return false;
  if (Wire.requestFrom((int)a, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) b[i] = Wire.read();
  return true;
}

void startSensors() {
  uint8_t t[2] = { 0x53, 0x1D };
  for (int i = 0; i < 2 && !adxl; i++) {
    if (rReg(t[i], A_DEVID) != 0xE5) continue;
    adxl = t[i];
    wReg(adxl, A_DATA_FORMAT, 0x0B);
    wReg(adxl, A_THRESH_TAP, 0x28); wReg(adxl, A_DUR, 0x10);
    wReg(adxl, A_LATENT, 0x50);     wReg(adxl, A_WINDOW, 0xF0);
    wReg(adxl, A_TAP_AXES, 0x07);
    wReg(adxl, A_THRESH_FF, 0x07);  wReg(adxl, A_TIME_FF, 0x14);
    wReg(adxl, A_INT_ENABLE, INT_TAP1 | INT_TAP2 | INT_FF);
    wReg(adxl, A_POWER_CTL, 0x08);
    delay(20); rReg(adxl, A_INT_SOURCE);
  }
  uint8_t m[2] = { 0x68, 0x69 };
  for (int i = 0; i < 2 && !mpu; i++) {
    uint8_t who = rReg(m[i], M_WHOAMI);
    if (who != 0x68 && who != 0x69 && who != 0x70 && who != 0x71 && who != 0x98) continue;
    mpu = m[i];
    wReg(mpu, M_PWR1, 0x00); delay(10);
    wReg(mpu, M_GYRO_CFG, 0x00); wReg(mpu, M_ACC_CFG, 0x00);
  }
  Serial.printf("ADXL345 %s   MPU6050 %s\n",
                adxl ? "ok" : "absent", mpu ? "ok" : "absent");
}

void readSensors() {
  uint8_t b[14];
  if (adxl && rBlk(adxl, A_DATAX0, b, 6)) {
    ax = (int16_t)((b[1] << 8) | b[0]) / 256.0f;
    ay = (int16_t)((b[3] << 8) | b[2]) / 256.0f;
    az = (int16_t)((b[5] << 8) | b[4]) / 256.0f;
    amag = sqrtf(ax * ax + ay * ay + az * az);
  }
  if (mpu && rBlk(mpu, M_ACCEL_H, b, 14)) {
    mx = (int16_t)((b[0] << 8) | b[1]) / 16384.0f;
    my = (int16_t)((b[2] << 8) | b[3]) / 16384.0f;
    mz = (int16_t)((b[4] << 8) | b[5]) / 16384.0f;
    mtemp = (int16_t)((b[6] << 8) | b[7]) / 340.0f + 36.53f;
    gx = (int16_t)((b[8] << 8) | b[9]) / 131.0f;
    gy = (int16_t)((b[10] << 8) | b[11]) / 131.0f;
    gz = (int16_t)((b[12] << 8) | b[13]) / 131.0f;
    if (!adxl) { ax = mx; ay = my; az = mz; amag = sqrtf(mx*mx + my*my + mz*mz); }
  }
}

// ================================================================
//  HUD DRAWING
// ================================================================
static void ctr(const char* s, int y, int size) {
  int w = (int)strlen(s) * 6 * size;
  oled.setTextSize(size);
  oled.setCursor(w < SCRW ? (SCRW - w) / 2 : 0, y);
  oled.print(s);
}

// Corner brackets. Cheap, and it makes everything look deliberate.
static void hud() {
  const int L = 6;
  oled.drawFastHLine(0, 0, L, SSD1306_WHITE);      oled.drawFastVLine(0, 0, L, SSD1306_WHITE);
  oled.drawFastHLine(SCRW - L, 0, L, SSD1306_WHITE);  oled.drawFastVLine(SCRW - 1, 0, L, SSD1306_WHITE);
  oled.drawFastHLine(0, SCRH - 1, L, SSD1306_WHITE);  oled.drawFastVLine(0, SCRH - L, L, SSD1306_WHITE);
  oled.drawFastHLine(SCRW - L, SCRH - 1, L, SSD1306_WHITE); oled.drawFastVLine(SCRW - 1, SCRH - L, L, SSD1306_WHITE);
}

static void clockStr(char* out, size_t n, bool withSec) {
  struct tm t;
  if (!timeOk || !getLocalTime(&t, 5)) { snprintf(out, n, withSec ? "--:--:--" : "--:--"); return; }
  if (withSec) snprintf(out, n, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  else         snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

// top strip: time on the left, wifi bars on the right
static void statusBar() {
  char t[10];
  clockStr(t, sizeof(t), false);
  oled.setTextSize(1);
  oled.setCursor(8, 2);
  oled.print(t);

  int bars = 0;
  if (WiFi.status() == WL_CONNECTED) {
    int r = WiFi.RSSI();
    bars = r > -55 ? 4 : r > -67 ? 3 : r > -78 ? 2 : 1;
  }
  for (int i = 0; i < 4; i++) {
    int h = 2 + i * 2, x = SCRW - 20 + i * 3;
    if (i < bars) oled.fillRect(x, 10 - h, 2, h, SSD1306_WHITE);
    else          oled.drawPixel(x, 9, SSD1306_WHITE);
  }
  oled.drawFastHLine(6, 12, SCRW - 12, SSD1306_WHITE);
}

// ---- weather glyphs ----
static void wxSun(int x, int y) {
  oled.fillCircle(x + 9, y + 9, 5, SSD1306_WHITE);
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    oled.drawLine(x + 9 + cosf(a) * 7, y + 9 + sinf(a) * 7,
                  x + 9 + cosf(a) * 9, y + 9 + sinf(a) * 9, SSD1306_WHITE);
  }
}
static void wxCloud(int x, int y) {
  oled.fillCircle(x + 6, y + 11, 4, SSD1306_WHITE);
  oled.fillCircle(x + 11, y + 9, 5, SSD1306_WHITE);
  oled.fillRect(x + 6, y + 10, 8, 5, SSD1306_WHITE);
}
static void wxRain(int x, int y) {
  wxCloud(x, y - 3);
  for (int i = 0; i < 3; i++) oled.drawLine(x + 4 + i * 4, y + 13, x + 3 + i * 4, y + 17, SSD1306_WHITE);
}
static void wxStorm(int x, int y) {
  wxCloud(x, y - 3);
  oled.drawLine(x + 10, y + 12, x + 7, y + 17, SSD1306_WHITE);
  oled.drawLine(x + 7, y + 17, x + 11, y + 16, SSD1306_WHITE);
}
static void wxSnow(int x, int y) {
  wxCloud(x, y - 3);
  for (int i = 0; i < 3; i++) oled.drawCircle(x + 4 + i * 4, y + 15, 1, SSD1306_WHITE);
}

// Open-Meteo weather codes, boiled down to something that fits
static const char* wxWord(int c) {
  if (c < 0)  return "NO DATA";
  if (c == 0) return "CLEAR";
  if (c <= 2) return "PARTLY SUNNY";
  if (c == 3) return "OVERCAST";
  if (c <= 48) return "FOGGY";
  if (c <= 57) return "DRIZZLE";
  if (c <= 67) return "RAIN";
  if (c <= 77) return "SNOW";
  if (c <= 82) return "SHOWERS";
  if (c <= 86) return "SNOW SHOWERS";
  return "THUNDERSTORM";
}
static void wxIcon(int c, int x, int y) {
  if (c < 0 || c == 0 || c <= 2) wxSun(x, y);
  else if (c <= 48)  wxCloud(x, y);
  else if (c <= 67)  wxRain(x, y);
  else if (c <= 86)  wxSnow(x, y);
  else               wxStorm(x, y);
}

static void drawClock() {
  struct tm t;
  bool ok = timeOk && getLocalTime(&t, 5);

  oled.clearDisplay();
  hud();

  char big[8], sec[4], date[20];
  if (ok) {
    snprintf(big, sizeof(big), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(sec, sizeof(sec), "%02d", t.tm_sec);
    strftime(date, sizeof(date), "%a %d %b %Y", &t);
    for (char* p = date; *p; p++) *p = toupper(*p);
  } else {
    strcpy(big, "--:--"); strcpy(sec, "--");
    // no clock yet, so it finds something else to think about
    snprintf(date, sizeof(date), "%s", QUIPS[(millis() / 4000) % QUIP_COUNT]);
    for (char* p = date; *p; p++) *p = toupper(*p);
  }

  oled.setTextSize(3);
  int bw = 5 * 18;
  oled.setCursor((SCRW - bw - 16) / 2, 14);
  oled.print(big);
  oled.setTextSize(1);
  oled.setCursor((SCRW - bw - 16) / 2 + bw + 4, 30);
  oled.print(sec);

  ctr(date, 44, 1);

  // seconds sweep along the bottom
  int fill = ok ? (int)((SCRW - 16) * (t.tm_sec + 1) / 60.0f) : 0;
  oled.drawRect(8, 55, SCRW - 16, 5, SSD1306_WHITE);
  if (fill > 2) oled.fillRect(9, 56, fill - 2, 3, SSD1306_WHITE);
  oled.display();
}

static void drawWeather() {
  oled.clearDisplay();
  hud();
  statusBar();

  if (!wxOk) {
    ctr("WEATHER", 20, 1);
    ctr(WiFi.status() == WL_CONNECTED ? "fetching..." : "needs the internet", 34, 1);
    ctr(wCity.length() ? wCity.c_str() : "location unknown", 48, 1);
    oled.display();
    return;
  }

  wxIcon(wCode, 6, 18);

  char l[22];
  snprintf(l, sizeof(l), "%d", (int)roundf(wTemp));
  oled.setTextSize(3);
  int tw = strlen(l) * 18;
  oled.setCursor(34, 18);
  oled.print(l);
  oled.drawCircle(34 + tw + 6, 21, 2, SSD1306_WHITE);      // degree ring
  oled.setTextSize(1);
  oled.setCursor(34 + tw + 11, 18);
  oled.print("C");
  oled.setCursor(34 + tw + 11, 32);
  snprintf(l, sizeof(l), "%d%%", (int)roundf(wHum));
  oled.print(l);

  ctr(wxWord(wCode), 45, 1);
  snprintf(l, sizeof(l), "%s %.0fkm/h", wCity.c_str(), wWind);
  ctr(l, 54, 1);
  oled.display();
}

static void drawInbox() {
  oled.clearDisplay();
  hud();
  statusBar();

  int align = ax > TILT ? 1 : (ax < -TILT ? -1 : 0);
  int step  = ay > TILT ? 1 : (ay < -TILT ? -1 : 0);

  oled.setTextSize(1);
  oled.setCursor(SCRW - 22, 2);
  oled.print(align < 0 ? "<<" : align > 0 ? ">>" : "||");

  if (!inboxN) {
    ctr("NO MESSAGES", 32, 1);
    ctr("send one from the app", 44, 1);
    oled.display();
    return;
  }

  // wrap first, so the block can be positioned knowing how tall it is
  const int PER = 20, MAXL = 3;
  String m = inbox[0], line[MAXL];
  int n = 0;
  for (int i = 0; n < MAXL && i < (int)m.length(); ) {
    int take = min(PER, (int)m.length() - i);
    if (take == PER) { int sp = m.lastIndexOf(' ', i + take); if (sp > i + 5) take = sp - i; }
    line[n++] = m.substring(i, i + take);
    i += take;
    while (i < (int)m.length() && m.charAt(i) == ' ') i++;
  }

  // tilting moves it a line up or down, never past the bottom edge
  int lowest = SCRH - 2 - (n - 1) * 11 - 7;
  int top = constrain(18 + step * 10, 16, max(16, lowest));

  for (int k = 0; k < n; k++) {
    int lw = line[k].length() * 6;
    int x = (SCRW - lw) / 2;
    if (align < 0) x = 8;
    if (align > 0) x = SCRW - lw - 8;
    oled.setCursor(x, top + k * 11);
    oled.print(line[k]);
  }
  oled.display();
}

static void drawHome() {
  oled.clearDisplay();
  hud();
  statusBar();

  char l[24];
  oled.setTextSize(2);
  oled.setCursor((SCRW - 60) / 2, 17);
  oled.print("NEXUS");

  oled.setTextSize(1);
  snprintf(l, sizeof(l), "tap%lu x2:%lu drop%lu",
           (unsigned long)cTap, (unsigned long)cDbl, (unsigned long)cFall);
  ctr(l, 36, 1);
  snprintf(l, sizeof(l), "up %lus  boot %lu",
           (unsigned long)(millis() / 1000UL), (unsigned long)cBoot);
  ctr(l, 45, 1);
  if (WiFi.status() == WL_CONNECTED) snprintf(l, sizeof(l), "%s", WiFi.localIP().toString().c_str());
  else                               snprintf(l, sizeof(l), "192.168.4.1");
  ctr(l, 54, 1);
  oled.display();
}

// depth 0 is the signpost, 1 is picking a setting, 2 is changing it
static void drawSettings() {
  oled.clearDisplay();
  hud();
  statusBar();

  if (navDepth == 0) {
    ctr("SETTINGS", 22, 1);
    ctr("knock twice to go in", 36, 1);
    ctr("once = next screen", 48, 1);
    oled.display();
    return;
  }

  char v[24];
  switch (setIdx) {
    case SET_BRIGHT: snprintf(v, sizeof(v), "%d", cfgBright); break;
    case SET_SLEEP:  snprintf(v, sizeof(v), "%ds", cfgSleepSec); break;
    case SET_EYES:   snprintf(v, sizeof(v), "%s", STYLES[cfgEyeStyle].name); break;
    case SET_WIFI:   snprintf(v, sizeof(v), "%s",
                       WiFi.status() == WL_CONNECTED ? "online" : "hotspot"); break;
    default:         snprintf(v, sizeof(v), "knock x2"); break;
  }

  ctr(SET_NAME[setIdx], 17, 1);

  int len = strlen(v), size = (len * 12 <= SCRW - 24) ? 2 : 1;
  int bw = len * 6 * size;
  int bx = (SCRW - bw) / 2;
  oled.setTextSize(size);
  oled.setCursor(bx, 29);
  oled.print(v);
  if (navDepth == 2) oled.drawRect(bx - 6, 26, bw + 12, 8 * size + 6, SSD1306_WHITE);

  int x0 = (SCRW - (SET_COUNT - 1) * 9) / 2;
  for (int i = 0; i < SET_COUNT; i++) {
    int x = x0 + i * 9;
    if (i == setIdx) oled.fillCircle(x, 47, 3, SSD1306_WHITE);
    else             oled.drawCircle(x, 47, 2, SSD1306_WHITE);
  }

  oled.setTextSize(1);
  ctr(navDepth == 2 ? "1 change     3 back" : "2 open       3 back", 55, 1);
  oled.display();
}

// ================================================================
//  SLEEP AND WAKE
// ================================================================
static void screenPower(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  oled.ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}

static void goSleep() {
  if (asleep) return;
  asleep = true;
  Serial.println("dozing");
  eyes.setIdleMode(OFF);
  eyes.setAutoblinker(OFF);
  eyes.setMood(TIRED);
  eyes.close();
  for (int i = 0; i < 30; i++) { eyes.update(); delay(16); }
  screenPower(false);                 // dark: saves the panel and the battery
  setCpuFrequencyMhz(80);             // and ease the processor right back
}

static void wake(const char* why) {
  lastActive = millis();
  if (!asleep) return;
  asleep = false;
  Serial.printf("waking (%s)\n", why);
  setCpuFrequencyMhz(160);
  screenPower(true);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setMood(DEFAULT);
  eyes.open();
  app = SCR_CLOCK; navDepth = 0;
}

// ================================================================
//  KNOCKS
// ================================================================
//  The ADXL only reports single taps, so we collect them into a
//  burst and decide afterwards whether that was one, two or three.

static void react(unsigned long ms) { reactUntil = millis() + ms; }

static void addMessage(const String& m) {
  for (int i = 3; i > 0; i--) inbox[i] = inbox[i - 1];
  inbox[0] = m;
  if (inboxN < 4) inboxN++;
  cMsg++;
}

static void applyEyes(int i) {
  cfgEyeStyle = (i + STYLE_COUNT) % STYLE_COUNT;
  const EyeStyle& e = STYLES[cfgEyeStyle];
  eyes.setCyclops(e.cyc);
  eyes.setWidth(e.w, e.w);
  eyes.setHeight(e.h, e.h);
  eyes.setBorderradius(e.r, e.r);
  eyes.setSpacebetween(e.gap);
  eyes.setMood(e.mood);
}

static void applyBright() { oled.ssd1306_command(SSD1306_SETCONTRAST); oled.ssd1306_command(cfgBright); }

// ---- one knock: move along ----
static void knockOnce() {
  cTap++;
  if (navDepth == 0) {
    app = (app + 1) % SCR_COUNT;
    if (app == SCR_FACE) { applyEyes(cfgEyeStyle); eyes.open(); }
  } else if (navDepth == 1) {
    setIdx = (setIdx + 1) % SET_COUNT;
  } else {
    switch (setIdx) {                      // depth 2: change the value
      case SET_BRIGHT: cfgBright += 45; if (cfgBright > 255) cfgBright = 25;
                       applyBright(); prefs.putInt("bri", cfgBright); break;
      case SET_SLEEP:  cfgSleepSec = cfgSleepSec >= 120 ? 15 : cfgSleepSec * 2;
                       prefs.putInt("slp", cfgSleepSec); break;
      case SET_EYES:   applyEyes(cfgEyeStyle + 1); prefs.putInt("eye", cfgEyeStyle); break;
      default: break;
    }
  }
}

// ---- two knocks: go in ----
static void knockTwice() {
  cDbl++;
  if (navDepth == 0) {
    if (app == SCR_SETTINGS) { navDepth = 1; setIdx = 0; }
    else if (app == SCR_FACE) { eyes.anim_laugh(); react(1500); }
  } else if (navDepth == 1) {
    if (setIdx == SET_REBOOT) { delay(200); ESP.restart(); }
    if (setIdx != SET_WIFI) navDepth = 2;
  }
}

// ---- three knocks: back out ----
static void knockThrice() {
  if (navDepth > 0) navDepth--;
  else app = SCR_CLOCK;
}

static void onFall() {
  cFall++;
  app = SCR_FACE; navDepth = 0;
  eyes.setMood(DEFAULT);
  eyes.setPosition(N);
  eyes.setVFlicker(ON, 6);
  for (int i = 0; i < 10; i++) { eyes.update(); delay(16); }
  eyes.setPosition(S);
  for (int i = 0; i < 10; i++) { eyes.update(); delay(16); }
  eyes.setVFlicker(OFF);
  eyes.setHeight(6, 6);
  react(2000);
}

static void settleBurst() {
  if (!tapBurst) return;
  if (millis() - tapFirst < TAP_WINDOW_MS) return;
  uint8_t n = tapBurst;
  tapBurst = 0;
  if      (n == 1) knockOnce();
  else if (n == 2) knockTwice();
  else             knockThrice();
  Serial.printf("knock x%u -> %s depth %d\n", n, SCR_NAME[app], navDepth);
}

static void input() {
  readSensors();
  unsigned long now = millis();

  if (adxl) {
    uint8_t s = rReg(adxl, A_INT_SOURCE);
    if (s & INT_FF) { wake("fall"); onFall(); return; }
    if (s & INT_TAP1) {
      wake("knock");
      if (!tapBurst) tapFirst = now;
      if (tapBurst < 3) tapBurst++;
      lastActive = now;
    }
  }
  settleBurst();

  bool shaken = fabsf(amag - 1.0f) > SHAKE_G && now - lastShake > 600;
  if (shaken) {
    lastShake = now; cShake++;
    if (asleep) { wake("shake"); return; }
    if (app == SCR_FACE) {
      eyes.setMood(ANGRY); eyes.setHFlicker(ON, 4); eyes.anim_confused(); react(1600);
    }
    lastActive = now;
    return;
  }

  if (fabsf(amag - 1.0f) > 0.12f || fabsf(gx) + fabsf(gy) + fabsf(gz) > 25.0f) {
    if (asleep) wake("picked up");
    lastActive = now;
  }
  if (asleep) return;

  if (now - lastActive > (unsigned long)cfgSleepSec * 1000UL) { goSleep(); return; }
  if (now < reactUntil) return;

  if (app == SCR_FACE) {                    // the eyes follow the tilt
    if      (ax >  TILT && ay >  TILT) eyes.setPosition(SE);
    else if (ax >  TILT && ay < -TILT) eyes.setPosition(NE);
    else if (ax < -TILT && ay >  TILT) eyes.setPosition(SW);
    else if (ax < -TILT && ay < -TILT) eyes.setPosition(NW);
    else if (ax >  TILT)               eyes.setPosition(E);
    else if (ax < -TILT)               eyes.setPosition(W);
    else if (ay >  TILT)               eyes.setPosition(S);
    else if (ay < -TILT)               eyes.setPosition(N);
    else                               eyes.setPosition(DEFAULT);
  }
}

// ================================================================
//  WEATHER
// ================================================================
static bool httpGetTo(const String& url, bool tls, String& out, int ms) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient h;
  h.setConnectTimeout(ms); h.setTimeout(ms);
  h.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool ok = false;
  if (tls) {
    WiFiClientSecure c; c.setInsecure();
    if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; }
  } else {
    WiFiClient c;
    if (h.begin(c, url) && h.GET() == 200) { out = h.getString(); ok = true; }
  }
  h.end();
  return ok;
}

// pull one number out of flat JSON without dragging in a parser
static float jsonNum(const String& s, const char* key, float def) {
  int i = s.indexOf(String("\"") + key + "\":");
  if (i < 0) return def;
  return s.substring(i + strlen(key) + 3).toFloat();
}

static float locLat = NAN, locLon = NAN;

static void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  String body;

  if (isnan(locLat)) {                       // where are we? ask once
    if (httpGetTo("http://ip-api.com/json/?fields=status,city,lat,lon", false, body, 6000)) {
      locLat = jsonNum(body, "lat", NAN);
      locLon = jsonNum(body, "lon", NAN);
      int c = body.indexOf("\"city\":\"");
      if (c >= 0) { int e = body.indexOf('"', c + 8); wCity = body.substring(c + 8, e); }
      if (wCity.length() > 14) wCity = wCity.substring(0, 14);
      Serial.println("located: " + wCity);
    }
    if (isnan(locLat)) return;
  }

  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(locLat, 3) +
               "&longitude=" + String(locLon, 3) +
               "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m";
  if (!httpGetTo(url, true, body, 8000)) return;

  wTemp = jsonNum(body, "temperature_2m", NAN);
  wHum  = jsonNum(body, "relative_humidity_2m", NAN);
  wWind = jsonNum(body, "wind_speed_10m", NAN);
  wCode = (int)jsonNum(body, "weather_code", -1);
  wxOk  = !isnan(wTemp);
  Serial.printf("weather: %.1fC %.0f%% code %d\n", wTemp, wHum, wCode);
}

// ================================================================
//  WEB PANEL
// ================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nexus</title><style>
:root{--bg:#070d13;--card:#101c27;--fg:#e6eef5;--mut:#7d93a6;--line:#1e2f3d;--acc:#2dd4bf}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;text-align:center}
.wrap{max-width:440px;margin:0 auto;padding:20px}
h1{font-size:13px;letter-spacing:.34em;color:var(--acc);margin:0}
.clock{font-size:46px;font-weight:200;letter-spacing:.02em;margin:2px 0 0;font-variant-numeric:tabular-nums}
.sub{color:var(--mut);font-size:12px;letter-spacing:.14em;text-transform:uppercase}
h2{font-size:11px;letter-spacing:.2em;color:var(--mut);margin:24px 0 8px;text-transform:uppercase}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin-bottom:8px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px 4px}
.tile b{display:block;font-size:20px;font-weight:500;font-variant-numeric:tabular-nums}
.tile span{font-size:10px;color:var(--mut);letter-spacing:.08em}
input,select{width:100%;padding:12px;border-radius:10px;border:1px solid var(--line);background:#0b141c;color:var(--fg);font:inherit;text-align:center}
button{font:inherit;font-weight:600;padding:12px;border:0;border-radius:10px;background:var(--acc);color:#04201c;cursor:pointer;width:100%;margin-top:8px}
button.g{background:transparent;color:var(--fg);border:1px solid var(--line)}
.row{display:flex;gap:8px}.row button{margin-top:0}
table{width:100%;font-size:13px;font-variant-numeric:tabular-nums}
td{padding:3px 0}td:first-child{color:var(--mut);text-align:left}td:last-child{text-align:right}
.bar{height:6px;background:#0b141c;border-radius:3px;overflow:hidden;margin-top:8px}
.bar i{display:block;height:100%;background:var(--acc)}
#t{margin-top:10px;font-size:13px;color:var(--acc);min-height:18px}
</style></head><body><div class="wrap">
<h1>N E X U S</h1>
<div class="clock" id="clk">--:--</div>
<div class="sub" id="sub">connecting</div>

<h2>Send to the face</h2>
<div class="card">
  <input id="m" maxlength="72" placeholder="type a message">
  <button onclick="send()">Show it</button>
</div>

<h2>Open an app</h2>
<div class="card"><div class="row">
  <button class="g" onclick="go(0)">Clock</button>
  <button class="g" onclick="go(1)">Weather</button>
  <button class="g" onclick="go(2)">Inbox</button>
</div><div class="row" style="margin-top:8px">
  <button class="g" onclick="go(3)">Face</button>
  <button class="g" onclick="go(4)">Settings</button>
  <button class="g" onclick="go(5)">Home</button>
</div></div>

<h2>Activity</h2>
<div class="g4">
  <div class="tile"><b id="c1">0</b><span>TAPS</span></div>
  <div class="tile"><b id="c2">0</b><span>DOUBLE</span></div>
  <div class="tile"><b id="c3">0</b><span>FALLS</span></div>
  <div class="tile"><b id="c4">0</b><span>SHAKES</span></div>
</div>

<h2>Motion</h2><div class="card"><table id="mot"></table></div>
<h2>System</h2><div class="card"><table id="sys"></table><div class="bar"><i id="hb"></i></div></div>

<h2>Settings</h2><div class="card">
  <input id="ssid" placeholder="wifi network"><div style="height:8px"></div>
  <input id="pass" type="password" placeholder="wifi password (blank = keep)"><div style="height:8px"></div>
  <input id="tz" placeholder="timezone eg IST-5:30"><div style="height:8px"></div>
  <input id="slp" type="number" min="10" max="3600" placeholder="sleep after, seconds">
  <button onclick="save()">Save and reboot</button>
  <div class="row" style="margin-top:8px">
    <button class="g" onclick="act('/api/wake')">Wake</button>
    <button class="g" onclick="act('/api/sleep')">Sleep</button>
    <button class="g" onclick="act('/api/weather')">Weather</button>
    <button class="g" onclick="if(confirm('Reboot?'))act('/api/reboot')">Reboot</button>
  </div>
</div>
<div id="t"></div>
</div><script>
const $=i=>document.getElementById(i);
window.esc=function(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
window.rows=function(el,o){$(el).innerHTML=Object.entries(o).map(([k,v])=>'<tr><td>'+k+'</td><td>'+esc(v)+'</td></tr>').join('')}
window.post=async function(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
window.act=async function(u){await post(u,{});$('t').textContent='done';load()}
window.go=async function(n){await post('/api/app',{n:n});$('t').textContent='opened'}
window.send=async function(){const m=$('m').value.trim();if(!m){$('t').textContent='type something';return}
  await post('/api/msg',{m:m});$('m').value='';$('t').textContent='sent';load()}
window.save=async function(){
  const d={ssid:$('ssid').value,tz:$('tz').value,slp:$('slp').value};
  if($('pass').value)d.pass=$('pass').value;
  await post('/api/cfg',d);$('t').textContent='saved, rebooting';}
let filled=false;
window.pushTime=async function(){
  const d=new Date();
  await post('/api/time',{e:Math.floor(d.getTime()/1000),o:-d.getTimezoneOffset()});
}
window.load=async function(){
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  if(!s.timeOk){ await pushTime(); }        // give it our clock, it has none
  $('clk').textContent=s.time;
  $('sub').textContent=(s.asleep?'asleep':'awake')+' · '+s.app+' · '+s.wx;
  $('c1').textContent=s.tap;$('c2').textContent=s.dbl;$('c3').textContent=s.fall;$('c4').textContent=s.shake;
  rows('mot',{'ADXL X':s.ax,'ADXL Y':s.ay,'ADXL Z':s.az,'gravity':s.amag+' g',
              'MPU X':s.mx,'MPU Y':s.my,'MPU Z':s.mz,'gyro':s.gyro,'temp':s.temp+' C'});
  rows('sys',{'free ram':s.heap+' B','used':s.used+' B','lowest':s.min+' B',
              'sketch':s.sk+' B','free flash':s.fsk+' B','chip':s.chip,
              'uptime':s.up+' s','boots':s.boots,'clients':s.cl,'ip':s.ip});
  $('hb').style.width=s.pct+'%';
  if(!filled){$('ssid').value=s.ssid;$('tz').value=s.tz;$('slp').value=s.slp;filled=true}
}
load();setInterval(load,1000);
</script></body></html>
)HTML";

static String f2(float v, int d) { return String(v, d); }

static void apiState() {
  char t[10];
  clockStr(t, sizeof(t), true);
  uint32_t heap = ESP.getFreeHeap(), tot = ESP.getHeapSize();
  String o = "{";
  o += "\"time\":\"" + String(t) + "\",\"app\":\"" + String(SCR_NAME[app]) + "\",";
  o += "\"timeOk\":" + String(timeOk ? "true" : "false") + ",";
  o += "\"wx\":\"" + String(wxOk ? (String((int)roundf(wTemp)) + "C " + wxWord(wCode)) : String("no data")) + "\",";
  o += "\"city\":\"" + wCity + "\",";
  o += "\"asleep\":" + String(asleep ? "true" : "false") + ",";
  o += "\"net\":\"" + String(WiFi.status() == WL_CONNECTED ? "online" : "hotspot only") + "\",";
  o += "\"ax\":" + f2(ax, 2) + ",\"ay\":" + f2(ay, 2) + ",\"az\":" + f2(az, 2) + ",\"amag\":" + f2(amag, 2) + ",";
  o += "\"mx\":" + f2(mx, 2) + ",\"my\":" + f2(my, 2) + ",\"mz\":" + f2(mz, 2) + ",";
  o += "\"gyro\":\"" + f2(gx, 0) + " / " + f2(gy, 0) + " / " + f2(gz, 0) + "\",\"temp\":" + f2(mtemp, 1) + ",";
  o += "\"tap\":" + String(cTap) + ",\"dbl\":" + String(cDbl) + ",\"fall\":" + String(cFall) +
       ",\"shake\":" + String(cShake) + ",\"boots\":" + String(cBoot) + ",";
  o += "\"heap\":" + String(heap) + ",\"used\":" + String(tot - heap) + ",\"min\":" + String(ESP.getMinFreeHeap()) +
       ",\"pct\":" + String(100 - heap * 100 / tot) + ",";
  o += "\"sk\":" + String(ESP.getSketchSize()) + ",\"fsk\":" + String(ESP.getFreeSketchSpace()) + ",";
  o += "\"chip\":\"" + String(ESP.getChipModel()) + " @" + String(ESP.getCpuFreqMHz()) + "MHz\",";
  o += "\"up\":" + String(millis() / 1000UL) + ",\"cl\":" + String(WiFi.softAPgetStationNum()) + ",";
  o += "\"ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\",";
  o += "\"ssid\":\"" + cfgSsid + "\",\"tz\":\"" + cfgTz + "\",\"slp\":" + String(cfgSleepSec) + "}";
  web.send(200, "application/json", o);
}

static void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/api/state", HTTP_GET, apiState);
  web.on("/api/msg", HTTP_POST, []() {
    String m = web.arg("m"); m.trim();
    if (m.length()) { addMessage(m.substring(0, 72)); app = SCR_MSG; navDepth = 0; wake("message"); }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/app", HTTP_POST, []() {
    app = constrain((int)web.arg("n").toInt(), 0, SCR_COUNT - 1);
    navDepth = 0;
    wake("panel");
    web.send(200, "application/json", "{\"ok\":true}");
  });
  // Your phone knows the time even when the clock cannot reach a time
  // server, so the page hands it over the moment it loads.
  web.on("/api/time", HTTP_POST, []() {
    long epoch = web.arg("e").toInt();
    int  offs  = web.arg("o").toInt();          // minutes east of UTC
    if (epoch > 1700000000L) {
      struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
      settimeofday(&tv, nullptr);
      char tz[24];
      snprintf(tz, sizeof(tz), "UTC%+d:%02d", -offs / 60, abs(offs) % 60);
      setenv("TZ", tz, 1); tzset();
      timeOk = true;
      Serial.printf("time set from the browser (%s)\n", tz);
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/weather", HTTP_POST, []() {
    nextWx = 0;
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/wake",  HTTP_POST, []() { wake("panel"); web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/sleep", HTTP_POST, []() { goSleep(); web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/cfg", HTTP_POST, []() {
    String s = web.arg("ssid"); s.trim();
    if (s.length()) { cfgSsid = s; prefs.putString("ssid", cfgSsid); }
    if (web.arg("pass").length()) { cfgPass = web.arg("pass"); prefs.putString("pass", cfgPass); }
    String z = web.arg("tz"); z.trim();
    if (z.length()) { cfgTz = z; prefs.putString("tz", cfgTz); }
    int sl = web.arg("slp").toInt();
    if (sl >= 10 && sl <= 3600) { cfgSleepSec = sl; prefs.putInt("slp", cfgSleepSec); }
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
  web.on("/api/reboot", HTTP_POST, []() {
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300); ESP.restart();
  });
  web.onNotFound([]() {
    web.sendHeader("Location", "http://192.168.4.1/", true);
    web.send(302, "text/plain", "");
  });
  web.begin();
}

// ================================================================
//  BOOT
// ================================================================
static void splash(const char* line1, const char* line2) {
  oled.clearDisplay();
  hud();
  oled.setTextSize(2);
  int w = 5 * 12;
  oled.setCursor((SCRW - w) / 2, 16);
  oled.print("NEXUS");
  oled.setTextSize(1);
  ctr(line1, 38, 1);
  if (line2) ctr(line2, 50, 1);
  oled.display();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("nexus", false);
  cBoot = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", cBoot);
  cfgSsid = prefs.getString("ssid", DEF_WIFI_SSID);
  cfgPass = prefs.getString("pass", DEF_WIFI_PASS);
  cfgTz   = prefs.getString("tz",   DEF_TZ);
  cfgSleepSec = constrain(prefs.getInt("slp", 30), 10, 3600);
  cfgBright   = constrain(prefs.getInt("bri", 160), 10, 255);
  cfgEyeStyle = constrain(prefs.getInt("eye", 0), 0, STYLE_COUNT - 1);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  // the last argument stops Adafruit calling Wire.begin() on the default pins
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false)) {
    Serial.println("no OLED"); return;
  }
  oled.setTextWrap(false);
  oled.setTextColor(SSD1306_WHITE);

  splash("starting up", nullptr);
  startSensors();

  // hotspot for the panel, station for the clock
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  splash(AP_SSID, "pass: password");
  delay(1400);

  splash("joining wifi", cfgSsid.c_str());
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) delay(120);

  if (WiFi.status() == WL_CONNECTED) {
    splash("getting the time", WiFi.localIP().toString().c_str());
    configTzTime(cfgTz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    struct tm tm0;
    t0 = millis();
    while (!getLocalTime(&tm0, 200) && millis() - t0 < 8000) delay(100);
    timeOk = getLocalTime(&tm0, 200);
    Serial.println(timeOk ? "time ok" : "no time");
  } else {
    splash("no network", "hotspot only");
    delay(1200);
  }

  setupWeb();

  eyes.begin(SCRW, SCRH, 50);
  applyEyes(cfgEyeStyle);
  eyes.setAutoblinker(ON, 3, 2); eyes.setIdleMode(ON, 2, 2);
  applyBright();

  splash(timeOk ? "ready" : "no clock yet", "1 next  2 in  3 back");
  delay(1700);

  app = SCR_CLOCK; navDepth = 0;
  lastActive = millis();
  Serial.printf("up. boot #%lu\n", (unsigned long)cBoot);
}

void loop() {
  web.handleClient();

  unsigned long now = millis();
  if (now - lastPoll >= 45) { lastPoll = now; input(); }

  // weather every 15 minutes, and only when there is a network
  if (!asleep && WiFi.status() == WL_CONNECTED && (long)(now - nextWx) >= 0) {
    nextWx = now + 900000UL;
    fetchWeather();
  }

  if (asleep) { delay(6); return; }        // nothing to draw while it dozes

  if (app == SCR_FACE && navDepth == 0) {
    eyes.update();                         // paces itself
  } else if (now - lastDraw >= 120) {      // 8 fps is plenty for the panels
    lastDraw = now;
    switch (app) {
      case SCR_CLOCK:    drawClock();    break;
      case SCR_WEATHER:  drawWeather();  break;
      case SCR_MSG:      drawInbox();    break;
      case SCR_SETTINGS: drawSettings(); break;
      default:           drawHome();     break;
    }
  }
  delay(2);
}
