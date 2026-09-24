/*
  ================================================================
   ADXL345 CHECK  -  ESP32-C3 + SSD1306
  ================================================================
   A standalone test. No WiFi, no MQTT, nothing else running.
   It scans the I2C bus, checks the ADXL345 identifies itself,
   then shows live X, Y and Z on the OLED and over Serial, and
   says SHAKE when you shake it.

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
   The accelerometer is read straight over I2C, so it needs no
   library of its own.

   WHAT TO LOOK FOR
     The first screen lists every address that answered. If 0x53
     (or 0x1D) is missing, it is wiring, not code. The sketch then
     reads the chip's ID register, which must come back as 0xE5.

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
#define ADXL_ADDR_LOW   0x53        // SDO tied low, the usual case
#define ADXL_ADDR_HIGH  0x1D        // SDO tied high
#define REG_DEVID       0x00        // always reads 0xE5 on a real ADXL345
#define REG_POWER_CTL   0x2D
#define REG_DATA_FORMAT 0x31
#define REG_DATAX0      0x32

#define LSB_PER_G   256.0f          // 4 mg per count in full resolution mode

// ---- shake detection ----
#define SHAKE_G        0.55f        // how far from 1 g counts as a shake
#define SHAKE_HOLD_MS  600          // how long SHAKE stays on the screen
#define SHAKE_GAP_MS   350          // ignore repeats closer together than this

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

uint8_t  adxlAddr = 0;              // 0 until we find it
bool     oledOk   = false;
uint32_t shakes   = 0;
unsigned long shakeUntil = 0, lastShake = 0;

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

// Reads all six data bytes in one go, as the datasheet asks
bool readXYZ(int16_t& x, int16_t& y, int16_t& z) {
  Wire.beginTransmission(adxlAddr);
  Wire.write(REG_DATAX0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)adxlAddr, 6) != 6) return false;

  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();

  x = (int16_t)((b[1] << 8) | b[0]);       // little endian
  y = (int16_t)((b[3] << 8) | b[2]);
  z = (int16_t)((b[5] << 8) | b[4]);
  return true;
}

// ================================================================
//  Startup: scan the bus, then wake the sensor
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
    if (id == 0xE5) {
      adxlAddr = tryAddr[i];
      writeReg(adxlAddr, REG_DATA_FORMAT, 0x0B);   // full resolution, +/- 16 g
      writeReg(adxlAddr, REG_POWER_CTL,   0x08);   // leave standby, start measuring
      delay(20);
      Serial.printf("ADXL345 ready at 0x%02X\n", adxlAddr);
      return true;
    }
  }
  Serial.println("no ADXL345 found (device id should be 0xE5)");
  return false;
}

// ================================================================
//  Screens
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
    oled.print("id 0xE5 confirmed");
  } else {
    oled.print("ADXL345 NOT FOUND");
    oled.setCursor(0, 44);
    oled.print("check wiring");
  }
  oled.display();
}

void showReading(float gx, float gy, float gz, float mag, bool shaking) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  if (shaking) {                       // take over the screen briefly
    centreText("SHAKE!", 14, 3);
    char c[16];
    snprintf(c, sizeof(c), "count %lu", (unsigned long)shakes);
    centreText(c, 46, 1);
    oled.display();
    return;
  }

  char line[24];
  oled.setTextSize(1);

  snprintf(line, sizeof(line), "ADXL345 0x%02X", adxlAddr);
  oled.setCursor(0, 0);
  oled.print(line);
  snprintf(line, sizeof(line), "#%lu", (unsigned long)shakes);
  oled.setCursor(OLED_W - (int)strlen(line) * 6, 0);
  oled.print(line);
  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);

  oled.setCursor(0, 15);  oled.printf("X %+6.2f g", gx);
  oled.setCursor(0, 26);  oled.printf("Y %+6.2f g", gy);
  oled.setCursor(0, 37);  oled.printf("Z %+6.2f g", gz);

  snprintf(line, sizeof(line), "|%.2f| g", mag);
  oled.setCursor(OLED_W - (int)strlen(line) * 6, 46);
  oled.print(line);

  // bar: 1 g sits about a third along, so a shake is obvious
  int w = (int)(mag / 3.0f * (OLED_W - 4));
  w = constrain(w, 0, OLED_W - 4);
  oled.drawRect(0, 55, OLED_W, 9, SSD1306_WHITE);
  if (w > 0) oled.fillRect(2, 57, w, 5, SSD1306_WHITE);

  oled.display();
}

// ================================================================

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== ADXL345 check ===");

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
}

void loop() {
  if (!adxlAddr) {                     // nothing to read, keep saying so
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

  // At rest the magnitude is about 1 g, which is gravity. Moving it
  // hard pushes that well above or below 1.
  unsigned long now = millis();
  if (fabsf(mag - 1.0f) > SHAKE_G && now - lastShake > SHAKE_GAP_MS) {
    lastShake  = now;
    shakeUntil = now + SHAKE_HOLD_MS;
    shakes++;
    Serial.printf("SHAKE  #%lu  (%.2f g)\n", (unsigned long)shakes, mag);
  }

  showReading(gx, gy, gz, mag, now < shakeUntil);

  // Printed so it also works in Tools > Serial Plotter
  static unsigned long lastPrint = 0;
  if (now - lastPrint > 200) {
    lastPrint = now;
    Serial.printf("X:%.2f Y:%.2f Z:%.2f MAG:%.2f\n", gx, gy, gz, mag);
  }

  delay(40);
}
