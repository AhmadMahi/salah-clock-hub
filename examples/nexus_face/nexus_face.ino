/*
  ================================================================
   NEXUS FACE  -  ESP32-C3 companion
  ================================================================
   Boots into a HUD style menu you steer by tilting the board.
   Tilt left or right to move, knock once to open, knock twice to
   come back. Leave it alone for a minute and it falls asleep, and
   the screen goes dark to save it. Shake it and it wakes.

   APPS
     CLOCK    real time from the internet, with a seconds sweep
     FACE     animated robot eyes that react to you
     SENSORS  live bubble level and both accelerometers
     INBOX    messages you send it, with tilt steered text
     SYSTEM   memory, uptime, network, counters

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
#include <Preferences.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

#define SDA_PIN 8
#define SCL_PIN 9
#define OLED_ADDR 0x3C
#define W 128
#define H 64

#define DEF_WIFI_SSID "YOUR_WIFI_NAME"
#define DEF_WIFI_PASS "YOUR_WIFI_PASSWORD"
#define DEF_TZ        "IST-5:30"          // India. See the panel to change.

const char* AP_SSID = "NEXUS-ROBOT";
const char* AP_PASS = "password";

Adafruit_SSD1306 oled(W, H, &Wire, -1);
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
enum { APP_MENU = 0, APP_CLOCK, APP_FACE, APP_SENSORS, APP_INBOX, APP_SYSTEM, APP_COUNT };
const char* APP_NAME[APP_COUNT] = { "MENU", "CLOCK", "FACE", "SENSORS", "INBOX", "SYSTEM" };

int  app = APP_MENU, sel = APP_CLOCK;
bool asleep = false, screenOn = true, timeOk = false;
unsigned long lastActive = 0, lastDraw = 0, lastPoll = 0, lastTiltStep = 0;
unsigned long reactUntil = 0, lastShake = 0;

uint32_t cTap = 0, cDbl = 0, cFall = 0, cShake = 0, cMsg = 0, cBoot = 0;
String   inbox[4];
int      inboxN = 0;

int  cfgSleepSec = 60;
String cfgSsid, cfgPass, cfgTz;

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
  oled.setCursor(w < W ? (W - w) / 2 : 0, y);
  oled.print(s);
}

// Corner brackets. Cheap, and it makes everything look deliberate.
static void hud() {
  const int L = 6;
  oled.drawFastHLine(0, 0, L, SSD1306_WHITE);      oled.drawFastVLine(0, 0, L, SSD1306_WHITE);
  oled.drawFastHLine(W - L, 0, L, SSD1306_WHITE);  oled.drawFastVLine(W - 1, 0, L, SSD1306_WHITE);
  oled.drawFastHLine(0, H - 1, L, SSD1306_WHITE);  oled.drawFastVLine(0, H - L, L, SSD1306_WHITE);
  oled.drawFastHLine(W - L, H - 1, L, SSD1306_WHITE); oled.drawFastVLine(W - 1, H - L, L, SSD1306_WHITE);
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
    int h = 2 + i * 2, x = W - 20 + i * 3;
    if (i < bars) oled.fillRect(x, 10 - h, 2, h, SSD1306_WHITE);
    else          oled.drawPixel(x, 9, SSD1306_WHITE);
  }
  oled.drawFastHLine(6, 12, W - 12, SSD1306_WHITE);
}

// ---- little 16x16 glyphs for the menu ----
static void iconClock(int x, int y) {
  oled.drawCircle(x + 8, y + 8, 7, SSD1306_WHITE);
  oled.drawLine(x + 8, y + 8, x + 8, y + 4, SSD1306_WHITE);
  oled.drawLine(x + 8, y + 8, x + 11, y + 9, SSD1306_WHITE);
}
static void iconFace(int x, int y) {
  oled.fillRoundRect(x, y + 3, 6, 10, 2, SSD1306_WHITE);
  oled.fillRoundRect(x + 10, y + 3, 6, 10, 2, SSD1306_WHITE);
}
static void iconWave(int x, int y) {
  for (int i = 0; i < 16; i++) {
    int h = (int)(5 * sinf(i * 0.9f));
    oled.drawPixel(x + i, y + 8 + h, SSD1306_WHITE);
    oled.drawPixel(x + i, y + 9 + h, SSD1306_WHITE);
  }
}
static void iconMail(int x, int y) {
  oled.drawRect(x, y + 3, 16, 11, SSD1306_WHITE);
  oled.drawLine(x + 1, y + 4, x + 8, y + 9, SSD1306_WHITE);
  oled.drawLine(x + 14, y + 4, x + 8, y + 9, SSD1306_WHITE);
}
static void iconChip(int x, int y) {
  oled.drawRect(x + 3, y + 3, 10, 10, SSD1306_WHITE);
  oled.drawRect(x + 6, y + 6, 4, 4, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {
    oled.drawFastHLine(x, y + 5 + i * 3, 3, SSD1306_WHITE);
    oled.drawFastHLine(x + 13, y + 5 + i * 3, 3, SSD1306_WHITE);
  }
}
static void drawIcon(int id, int x, int y) {
  switch (id) {
    case APP_CLOCK:   iconClock(x, y); break;
    case APP_FACE:    iconFace(x, y);  break;
    case APP_SENSORS: iconWave(x, y);  break;
    case APP_INBOX:   iconMail(x, y);  break;
    default:          iconChip(x, y);  break;
  }
}

// ================================================================
//  SCREENS
// ================================================================
static void drawMenu() {
  oled.clearDisplay();
  hud();
  statusBar();

  // selected app, big, with chevrons either side
  drawIcon(sel, 56, 18);
  oled.setTextSize(1);
  ctr(APP_NAME[sel], 38, 1);

  int blink = (millis() / 500) % 2;
  if (blink) {
    oled.fillTriangle(14, 26, 22, 21, 22, 31, SSD1306_WHITE);
    oled.fillTriangle(W - 14, 26, W - 22, 21, W - 22, 31, SSD1306_WHITE);
  }

  // position dots
  int n = APP_COUNT - 1, x0 = (W - (n - 1) * 10) / 2;
  for (int i = 0; i < n; i++) {
    int x = x0 + i * 10;
    if (i + 1 == sel) oled.fillCircle(x, 54, 3, SSD1306_WHITE);
    else              oled.drawCircle(x, 54, 2, SSD1306_WHITE);
  }
  oled.display();
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
    strcpy(date, WiFi.status() == WL_CONNECTED ? "SYNCING TIME" : "NO NETWORK");
  }

  oled.setTextSize(3);
  int bw = 5 * 18;
  oled.setCursor((W - bw - 16) / 2, 14);
  oled.print(big);
  oled.setTextSize(1);
  oled.setCursor((W - bw - 16) / 2 + bw + 4, 30);
  oled.print(sec);

  ctr(date, 44, 1);

  // seconds sweep along the bottom
  int fill = ok ? (int)((W - 16) * (t.tm_sec + 1) / 60.0f) : 0;
  oled.drawRect(8, 55, W - 16, 5, SSD1306_WHITE);
  if (fill > 2) oled.fillRect(9, 56, fill - 2, 3, SSD1306_WHITE);
  oled.display();
}

static void drawSensors() {
  oled.clearDisplay();
  hud();
  statusBar();

  const int BX = 8, BY = 18, BW = 44, BH = 38;
  oled.drawRect(BX, BY, BW, BH, SSD1306_WHITE);
  int cx = BX + BW / 2, cy = BY + BH / 2;
  oled.drawFastHLine(cx - 3, cy, 7, SSD1306_WHITE);
  oled.drawFastVLine(cx, cy - 3, 7, SSD1306_WHITE);
  int dx = constrain((int)(ax * (BW / 2 - 5)), -(BW / 2 - 5), BW / 2 - 5);
  int dy = constrain((int)(ay * (BH / 2 - 5)), -(BH / 2 - 5), BH / 2 - 5);
  oled.fillCircle(cx + dx, cy + dy, 4, SSD1306_WHITE);

  char l[20];
  oled.setTextSize(1);
  snprintf(l, sizeof(l), "X%+5.2f", ax); oled.setCursor(58, 18); oled.print(l);
  snprintf(l, sizeof(l), "Y%+5.2f", ay); oled.setCursor(58, 28); oled.print(l);
  snprintf(l, sizeof(l), "Z%+5.2f", az); oled.setCursor(58, 38); oled.print(l);
  snprintf(l, sizeof(l), "%.2fg %s", amag, mpu ? "2x" : "1x");
  oled.setCursor(58, 48); oled.print(l);
  oled.display();
}

static void drawInbox() {
  oled.clearDisplay();
  hud();
  statusBar();

  int align = ax > TILT ? 1 : (ax < -TILT ? -1 : 0);
  int step  = ay > TILT ? 1 : (ay < -TILT ? -1 : 0);

  oled.setTextSize(1);
  oled.setCursor(W - 22, 2);
  oled.print(align < 0 ? "<<" : align > 0 ? ">>" : "||");

  if (!inboxN) {
    ctr("NO MESSAGES", 32, 1);
    ctr("send one from the panel", 44, 1);
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
  int lowest = H - 2 - (n - 1) * 11 - 7;
  int top = constrain(18 + step * 10, 16, max(16, lowest));

  for (int k = 0; k < n; k++) {
    int lw = line[k].length() * 6;
    int x = (W - lw) / 2;
    if (align < 0) x = 8;
    if (align > 0) x = W - lw - 8;
    oled.setCursor(x, top + k * 11);
    oled.print(line[k]);
  }
  oled.display();
}

static void drawSystem() {
  oled.clearDisplay();
  hud();
  statusBar();

  char l[24];
  uint32_t heap = ESP.getFreeHeap(), tot = ESP.getHeapSize();
  oled.setTextSize(1);

  snprintf(l, sizeof(l), "ram  %uk / %uk", (unsigned)(heap / 1024), (unsigned)(tot / 1024));
  oled.setCursor(8, 17); oled.print(l);

  int bw = W - 16, fill = bw * (tot - heap) / tot;
  oled.drawRect(8, 27, bw, 5, SSD1306_WHITE);
  if (fill > 2) oled.fillRect(9, 28, fill - 2, 3, SSD1306_WHITE);

  snprintf(l, sizeof(l), "up %lus  boot %lu",
           (unsigned long)(millis() / 1000UL), (unsigned long)cBoot);
  oled.setCursor(8, 36); oled.print(l);
  snprintf(l, sizeof(l), "tap%lu dbl%lu fall%lu",
           (unsigned long)cTap, (unsigned long)cDbl, (unsigned long)cFall);
  oled.setCursor(8, 45); oled.print(l);

  if (WiFi.status() == WL_CONNECTED) snprintf(l, sizeof(l), "%s", WiFi.localIP().toString().c_str());
  else                               snprintf(l, sizeof(l), "192.168.4.1");
  oled.setCursor(8, 54); oled.print(l);
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
}

static void wake(const char* why) {
  lastActive = millis();
  if (!asleep) return;
  asleep = false;
  Serial.printf("waking (%s)\n", why);
  screenPower(true);
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  eyes.setMood(DEFAULT);
  eyes.open();
  app = APP_MENU;
}

// ================================================================
//  INPUT: tilt to move, knock to choose
// ================================================================
static void react(unsigned long ms) { reactUntil = millis() + ms; }

static void addMessage(const String& m) {
  for (int i = 3; i > 0; i--) inbox[i] = inbox[i - 1];
  inbox[0] = m;
  if (inboxN < 4) inboxN++;
  cMsg++;
}

static void onTap() {
  cTap++;
  if (app == APP_MENU) { app = sel; Serial.printf("open %s\n", APP_NAME[app]); }
  else if (app == APP_FACE) { eyes.blink(); react(600); }
  else app = APP_MENU;
}

static void onDoubleTap() {
  cDbl++;
  if (app == APP_FACE) { eyes.anim_laugh(); react(1600); }
  app = APP_MENU;
  Serial.println("back to menu");
}

static void onFall() {
  cFall++;
  Serial.println("FALL");
  app = APP_FACE;
  eyes.setMood(DEFAULT);
  eyes.setPosition(N);
  eyes.setVFlicker(ON, 6);
  for (int i = 0; i < 10; i++) { eyes.update(); delay(16); }
  eyes.setPosition(S);
  for (int i = 0; i < 10; i++) { eyes.update(); delay(16); }
  eyes.setVFlicker(OFF);
  eyes.setHeight(6, 6);
  react(2200);
}

static void input() {
  readSensors();
  unsigned long now = millis();

  if (adxl) {
    uint8_t s = rReg(adxl, A_INT_SOURCE);
    if (s & INT_FF)        { wake("fall"); onFall();      return; }
    if (s & INT_TAP2)      { wake("knock"); onDoubleTap(); return; }
    if (s & INT_TAP1)      { wake("knock"); onTap();       return; }
  }

  bool shaken = fabsf(amag - 1.0f) > SHAKE_G && now - lastShake > 600;
  if (shaken) {
    lastShake = now;
    cShake++;
    if (asleep) { wake("shake"); return; }
    if (app == APP_FACE) {
      eyes.setMood(ANGRY); eyes.setHFlicker(ON, 4); eyes.anim_confused(); react(1800);
    }
    lastActive = now;
    return;
  }

  bool moved = fabsf(amag - 1.0f) > 0.12f || fabsf(gx) + fabsf(gy) + fabsf(gz) > 25.0f;
  if (moved) lastActive = now;
  if (asleep) return;

  if (now - lastActive > (unsigned long)cfgSleepSec * 1000UL) { goSleep(); return; }
  if (now < reactUntil) return;

  if (app == APP_MENU) {                       // tilt steps the selection
    if (now - lastTiltStep > STEP_MS) {
      if (ax > TILT)       { sel = sel % (APP_COUNT - 1) + 1; lastTiltStep = now; lastActive = now; }
      else if (ax < -TILT) { sel = (sel - 2 + APP_COUNT - 1) % (APP_COUNT - 1) + 1; lastTiltStep = now; lastActive = now; }
    }
  } else if (app == APP_FACE) {                // the eyes follow the tilt
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
  <button class="g" onclick="go(1)">Clock</button>
  <button class="g" onclick="go(2)">Face</button>
  <button class="g" onclick="go(3)">Sensors</button>
</div><div class="row" style="margin-top:8px">
  <button class="g" onclick="go(4)">Inbox</button>
  <button class="g" onclick="go(5)">System</button>
  <button class="g" onclick="go(0)">Menu</button>
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
window.load=async function(){
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  $('clk').textContent=s.time;
  $('sub').textContent=(s.asleep?'asleep':'awake')+' · '+s.app+' · '+s.net;
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
  o += "\"time\":\"" + String(t) + "\",\"app\":\"" + String(APP_NAME[app]) + "\",";
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
    if (m.length()) { addMessage(m.substring(0, 72)); app = APP_INBOX; wake("message"); }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/app", HTTP_POST, []() {
    app = constrain((int)web.arg("n").toInt(), 0, APP_COUNT - 1);
    wake("panel");
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
  oled.setCursor((W - w) / 2, 16);
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
  cfgSleepSec = constrain(prefs.getInt("slp", 60), 10, 3600);

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

  eyes.begin(W, H, 50);
  eyes.setWidth(36, 36); eyes.setHeight(36, 36);
  eyes.setBorderradius(10, 10); eyes.setSpacebetween(12);
  eyes.setAutoblinker(ON, 3, 2); eyes.setIdleMode(ON, 2, 2);

  splash(timeOk ? "ready" : "ready, no clock", "tilt to move, knock to pick");
  delay(1600);

  app = APP_MENU; sel = APP_CLOCK;
  lastActive = millis();
  Serial.printf("up. boot #%lu\n", (unsigned long)cBoot);
}

void loop() {
  web.handleClient();

  unsigned long now = millis();
  if (now - lastPoll >= 45) { lastPoll = now; input(); }

  if (asleep) { delay(4); return; }        // nothing to draw while it dozes

  if (app == APP_FACE) {
    eyes.update();                         // paces itself
  } else if (now - lastDraw >= 120) {      // 8 fps is plenty for the panels
    lastDraw = now;
    switch (app) {
      case APP_CLOCK:   drawClock();   break;
      case APP_SENSORS: drawSensors(); break;
      case APP_INBOX:   drawInbox();   break;
      case APP_SYSTEM:  drawSystem();  break;
      default:          drawMenu();    break;
    }
  }
  delay(2);
}
