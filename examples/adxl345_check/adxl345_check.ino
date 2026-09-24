/*
  ================================================================
   ADXL345 PLAYGROUND  -  ESP32-C3 + SSD1306
  ================================================================
   Standalone. No WiFi, no MQTT. Scans the I2C bus, checks the
   sensor identifies itself, then turns the OLED into a live
   bubble level that reacts to how you hold and knock the board.

   WHAT IT PICKS UP
     tilt        LEFT, RIGHT, FORWARD, BACK, LEVEL, UPSIDE DOWN
     shake       a good rattle in any direction
     tap         one sharp knock          (sensor hardware)
     double tap  two knocks in a row      (sensor hardware)
     free fall   drop it, or let it fall a few centimetres onto
                 something soft           (sensor hardware)

   Tap and free fall are detected by the ADXL345 itself, not by
   watching the numbers, so they catch spikes far shorter than the
   loop could ever see. No interrupt wire needed: the sketch just
   reads the chip's interrupt source register.

   WIRING   both devices share one I2C bus
     SDA ........... GPIO 8
     SCL ........... GPIO 9
     OLED .......... 0x3C
     ADXL345 ....... 0x53, or 0x1D if its SDO pin is tied high
     VCC ........... 3V3 on both
     GND ........... GND on both

   LIBRARIES
     Adafruit GFX Library
     Adafruit SSD1306
   The accelerometer is read straight over I2C, no driver needed.

   If LEFT and FORWARD feel swapped, your board is simply mounted
   a different way round. Swap gx and gy where noted in gesture().

   Board: any ESP32-C3 board
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define I2C_SDA     8
#define I2C_SCL     9
#define OLED_ADDR   0x3C
#define OLED_W      128
#define OLED_H      64

// ---- ADXL345 registers ----
#define ADXL_ADDR_LOW   0x53
#define ADXL_ADDR_HIGH  0x1D
#define REG_DEVID       0x00        // always 0xE5 on a real ADXL345
#define REG_THRESH_TAP  0x1D
#define REG_THRESH_FF   0x28
#define REG_TIME_FF     0x29
#define REG_DUR         0x21
#define REG_LATENT      0x22
#define REG_WINDOW      0x23
#define REG_TAP_AXES    0x2A
#define REG_POWER_CTL   0x2D
#define REG_INT_ENABLE  0x2E
#define REG_INT_SOURCE  0x30
#define REG_DATA_FORMAT 0x31
#define REG_DATAX0      0x32

#define INT_SINGLE_TAP  0x40
#define INT_DOUBLE_TAP  0x20
#define INT_FREE_FALL   0x04

#define LSB_PER_G    256.0f         // 4 mg per count in full resolution mode

// ---- how touchy each gesture is ----
#define TILT_ON      0.35f          // tilt past this to register a direction
#define TILT_OFF     0.22f          // and back inside this to reset it
#define FLIP_Z      -0.70f          // z below this means it is upside down
#define SHAKE_G      0.55f          // how far from 1 g counts as a shake
#define SHAKE_GAP_MS 350
#define BANNER_MS    700            // how long a gesture takes over the screen

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

uint8_t adxlAddr = 0;
bool    oledOk   = false;

uint32_t nShake = 0, nTap = 0, nDouble = 0, nFall = 0, nTilt = 0;
unsigned long lastShake = 0, bannerUntil = 0;
char     banner[16]  = "";
char     tiltWord[14] = "LEVEL";
char     lastTilt[14] = "LEVEL";

// ================================================================
//  I2C helpers
// ================================================================

void writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t readReg(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((int)addr, 1) != 1) return 0;
  return Wire.read();
}

bool readXYZ(int16_t& x, int16_t& y, int16_t& z) {
  Wire.beginTransmission(adxlAddr);
  Wire.write(REG_DATAX0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)adxlAddr, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  x = (int16_t)((b[1] << 8) | b[0]);          // little endian
  y = (int16_t)((b[3] << 8) | b[2]);
  z = (int16_t)((b[5] << 8) | b[4]);
  return true;
}

// ================================================================
//  Startup
// ================================================================

String scanBus() {
  String found = "";
  int n = 0;
  Serial.println("\nscanning I2C...");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      char buf[8];
      snprintf(buf, sizeof(buf), "0x%02X ", a);
      Serial.printf("  found %s\n", buf);
      found += buf;
      n++;
    }
  }
  if (!n) Serial.println("  nothing answered. check SDA, SCL, power and ground.");
  return n ? found : String("none");
}

bool startSensor() {
  uint8_t tryAddr[2] = { ADXL_ADDR_LOW, ADXL_ADDR_HIGH };

  for (int i = 0; i < 2; i++) {
    uint8_t id = readReg(tryAddr[i], REG_DEVID);
    Serial.printf("  0x%02X device id = 0x%02X\n", tryAddr[i], id);
    if (id != 0xE5) continue;

    adxlAddr = tryAddr[i];
    writeReg(adxlAddr, REG_DATA_FORMAT, 0x0B);    // full resolution, +/- 16 g

    // ---- hardware tap detection ----
    writeReg(adxlAddr, REG_THRESH_TAP, 0x28);     // 40 * 62.5 mg  = 2.5 g
    writeReg(adxlAddr, REG_DUR,        0x10);     // 16 * 625 us   = 10 ms max
    writeReg(adxlAddr, REG_LATENT,     0x50);     // 80 * 1.25 ms  = 100 ms gap
    writeReg(adxlAddr, REG_WINDOW,     0xF0);     // 240 * 1.25 ms = 300 ms window
    writeReg(adxlAddr, REG_TAP_AXES,   0x07);     // listen on X, Y and Z

    // ---- hardware free fall detection ----
    writeReg(adxlAddr, REG_THRESH_FF,  0x07);     // 7 * 62.5 mg = 437 mg
    writeReg(adxlAddr, REG_TIME_FF,    0x14);     // 20 * 5 ms   = 100 ms

    writeReg(adxlAddr, REG_INT_ENABLE, INT_SINGLE_TAP | INT_DOUBLE_TAP | INT_FREE_FALL);
    writeReg(adxlAddr, REG_POWER_CTL,  0x08);     // leave standby, start measuring
    delay(20);
    readReg(adxlAddr, REG_INT_SOURCE);            // clear anything left over

    Serial.printf("ADXL345 ready at 0x%02X, tap and free fall armed\n", adxlAddr);
    return true;
  }
  Serial.println("no ADXL345 found (device id should be 0xE5)");
  return false;
}

// ================================================================
//  Gestures
// ================================================================

void fire(const char* name, uint32_t& counter) {
  counter++;
  strncpy(banner, name, sizeof(banner) - 1);
  banner[sizeof(banner) - 1] = 0;
  bannerUntil = millis() + BANNER_MS;
  Serial.printf(">> %s  (shake %lu, tap %lu, double %lu, fall %lu)\n",
                name, (unsigned long)nShake, (unsigned long)nTap,
                (unsigned long)nDouble, (unsigned long)nFall);
}

// Which way is it leaning? Hysteresis stops it flickering on the edge.
void gesture(float gx, float gy, float gz) {
  // >>> swap gx and gy here if left/right and forward/back feel swapped <<<
  const char* w = tiltWord;

  if (gz < FLIP_Z) {
    w = "UPSIDE DOWN";
  } else if (gx >  TILT_ON) { w = "RIGHT";
  } else if (gx < -TILT_ON) { w = "LEFT";
  } else if (gy >  TILT_ON) { w = "BACK";
  } else if (gy < -TILT_ON) { w = "FORWARD";
  } else if (fabsf(gx) < TILT_OFF && fabsf(gy) < TILT_OFF) {
    w = "LEVEL";
  }

  if (strcmp(w, tiltWord) != 0) {
    strncpy(tiltWord, w, sizeof(tiltWord) - 1);
    tiltWord[sizeof(tiltWord) - 1] = 0;
    if (strcmp(w, "LEVEL") != 0) {
      nTilt++;
      strncpy(lastTilt, w, sizeof(lastTilt) - 1);
      Serial.printf("tilt: %s\n", w);
    }
  }
}

// The sensor's own interrupt flags. Reading the register clears them.
void hardwareEvents() {
  uint8_t src = readReg(adxlAddr, REG_INT_SOURCE);
  if (src & INT_FREE_FALL)      fire("FREE FALL", nFall);
  else if (src & INT_DOUBLE_TAP) fire("DOUBLE TAP", nDouble);
  else if (src & INT_SINGLE_TAP) fire("TAP", nTap);
}

// ================================================================
//  Screen
// ================================================================

void centreText(const char* s, int y, int size) {
  int w = (int)strlen(s) * 6 * size;
  oled.setTextSize(size);
  oled.setCursor(w < OLED_W ? (OLED_W - w) / 2 : 0, y);
  oled.print(s);
}

void showBootScreen(const String& found) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  centreText("I2C SCAN", 0, 1);
  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);

  oled.setCursor(0, 16);
  oled.print("found: ");
  oled.print(found);

  oled.setCursor(0, 32);
  if (adxlAddr) {
    oled.printf("ADXL345 OK @0x%02X", adxlAddr);
    oled.setCursor(0, 44);
    oled.print("tilt tap shake fall");
  } else {
    oled.print("ADXL345 NOT FOUND");
    oled.setCursor(0, 44);
    oled.print("check wiring");
  }
  oled.display();
}

// A gesture just fired: take the whole screen for a moment
void showBanner() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  int size = (strlen(banner) > 8) ? 1 : 2;
  centreText(banner, size == 2 ? 20 : 26, size);
  char c[24];
  snprintf(c, sizeof(c), "shake %lu  tap %lu", (unsigned long)nShake, (unsigned long)nTap);
  centreText(c, 50, 1);
  oled.display();
}

void showLive(float gx, float gy, float gz, float mag) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  // header: how it is being held right now
  oled.setCursor(0, 0);
  oled.print(tiltWord);
  char c[12];
  snprintf(c, sizeof(c), "%.2fg", mag);
  oled.setCursor(OLED_W - (int)strlen(c) * 6, 0);
  oled.print(c);
  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);

  // bubble level: the dot chases gravity, so it sits dead centre when flat
  const int BX = 2, BY = 14, BW = 58, BH = 40;
  oled.drawRect(BX, BY, BW, BH, SSD1306_WHITE);
  int ccx = BX + BW / 2, ccy = BY + BH / 2;
  oled.drawFastHLine(ccx - 4, ccy, 9, SSD1306_WHITE);
  oled.drawFastVLine(ccx, ccy - 4, 9, SSD1306_WHITE);

  int dx = (int)(gx * (BW / 2 - 5));
  int dy = (int)(gy * (BH / 2 - 5));
  dx = constrain(dx, -(BW / 2 - 5), BW / 2 - 5);
  dy = constrain(dy, -(BH / 2 - 5), BH / 2 - 5);
  oled.fillCircle(ccx + dx, ccy + dy, 4, SSD1306_WHITE);

  // numbers down the right
  oled.setCursor(66, 15); oled.printf("X%+5.2f", gx);
  oled.setCursor(66, 26); oled.printf("Y%+5.2f", gy);
  oled.setCursor(66, 37); oled.printf("Z%+5.2f", gz);
  oled.setCursor(66, 48); oled.print(lastTilt);

  // running totals along the bottom
  oled.setCursor(0, 56);
  oled.printf("S%lu T%lu D%lu F%lu", (unsigned long)nShake, (unsigned long)nTap,
              (unsigned long)nDouble, (unsigned long)nFall);
  oled.display();
}

// ================================================================

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== ADXL345 playground ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // the last argument stops Adafruit calling Wire.begin() again on the
  // default pins, which would undo the line above
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  Serial.println(oledOk ? "OLED ready" : "no OLED at 0x3C");
  if (oledOk) oled.setTextWrap(false);

  String found = scanBus();
  startSensor();
  showBootScreen(found);
  delay(2500);

  Serial.println("\ntilt it, knock it, shake it, drop it.");
}

void loop() {
  if (!adxlAddr) {
    showBootScreen("none");
    delay(1000);
    return;
  }

  int16_t rx, ry, rz;
  if (!readXYZ(rx, ry, rz)) {
    Serial.println("read failed");
    delay(200);
    return;
  }

  float gx = rx / LSB_PER_G;
  float gy = ry / LSB_PER_G;
  float gz = rz / LSB_PER_G;
  float mag = sqrtf(gx * gx + gy * gy + gz * gz);

  hardwareEvents();
  gesture(gx, gy, gz);

  unsigned long now = millis();
  if (fabsf(mag - 1.0f) > SHAKE_G && now - lastShake > SHAKE_GAP_MS) {
    lastShake = now;
    fire("SHAKE", nShake);
  }

  if (now < bannerUntil) showBanner();
  else                   showLive(gx, gy, gz, mag);

  // also readable in Tools > Serial Plotter
  static unsigned long lastPrint = 0;
  if (now - lastPrint > 250) {
    lastPrint = now;
    Serial.printf("X:%.2f Y:%.2f Z:%.2f MAG:%.2f\n", gx, gy, gz, mag);
  }

  delay(35);
}
