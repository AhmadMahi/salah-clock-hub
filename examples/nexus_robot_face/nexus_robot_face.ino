/*
  ================================================================
   ROBOT FACE  -  ESP32-C3 + SSD1306 + ADXL345 + MPU6050
  ================================================================
   An emotional companion with its own WiFi hotspot.

   ON POWER UP      it is asleep, yawns, wakes, looks around, grins
   LEFT ALONE       cycles moods, blinks, glances about, then dozes
                    off again until something moves it
   TILT             the eyes follow
   SHAKE            angry, with a shudder
   ONE KNOCK        next eye style
   TWO KNOCKS       next eye style again, and it laughs
   DROP IT          eyes roll and it falls flat

   ITS OWN HOTSPOT
     network   NEXUS-ROBOT
     password  password
     then open http://192.168.4.1
   From there: send it a message to show, watch both sensors live,
   read system info, pick the eye style, switch screens, reboot.

   MESSAGE SCREEN   tilt steers the text. Lean left and it aligns
   left, right and it aligns right, level and it centres. Tip it
   forward or back and the text steps down or up a line.

   WIRING   everything on one I2C bus
     SDA ........... GPIO 8
     SCL ........... GPIO 9
     OLED .......... 0x3C
     ADXL345 ....... 0x53, or 0x1D if its SDO pin is tied high
     MPU6050 ....... 0x68, or 0x69 if its AD0 pin is tied high
     VCC ........... 3V3 on all
     GND ........... GND on all
   Either sensor may be missing; the sketch uses whatever answers.

   LIBRARIES (Library Manager)
     FluxGarage RoboEyes      by Dennis Hoelscher
     Adafruit GFX Library
     Adafruit SSD1306

   Board: any ESP32-C3 board
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

#define I2C_SDA     8
#define I2C_SCL     9
#define OLED_ADDR   0x3C
#define SCREEN_W    128
#define SCREEN_H    64

const char* AP_SSID = "NEXUS-ROBOT";
const char* AP_PASS = "password";

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(display);
WebServer   web(80);
Preferences prefs;

// ================================================================
//  ADXL345
// ================================================================
#define ADXL_LOW 0x53
#define ADXL_HIGH 0x1D
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
#define INT_FF   0x04
#define A_LSB_G  256.0f

// ================================================================
//  MPU6050
// ================================================================
#define MPU_LOW 0x68
#define MPU_HIGH 0x69
#define M_WHOAMI 0x75
#define M_PWR1 0x6B
#define M_GYRO_CFG 0x1B
#define M_ACC_CFG 0x1C
#define M_ACCEL_H 0x3B
#define M_LSB_G  16384.0f      // +/- 2 g
#define M_LSB_DPS 131.0f       // +/- 250 deg/s

uint8_t adxlAddr = 0, mpuAddr = 0;
bool    oledOk = false;

// live values
float ax = 0, ay = 0, az = 0, amag = 1;          // ADXL345, g
float mx = 0, my = 0, mz = 0, mmag = 1;          // MPU6050 accel, g
float gxr = 0, gyr = 0, gzr = 0;                 // MPU6050 gyro, deg/s
float mtemp = 0;

// counters, boots kept in flash
uint32_t nTap = 0, nDouble = 0, nFall = 0, nShake = 0, nMpuTap = 0, nMsg = 0;
uint32_t nBoots = 0;

// ================================================================
//  EYE STYLES
// ================================================================
struct EyeStyle {
  const char* name;
  byte w, h, radius;
  int  space;
  bool cyclops;
  byte mood;
};
const EyeStyle STYLES[] = {
  { "round",   36, 36, 10, 12, false, DEFAULT },
  { "square",  38, 38,  2, 10, false, DEFAULT },
  { "wide",    48, 28, 12,  8, false, DEFAULT },
  { "narrow",  24, 40,  8, 22, false, DEFAULT },
  { "sleepy",  36, 14, 6,  12, false, TIRED   },
  { "cross",   34, 34,  8, 14, false, ANGRY   },
  { "joy",     36, 36, 16, 12, false, HAPPY   },
  { "cyclops", 46, 46, 14,  0, true,  DEFAULT },
};
const int STYLE_COUNT = sizeof(STYLES) / sizeof(STYLES[0]);
int styleIdx = 0;

// ================================================================
//  STATE
// ================================================================
enum { SCR_FACE = 0, SCR_STATS = 1, SCR_MSG = 2 };
int  screenMode = SCR_FACE;

String message = "";
unsigned long msgUntil = 0;

bool  asleep = false;
unsigned long lastMotion = 0, nextMood = 0, reactUntil = 0, lastShake = 0, lastMpuTap = 0;
uint8_t idleMood = DEFAULT;
float lastMpuMag = 1.0f;

#define MOOD_EVERY_MS  6000
#define TILT_ON        0.35f
#define SHAKE_G        0.55f
#define SHAKE_GAP_MS   600
#define REACT_MS       2200
#define DOZE_AFTER_MS  45000        // no movement for this long and it dozes
#define MSG_SHOW_MS    20000

// ================================================================
//  I2C helpers
// ================================================================
void wReg(uint8_t a, uint8_t r, uint8_t v) {
  Wire.beginTransmission(a); Wire.write(r); Wire.write(v); Wire.endTransmission();
}
uint8_t rReg(uint8_t a, uint8_t r) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((int)a, 1) != 1) return 0;
  return Wire.read();
}
bool rBlock(uint8_t a, uint8_t r, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(a); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)a, (int)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

bool startAdxl() {
  uint8_t t[2] = { ADXL_LOW, ADXL_HIGH };
  for (int i = 0; i < 2; i++) {
    if (rReg(t[i], A_DEVID) != 0xE5) continue;
    adxlAddr = t[i];
    wReg(adxlAddr, A_DATA_FORMAT, 0x0B);
    wReg(adxlAddr, A_THRESH_TAP, 0x28);
    wReg(adxlAddr, A_DUR, 0x10);
    wReg(adxlAddr, A_LATENT, 0x50);
    wReg(adxlAddr, A_WINDOW, 0xF0);
    wReg(adxlAddr, A_TAP_AXES, 0x07);
    wReg(adxlAddr, A_THRESH_FF, 0x07);
    wReg(adxlAddr, A_TIME_FF, 0x14);
    wReg(adxlAddr, A_INT_ENABLE, INT_TAP1 | INT_TAP2 | INT_FF);
    wReg(adxlAddr, A_POWER_CTL, 0x08);
    delay(20);
    rReg(adxlAddr, A_INT_SOURCE);
    Serial.printf("ADXL345 at 0x%02X\n", adxlAddr);
    return true;
  }
  Serial.println("no ADXL345");
  return false;
}

bool startMpu() {
  uint8_t t[2] = { MPU_LOW, MPU_HIGH };
  for (int i = 0; i < 2; i++) {
    uint8_t who = rReg(t[i], M_WHOAMI);
    // the part and its clones answer with a few different ids
    if (who != 0x68 && who != 0x69 && who != 0x70 && who != 0x71 && who != 0x98) continue;
    mpuAddr = t[i];
    wReg(mpuAddr, M_PWR1, 0x00);        // leave sleep
    delay(10);
    wReg(mpuAddr, M_GYRO_CFG, 0x00);    // +/- 250 deg/s
    wReg(mpuAddr, M_ACC_CFG, 0x00);     // +/- 2 g
    Serial.printf("MPU6050 at 0x%02X (who am i 0x%02X)\n", mpuAddr, who);
    return true;
  }
  Serial.println("no MPU6050");
  return false;
}

void readAdxl() {
  if (!adxlAddr) return;
  uint8_t b[6];
  if (!rBlock(adxlAddr, A_DATAX0, b, 6)) return;
  ax = (int16_t)((b[1] << 8) | b[0]) / A_LSB_G;
  ay = (int16_t)((b[3] << 8) | b[2]) / A_LSB_G;
  az = (int16_t)((b[5] << 8) | b[4]) / A_LSB_G;
  amag = sqrtf(ax * ax + ay * ay + az * az);
}

void readMpu() {
  if (!mpuAddr) return;
  uint8_t b[14];
  if (!rBlock(mpuAddr, M_ACCEL_H, b, 14)) return;      // accel, temp, gyro in one go
  mx = (int16_t)((b[0] << 8) | b[1]) / M_LSB_G;
  my = (int16_t)((b[2] << 8) | b[3]) / M_LSB_G;
  mz = (int16_t)((b[4] << 8) | b[5]) / M_LSB_G;
  mtemp = (int16_t)((b[6] << 8) | b[7]) / 340.0f + 36.53f;
  gxr = (int16_t)((b[8]  << 8) | b[9])  / M_LSB_DPS;
  gyr = (int16_t)((b[10] << 8) | b[11]) / M_LSB_DPS;
  gzr = (int16_t)((b[12] << 8) | b[13]) / M_LSB_DPS;
  mmag = sqrtf(mx * mx + my * my + mz * mz);
}

// The MPU6050 has no tap hardware, so watch for a sharp jerk instead
void mpuTapWatch() {
  if (!mpuAddr) return;
  float jerk = fabsf(mmag - lastMpuMag);
  lastMpuMag = mmag;
  unsigned long now = millis();
  if (jerk > 0.9f && now - lastMpuTap > 250) {
    lastMpuTap = now;
    nMpuTap++;
  }
}

// ================================================================
//  EYES
// ================================================================
void applyStyle(int i) {
  styleIdx = (i + STYLE_COUNT) % STYLE_COUNT;
  const EyeStyle& s = STYLES[styleIdx];
  eyes.setCyclops(s.cyclops);
  eyes.setWidth(s.w, s.w);
  eyes.setHeight(s.h, s.h);
  eyes.setBorderradius(s.radius, s.radius);
  eyes.setSpacebetween(s.space);
  eyes.setMood(s.mood);
  idleMood = s.mood;
  Serial.printf("eye style -> %s\n", s.name);
}

void react(unsigned long ms) { reactUntil = millis() + ms; }
bool reacting() { return millis() < reactUntil; }

void wakeUp() {
  if (!asleep) return;
  asleep = false;
  eyes.setAutoblinker(ON, 3, 2);
  eyes.setIdleMode(ON, 2, 2);
  applyStyle(styleIdx);
  eyes.open();
  eyes.anim_laugh();
  Serial.println("waking up");
}

void doze() {
  if (asleep) return;
  asleep = true;
  eyes.setIdleMode(OFF);
  eyes.setAutoblinker(OFF);
  eyes.setMood(TIRED);
  eyes.close();
  Serial.println("dozing off");
}

// eyes roll and the whole face drops - it just fell over
void fallAnimation() {
  nFall++;
  Serial.println("FALL");
  eyes.setMood(DEFAULT);
  eyes.setPosition(N);
  eyes.setVFlicker(ON, 6);
  for (int i = 0; i < 12; i++) { eyes.update(); delay(18); }
  eyes.setPosition(S);
  for (int i = 0; i < 12; i++) { eyes.update(); delay(18); }
  eyes.setVFlicker(OFF);
  eyes.setHeight(6, 6);                    // flattened, knocked out
  eyes.setPosition(S);
  react(2600);
}

// ================================================================
//  SCREENS
// ================================================================
void ctr(const char* s, int y, int size) {
  int w = (int)strlen(s) * 6 * size;
  display.setTextSize(size);
  display.setCursor(w < SCREEN_W ? (SCREEN_W - w) / 2 : 0, y);
  display.print(s);
}

void drawStats() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  ctr("ROBOT STATS", 0, 1);
  display.drawFastHLine(14, 10, SCREEN_W - 28, SSD1306_WHITE);

  char l[26];
  int y = 14;
  snprintf(l, sizeof(l), "tap %lu   double %lu", (unsigned long)nTap, (unsigned long)nDouble);
  ctr(l, y, 1); y += 10;
  snprintf(l, sizeof(l), "falls %lu   shake %lu", (unsigned long)nFall, (unsigned long)nShake);
  ctr(l, y, 1); y += 10;
  snprintf(l, sizeof(l), "boots %lu   msgs %lu", (unsigned long)nBoots, (unsigned long)nMsg);
  ctr(l, y, 1); y += 10;
  snprintf(l, sizeof(l), "g %.2f   ram %uk", amag, (unsigned)(ESP.getFreeHeap() / 1024));
  ctr(l, y, 1); y += 10;
  snprintf(l, sizeof(l), "up %lus  %s", (unsigned long)(millis() / 1000UL), STYLES[styleIdx].name);
  ctr(l, y, 1);

  display.display();
}

// Tilt steers the text: lean left it goes left, right it goes right,
// tip it and the whole block steps down or up a line.
void drawMessage() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  int align = 0;                            // -1 left, 0 centre, +1 right
  if      (ax >  TILT_ON) align =  1;
  else if (ax < -TILT_ON) align = -1;

  int step = 0;                             // -1 up a line, +1 down a line
  if      (ay >  TILT_ON) step =  1;
  else if (ay < -TILT_ON) step = -1;

  const char* arrow = align < 0 ? "<<" : (align > 0 ? ">>" : "||");
  ctr("MESSAGE", 0, 1);
  display.setCursor(SCREEN_W - 12, 0);
  display.print(arrow);
  display.drawFastHLine(14, 10, SCREEN_W - 28, SSD1306_WHITE);

  // wrap into lines of 21 characters
  String m = message.length() ? message : String("(nothing yet)");
  const int PER = 21, MAXL = 4;
  String lines[MAXL];
  int n = 0, i = 0;
  while (i < (int)m.length() && n < MAXL) {
    int take = min(PER, (int)m.length() - i);
    if (take == PER) {                      // do not split mid word if we can help it
      int sp = m.lastIndexOf(' ', i + take);
      if (sp > i + 6) take = sp - i;
    }
    lines[n++] = m.substring(i, i + take);
    i += take;
    while (i < (int)m.length() && m.charAt(i) == ' ') i++;
  }

  int blockH = n * 11;
  int top = 14 + ((50 - blockH) / 2) + step * 11;
  top = constrain(top, 12, 54 - blockH < 12 ? 12 : 54 - blockH);

  for (int k = 0; k < n; k++) {
    int w = lines[k].length() * 6;
    int x = (SCREEN_W - w) / 2;             // centred
    if (align < 0) x = 2;                   // pushed left
    if (align > 0) x = SCREEN_W - w - 2;    // pushed right
    display.setCursor(x, top + k * 11);
    display.print(lines[k]);
  }
  display.display();
}

// ================================================================
//  REACTIONS
// ================================================================
void onTap() {
  nTap++;
  applyStyle(styleIdx + 1);
  eyes.blink();
  react(900);
  Serial.printf("tap #%lu\n", (unsigned long)nTap);
}

void onDoubleTap() {
  nDouble++;
  applyStyle(styleIdx + 2);
  eyes.anim_laugh();
  react(REACT_MS);
  Serial.printf("double tap #%lu\n", (unsigned long)nDouble);
}

void onShake() {
  nShake++;
  eyes.setMood(ANGRY);
  eyes.setHFlicker(ON, 4);
  eyes.anim_confused();
  react(REACT_MS);
  Serial.printf("shake #%lu\n", (unsigned long)nShake);
}

void calmDown() {
  eyes.setHFlicker(OFF);
  eyes.setVFlicker(OFF);
  eyes.setCuriosity(OFF);
  applyStyle(styleIdx);
  eyes.setPosition(DEFAULT);
}

void motion() {
  readAdxl();
  readMpu();
  mpuTapWatch();

  unsigned long now = millis();

  if (adxlAddr) {
    uint8_t src = rReg(adxlAddr, A_INT_SOURCE);
    if (src & INT_FF)        { wakeUp(); fallAnimation(); lastMotion = now; return; }
    else if (src & INT_TAP2) { wakeUp(); onDoubleTap();   lastMotion = now; return; }
    else if (src & INT_TAP1) { wakeUp(); onTap();         lastMotion = now; return; }
  }

  if (fabsf(amag - 1.0f) > SHAKE_G && now - lastShake > SHAKE_GAP_MS) {
    lastShake = now;
    wakeUp();
    onShake();
    lastMotion = now;
    return;
  }

  // any real movement at all counts as being disturbed
  if (fabsf(amag - 1.0f) > 0.12f || fabsf(gxr) + fabsf(gyr) + fabsf(gzr) > 25.0f) {
    lastMotion = now;
    wakeUp();
  }

  if (asleep || reacting()) return;

  if (now - lastMotion > DOZE_AFTER_MS) { doze(); return; }

  if      (ax >  TILT_ON && ay >  TILT_ON) eyes.setPosition(SE);
  else if (ax >  TILT_ON && ay < -TILT_ON) eyes.setPosition(NE);
  else if (ax < -TILT_ON && ay >  TILT_ON) eyes.setPosition(SW);
  else if (ax < -TILT_ON && ay < -TILT_ON) eyes.setPosition(NW);
  else if (ax >  TILT_ON)                  eyes.setPosition(E);
  else if (ax < -TILT_ON)                  eyes.setPosition(W);
  else if (ay >  TILT_ON)                  eyes.setPosition(S);
  else if (ay < -TILT_ON)                  eyes.setPosition(N);
  else                                     eyes.setPosition(DEFAULT);
}

void idleMoods() {
  if (asleep || reacting() || millis() < nextMood) return;
  nextMood = millis() + MOOD_EVERY_MS;
  static uint8_t step = 0;
  switch (++step % 4) {
    case 0: eyes.setMood(idleMood); break;
    case 1: eyes.anim_confused();   break;
    case 2: eyes.setMood(HAPPY);    break;
    case 3: eyes.anim_laugh();      break;
  }
}

// ================================================================
//  WEB
// ================================================================
const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Robot</title><style>
:root{--bg:#0b1218;--card:#131f2a;--fg:#e6eef5;--mut:#8ba0b2;--line:#233240;--acc:#2dd4bf;--accfg:#04201c}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;text-align:center}
.wrap{max-width:460px;margin:0 auto;padding:18px}
h1{font-size:22px;margin:6px 0 2px}
.sub{color:var(--mut);font-size:13px;margin-bottom:14px}
h2{font-size:15px;margin:22px 0 8px;color:var(--mut);text-transform:uppercase;letter-spacing:.08em}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin-bottom:10px}
.g{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:8px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:10px 6px}
.tile b{display:block;font-size:20px;font-variant-numeric:tabular-nums}
.tile span{font-size:11px;color:var(--mut)}
input,select{width:100%;padding:11px;border-radius:9px;border:1px solid var(--line);background:#0e1822;color:var(--fg);font:inherit;text-align:center}
button{font:inherit;font-weight:600;padding:11px 14px;border:0;border-radius:9px;background:var(--acc);color:var(--accfg);cursor:pointer;width:100%;margin-top:8px}
button.g{background:transparent;color:var(--fg);border:1px solid var(--line)}
.row{display:flex;gap:8px}.row button{margin-top:0}
.bar{height:7px;background:#0e1822;border-radius:4px;overflow:hidden;margin-top:6px}
.bar i{display:block;height:100%;background:var(--acc)}
table{width:100%;font-size:13px;font-variant-numeric:tabular-nums}
td{padding:3px 0}td:first-child{color:var(--mut);text-align:left}td:last-child{text-align:right}
#t{margin-top:10px;font-size:13px;color:var(--acc);min-height:18px}
</style></head><body><div class="wrap">
<h1>Robot</h1><div class="sub" id="sub">connected</div>

<h2>Say something</h2>
<div class="card">
  <input id="m" maxlength="80" placeholder="type a message">
  <button onclick="send()">Show it on the face</button>
</div>

<h2>Counters</h2>
<div class="g4">
  <div class="tile"><b id="c_tap">0</b><span>taps</span></div>
  <div class="tile"><b id="c_dbl">0</b><span>double</span></div>
  <div class="tile"><b id="c_fall">0</b><span>falls</span></div>
  <div class="tile"><b id="c_shk">0</b><span>shakes</span></div>
</div>
<div class="g4" style="margin-top:8px">
  <div class="tile"><b id="c_boot">0</b><span>boots</span></div>
  <div class="tile"><b id="c_msg">0</b><span>msgs</span></div>
  <div class="tile"><b id="c_mtap">0</b><span>mpu hits</span></div>
  <div class="tile"><b id="c_up">0</b><span>uptime s</span></div>
</div>

<h2>ADXL345</h2><div class="card"><table id="adxl"></table></div>
<h2>MPU6050</h2><div class="card"><table id="mpu"></table></div>

<h2>Face</h2>
<div class="card">
  <select id="style" onchange="setStyle()"></select>
  <div class="row" style="margin-top:8px">
    <button class="g" onclick="scr(0)">Face</button>
    <button class="g" onclick="scr(1)">Stats</button>
    <button class="g" onclick="scr(2)">Message</button>
  </div>
  <div class="row" style="margin-top:8px">
    <button class="g" onclick="act('/api/wake')">Wake</button>
    <button class="g" onclick="act('/api/sleep')">Sleep</button>
  </div>
</div>

<h2>System</h2><div class="card">
  <table id="sys"></table>
  <div class="bar"><i id="heapbar" style="width:0%"></i></div>
  <button class="g" onclick="if(confirm('Reboot the robot?'))act('/api/reboot')">Reboot</button>
</div>
<div id="t"></div>
</div><script>
const $=i=>document.getElementById(i);
window.esc=function(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
window.rows=function(el,o){$(el).innerHTML=Object.entries(o).map(([k,v])=>'<tr><td>'+k+'</td><td>'+esc(v)+'</td></tr>').join('')}
window.post=async function(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
window.act=async function(u){await post(u,{});$('t').textContent='done';load()}
window.scr=async function(n){await post('/api/screen',{n:n});$('t').textContent='screen changed'}
window.setStyle=async function(){await post('/api/style',{i:$('style').value});$('t').textContent='eyes changed'}
window.send=async function(){
  const m=$('m').value.trim(); if(!m){$('t').textContent='type something first';return}
  await post('/api/msg',{m:m}); $('m').value=''; $('t').textContent='sent to the face'; load();
}
let styled=false;
window.load=async function(){
  const s=await(await fetch('/api/state',{cache:'no-store'})).json();
  $('sub').textContent=s.adxl+'  |  '+s.mpu+'  |  '+(s.asleep?'dozing':'awake');
  $('c_tap').textContent=s.tap; $('c_dbl').textContent=s.dbl; $('c_fall').textContent=s.fall;
  $('c_shk').textContent=s.shake; $('c_boot').textContent=s.boots; $('c_msg').textContent=s.msgs;
  $('c_mtap').textContent=s.mtap; $('c_up').textContent=s.up;
  rows('adxl',{'X':s.ax+' g','Y':s.ay+' g','Z':s.az+' g','magnitude':s.amag+' g'});
  rows('mpu',{'X':s.mx+' g','Y':s.my+' g','Z':s.mz+' g',
              'gyro X':s.gx+' dps','gyro Y':s.gy+' dps','gyro Z':s.gz+' dps','temp':s.temp+' C'});
  rows('sys',{'free heap':s.heap+' B','used':s.heapUsed+' B','lowest ever':s.heapMin+' B',
              'sketch':s.sketch+' B','free flash':s.freeSketch+' B','chip':s.chip,'clients':s.clients});
  $('heapbar').style.width=s.heapPct+'%';
  if(!styled){ $('style').innerHTML=s.styles.map((n,i)=>'<option value="'+i+'">'+esc(n)+'</option>').join(''); styled=true; }
  $('style').value=s.style;
}
load(); setInterval(load,1200);
</script></body></html>
)HTML";

String j(const char* k, float v, int d) { return String("\"") + k + "\":" + String(v, d) + ","; }

void handleState() {
  String o = "{";
  o += j("ax", ax, 2) + j("ay", ay, 2) + j("az", az, 2) + j("amag", amag, 2);
  o += j("mx", mx, 2) + j("my", my, 2) + j("mz", mz, 2);
  o += j("gx", gxr, 1) + j("gy", gyr, 1) + j("gz", gzr, 1) + j("temp", mtemp, 1);
  o += "\"tap\":" + String(nTap) + ",\"dbl\":" + String(nDouble) + ",\"fall\":" + String(nFall) + ",";
  o += "\"shake\":" + String(nShake) + ",\"boots\":" + String(nBoots) + ",\"msgs\":" + String(nMsg) + ",";
  o += "\"mtap\":" + String(nMpuTap) + ",\"up\":" + String(millis() / 1000UL) + ",";
  o += "\"asleep\":" + String(asleep ? "true" : "false") + ",\"style\":" + String(styleIdx) + ",";
  o += "\"adxl\":\"" + String(adxlAddr ? "ADXL345 ok" : "no ADXL345") + "\",";
  o += "\"mpu\":\"" + String(mpuAddr ? "MPU6050 ok" : "no MPU6050") + "\",";

  uint32_t heap = ESP.getFreeHeap(), total = ESP.getHeapSize();
  o += "\"heap\":" + String(heap) + ",\"heapUsed\":" + String(total - heap) + ",";
  o += "\"heapMin\":" + String(ESP.getMinFreeHeap()) + ",\"heapPct\":" + String(100 - (heap * 100 / total)) + ",";
  o += "\"sketch\":" + String(ESP.getSketchSize()) + ",\"freeSketch\":" + String(ESP.getFreeSketchSpace()) + ",";
  o += "\"chip\":\"" + String(ESP.getChipModel()) + " @" + String(ESP.getCpuFreqMHz()) + "MHz\",";
  o += "\"clients\":" + String(WiFi.softAPgetStationNum()) + ",";
  o += "\"styles\":[";
  for (int i = 0; i < STYLE_COUNT; i++) { o += "\""; o += STYLES[i].name; o += "\""; if (i < STYLE_COUNT - 1) o += ","; }
  o += "]}";
  web.send(200, "application/json", o);
}

void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", PAGE); });
  web.on("/api/state", HTTP_GET, handleState);
  web.on("/api/msg", HTTP_POST, []() {
    String m = web.arg("m"); m.trim();
    if (m.length()) {
      if (m.length() > 80) m = m.substring(0, 80);
      message = m; nMsg++;
      screenMode = SCR_MSG;
      msgUntil = millis() + MSG_SHOW_MS;
      wakeUp();
      Serial.println("message: " + message);
    }
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/style", HTTP_POST, []() {
    applyStyle(web.arg("i").toInt());
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/screen", HTTP_POST, []() {
    screenMode = constrain((int)web.arg("n").toInt(), 0, 2);
    msgUntil = (screenMode == SCR_MSG) ? millis() + MSG_SHOW_MS : 0;
    web.send(200, "application/json", "{\"ok\":true}");
  });
  web.on("/api/wake",  HTTP_POST, []() { lastMotion = millis(); wakeUp(); web.send(200, "application/json", "{\"ok\":true}"); });
  web.on("/api/sleep", HTTP_POST, []() { doze(); web.send(200, "application/json", "{\"ok\":true}"); });
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

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== robot face ===");

  prefs.begin("robot", false);
  nBoots = prefs.getUInt("boots", 0) + 1;
  prefs.putUInt("boots", nBoots);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // the last argument stops Adafruit calling Wire.begin() again on the
  // default pins, which would undo the line above
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  if (!oledOk) { Serial.println("no OLED at 0x3C"); return; }
  display.setTextWrap(false);
  display.clearDisplay();
  display.display();

  startAdxl();
  startMpu();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.printf("hotspot %s / %s  ->  http://%s\n",
                AP_SSID, AP_PASS, WiFi.softAPIP().toString().c_str());
  setupWeb();

  eyes.begin(SCREEN_W, SCREEN_H, 60);
  applyStyle(0);

  // asleep to begin with, then a yawn and a proper wake up
  eyes.close();
  eyes.setMood(TIRED);
  for (int i = 0; i < 40; i++) { eyes.update(); delay(16); }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  ctr(AP_SSID, 18, 1);
  ctr("pass: password", 32, 1);
  ctr("192.168.4.1", 44, 1);
  display.display();
  delay(2600);

  asleep = true;
  wakeUp();
  lastMotion = millis();
  nextMood = millis() + MOOD_EVERY_MS;
}

void loop() {
  if (!oledOk) { delay(1000); return; }
  web.handleClient();

  static unsigned long lastPoll = 0;
  if (millis() - lastPoll > 40) {
    lastPoll = millis();
    motion();
    if (!reacting() && reactUntil) { calmDown(); reactUntil = 0; }
    idleMoods();
  }

  if (screenMode == SCR_MSG && msgUntil && millis() > msgUntil) screenMode = SCR_FACE;

  static unsigned long lastDraw = 0;
  if (screenMode == SCR_FACE) {
    eyes.update();
  } else if (millis() - lastDraw > 90) {
    lastDraw = millis();
    if (screenMode == SCR_STATS) drawStats();
    else                         drawMessage();
  }
}
