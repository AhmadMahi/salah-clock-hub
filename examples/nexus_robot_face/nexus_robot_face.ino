/*
  ================================================================
   ROBOT FACE  -  ESP32-C3 + SSD1306 + ADXL345
  ================================================================
   An emotional companion, in the spirit of the MONSTRIX build on
   MakerWorld. Animated robot eyes that idle, blink and change mood
   on their own, and react to how you handle the board.

   That project uses an MPU6050. This one uses the ADXL345 you
   already have, which does the job just as well and throws in
   hardware tap and free fall detection for free.

   HOW IT REACTS
     tilt it .............. the eyes look that way
     shake it ............. ANGRY, and it shudders
     tap it once .......... startled blink
     tap it twice ......... it laughs
     drop it .............. wide eyed panic
     leave it alone ....... cycles through moods every 6 seconds,
                            blinking and glancing around by itself

   WIRING   both devices share one I2C bus
     SDA ........... GPIO 8
     SCL ........... GPIO 9
     OLED .......... 0x3C
     ADXL345 ....... 0x53, or 0x1D if its SDO pin is tied high
     VCC ........... 3V3 on both
     GND ........... GND on both

   LIBRARIES (Library Manager)
     FluxGarage RoboEyes      by Dennis Hoelscher
     Adafruit GFX Library
     Adafruit SSD1306
   The accelerometer is read straight over I2C, no driver needed.

   If tilting left makes it look right, your board is mounted the
   other way round. There is a marked line in readMotion() to swap.

   Board: any ESP32-C3 board
  ================================================================
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FluxGarage_RoboEyes.h>

#define I2C_SDA     8
#define I2C_SCL     9
#define OLED_ADDR   0x3C
#define SCREEN_W    128
#define SCREEN_H    64

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
RoboEyes<Adafruit_SSD1306> eyes(display);

// ---- ADXL345 ----
#define ADXL_ADDR_LOW   0x53
#define ADXL_ADDR_HIGH  0x1D
#define REG_DEVID       0x00
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

#define LSB_PER_G    256.0f

// ---- personality ----
#define MOOD_EVERY_MS  6000       // swap mood this often when left alone
#define TILT_ON        0.35f      // how far to lean before the eyes follow
#define SHAKE_G        0.55f      // how hard a shake has to be
#define SHAKE_GAP_MS   600
#define REACT_MS       2500       // how long a reaction holds before idling

uint8_t adxlAddr = 0;
bool    oledOk   = false;

unsigned long nextMood = 0, reactUntil = 0, lastShake = 0;
uint8_t  idleMood = DEFAULT;
uint32_t nShake = 0, nTap = 0, nDouble = 0, nFall = 0;

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
  x = (int16_t)((b[1] << 8) | b[0]);
  y = (int16_t)((b[3] << 8) | b[2]);
  z = (int16_t)((b[5] << 8) | b[4]);
  return true;
}

bool startSensor() {
  uint8_t tryAddr[2] = { ADXL_ADDR_LOW, ADXL_ADDR_HIGH };
  for (int i = 0; i < 2; i++) {
    if (readReg(tryAddr[i], REG_DEVID) != 0xE5) continue;
    adxlAddr = tryAddr[i];

    writeReg(adxlAddr, REG_DATA_FORMAT, 0x0B);   // full resolution, +/- 16 g
    writeReg(adxlAddr, REG_THRESH_TAP, 0x28);    // 2.5 g knock
    writeReg(adxlAddr, REG_DUR,        0x10);
    writeReg(adxlAddr, REG_LATENT,     0x50);
    writeReg(adxlAddr, REG_WINDOW,     0xF0);
    writeReg(adxlAddr, REG_TAP_AXES,   0x07);
    writeReg(adxlAddr, REG_THRESH_FF,  0x07);
    writeReg(adxlAddr, REG_TIME_FF,    0x14);
    writeReg(adxlAddr, REG_INT_ENABLE, INT_SINGLE_TAP | INT_DOUBLE_TAP | INT_FREE_FALL);
    writeReg(adxlAddr, REG_POWER_CTL,  0x08);
    delay(20);
    readReg(adxlAddr, REG_INT_SOURCE);           // clear anything stale
    Serial.printf("ADXL345 ready at 0x%02X\n", adxlAddr);
    return true;
  }
  Serial.println("no ADXL345 found - the face still works, it just will not react");
  return false;
}

// ================================================================
//  Reactions
// ================================================================

void react(unsigned long holdMs) { reactUntil = millis() + holdMs; }
bool reacting() { return millis() < reactUntil; }

void onShake() {
  nShake++;
  Serial.printf("shake #%lu -> angry\n", (unsigned long)nShake);
  eyes.setMood(ANGRY);
  eyes.setHFlicker(ON, 4);          // a cross little shudder
  eyes.anim_confused();
  react(REACT_MS);
}

void onTap() {
  nTap++;
  Serial.printf("tap #%lu -> startled\n", (unsigned long)nTap);
  eyes.setMood(DEFAULT);
  eyes.setCuriosity(ON);
  eyes.blink();
  react(1200);
}

void onDoubleTap() {
  nDouble++;
  Serial.printf("double tap #%lu -> laughing\n", (unsigned long)nDouble);
  eyes.setMood(HAPPY);
  eyes.anim_laugh();
  react(REACT_MS);
}

void onFreeFall() {
  nFall++;
  Serial.printf("free fall #%lu -> panic\n", (unsigned long)nFall);
  eyes.setMood(DEFAULT);
  eyes.setWidth(42, 42);            // eyes wide open
  eyes.setHeight(44, 44);
  eyes.setVFlicker(ON, 5);
  react(REACT_MS);
}

void calmDown() {
  eyes.setHFlicker(OFF);
  eyes.setVFlicker(OFF);
  eyes.setCuriosity(OFF);
  eyes.setWidth(36, 36);            // back to the resting shape
  eyes.setHeight(36, 36);
  eyes.setPosition(DEFAULT);
  eyes.setMood(idleMood);
}

// Tilt steers where the eyes look, and the hardware flags do the rest
void readMotion() {
  if (!adxlAddr) return;

  uint8_t src = readReg(adxlAddr, REG_INT_SOURCE);
  if      (src & INT_FREE_FALL)  onFreeFall();
  else if (src & INT_DOUBLE_TAP) onDoubleTap();
  else if (src & INT_SINGLE_TAP) onTap();

  int16_t rx, ry, rz;
  if (!readXYZ(rx, ry, rz)) return;

  // >>> swap gx and gy here if the eyes look the wrong way <<<
  float gx = rx / LSB_PER_G;
  float gy = ry / LSB_PER_G;
  float gz = rz / LSB_PER_G;
  float mag = sqrtf(gx * gx + gy * gy + gz * gz);

  unsigned long now = millis();
  if (fabsf(mag - 1.0f) > SHAKE_G && now - lastShake > SHAKE_GAP_MS) {
    lastShake = now;
    onShake();
    return;
  }

  if (reacting()) return;           // mid reaction, leave the eyes alone

  // lean it and the eyes follow
  if      (gx >  TILT_ON && gy >  TILT_ON) eyes.setPosition(SE);
  else if (gx >  TILT_ON && gy < -TILT_ON) eyes.setPosition(NE);
  else if (gx < -TILT_ON && gy >  TILT_ON) eyes.setPosition(SW);
  else if (gx < -TILT_ON && gy < -TILT_ON) eyes.setPosition(NW);
  else if (gx >  TILT_ON)                  eyes.setPosition(E);
  else if (gx < -TILT_ON)                  eyes.setPosition(W);
  else if (gy >  TILT_ON)                  eyes.setPosition(S);
  else if (gy < -TILT_ON)                  eyes.setPosition(N);
  else                                     eyes.setPosition(DEFAULT);

  if (gz < -0.7f) eyes.setMood(TIRED);     // held upside down, it gives up
}

// Left alone, it works through its moods
void idleMoods() {
  if (reacting() || millis() < nextMood) return;
  nextMood = millis() + MOOD_EVERY_MS;

  static uint8_t step = 0;
  step++;
  switch (step % 6) {
    case 0: idleMood = DEFAULT; break;
    case 1: idleMood = HAPPY;   break;
    case 2: idleMood = DEFAULT; eyes.anim_confused(); break;
    case 3: idleMood = TIRED;   break;
    case 4: idleMood = HAPPY;   eyes.anim_laugh();    break;
    case 5: idleMood = ANGRY;   break;
  }
  eyes.setMood(idleMood);
  Serial.printf("mood -> %d\n", idleMood);
}

// ================================================================

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== robot face ===");

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // the last argument stops Adafruit calling Wire.begin() again on the
  // default pins, which would undo the line above
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  if (!oledOk) {
    Serial.println("no OLED at 0x3C - check wiring");
    return;
  }
  display.clearDisplay();
  display.display();

  startSensor();

  eyes.begin(SCREEN_W, SCREEN_H, 60);       // 60 fps ceiling
  eyes.setWidth(36, 36);
  eyes.setHeight(36, 36);
  eyes.setBorderradius(10, 10);
  eyes.setSpacebetween(12);
  eyes.setAutoblinker(ON, 3, 2);            // blink every 3 s, give or take 2
  eyes.setIdleMode(ON, 2, 2);               // glance around on its own

  // wake up: eyes open, a look around, then a grin
  eyes.close();
  for (int i = 0; i < 25; i++) { eyes.update(); delay(16); }
  eyes.open();
  eyes.setMood(HAPPY);
  eyes.anim_laugh();

  nextMood = millis() + MOOD_EVERY_MS;
  Serial.println("tilt it, knock it, shake it, drop it.");
}

void loop() {
  if (!oledOk) { delay(1000); return; }

  static unsigned long lastPoll = 0;
  if (millis() - lastPoll > 40) {           // the sensor does not need 60 fps
    lastPoll = millis();
    readMotion();
    if (!reacting() && reactUntil) { calmDown(); reactUntil = 0; }
    idleMoods();
  }

  eyes.update();                            // draws at its own frame rate
}
