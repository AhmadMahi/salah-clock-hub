/*
  ================================================================
   NEXUS NODE  -  ESP32-C3 + 0.96" OLED, over ESP-NOW only
  ================================================================
   This node never joins WiFi. It wakes, shows what it is doing on
   its own little screen, talks to the hub, and goes back to sleep.

   EACH WAKE
     1. eyes blink open
     2. PING   sends a random number. The hub shows it on its screen
               and answers with that number + 1.
     3. MSG    sends a line of text, which the hub stores in its
               15 slot queue.
     4. POLL   asks for anything waiting for this node, for example a
               message you typed on your phone.
     5. holds the result for a moment, eyes close, deep sleep.

   WIRING   0.96" SSD1306, I2C address 0x3C
     SDA -> GPIO 8
     SCL -> GPIO 9
     VCC -> 3V3
     GND -> GND

   LIBRARIES (Library Manager)
     Adafruit GFX Library
     Adafruit SSD1306

   THE CHANNEL
   The hub sits on your router's WiFi channel and cannot move off it.
   This node does not join WiFi, so it has no way to know that channel
   on its own. On the first wake it tries each channel until the hub
   answers, then remembers the winner in RTC memory, so later wakes
   are immediate. If the router ever changes channel, it simply scans
   again on the next wake.

   SETUP
     Board: any ESP32-C3 board (ESP32 Arduino core 2.x or 3.x)
     This is a single file. Paste it into a new sketch and upload.
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ================================================================
//  PACKET FORMAT  -  must stay byte for byte identical to
//  espnow_packet.h in the hub sketch. If you change one, change both.
// ================================================================

#define NOW_MAGIC   0x5A4Cu      // 'ZL'
#define NOW_VERSION 1

enum {
  PKT_MSG   = 1,   // a text message   node -> hub, or hub -> node
  PKT_TELEM = 2,   // temperature / humidity from a sensor node
  PKT_SYNC  = 3,   // hub -> nodes: time, prayer times, weather
  PKT_PING  = 4,   // node -> hub: "here is a number, show it and answer me"
  PKT_ACK   = 5,   // hub -> node: the reply to PING, MSG or POLL
  PKT_POLL  = 6    // node -> hub: "send me anything I have not collected"
};

struct __attribute__((packed)) NowPacket {
  uint16_t magic;           // must be NOW_MAGIC
  uint8_t  version;         // NOW_VERSION
  uint8_t  type;            // PKT_*
  uint32_t seq;             // sender's own counter
  char     from[16];        // node name, zero terminated
  char     text[101];       // message text, zero terminated
  uint8_t  reserved;
  float    temp;            // Celsius, NAN when unused
  float    hum;             // percent,  NAN when unused
  uint16_t prayer[5];       // Fajr..Isha, minutes since midnight, 0xFFFF = unknown
  uint32_t epoch;           // unix time, hub -> nodes only
};

// If this ever fails, the hub and this node disagree about the packet
// layout and they will not understand each other.
static_assert(sizeof(NowPacket) == 148, "NowPacket layout changed - update both sides");

// ---------------- settings ----------------

const char* NODE_NAME = "c3-test";      // shown next to your messages on the hub

// ---- display ----
#define HAS_DISPLAY   1                 // set to 0 to run without a screen
#define OLED_SDA      8
#define OLED_SCL      9
#define OLED_ADDR     0x3C
#define OLED_W        128
#define OLED_H        64

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
bool oledOk = false;

// Seconds of deep sleep between wakes. Set it to 0 to stay awake and repeat
// on a timer instead, which is handy when you want an unbroken Serial log.
#define DEEP_SLEEP_SECONDS  15
#define AWAKE_REPEAT_MS     15000

const int  CHANNEL_MIN   = 1;
const int  CHANNEL_MAX   = 13;          // use 11 in North America
const int  ACK_WAIT_MS   = 300;         // how long to listen for the hub
const int  MAILBOX_WAIT  = 900;         // how long to wait for polled messages

// ---------------- state ----------------

RTC_DATA_ATTR int      savedChannel = 0;   // survives deep sleep
RTC_DATA_ATTR uint32_t wakeCount    = 0;

uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

volatile bool     gotAck     = false;
volatile uint32_t ackSeq     = 0;
volatile int      ackCount   = 0;
volatile int      ackTotal   = 0;
volatile uint32_t ackEpoch   = 0;
char              ackNote[32] = {0};

volatile int      inboxCount = 0;
char              inboxFrom[4][16] = {{0}};
char              inboxText[4][64] = {{0}};
uint8_t           hubMac[6]  = {0};
bool              haveHub    = false;

uint32_t mySeq = 0;


// ================================================================
//  DISPLAY
// ================================================================
//  Every screen is built from the same centred stack:
//
//        TITLE           small, centred            y 2
//       --------         short rule                y 13
//        4821            double height, centred    y 21
//     waiting for hub    small, centred            y 41
//      matches, good     small, centred            y 52
//
//  so the one thing that matters is always in the same place.

void uiBegin() {
#if HAS_DISPLAY
  Wire.begin(OLED_SDA, OLED_SCL);
  // the last argument keeps Adafruit from calling Wire.begin() again with
  // the default pins, which would undo the line above
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, true, false);
  if (!oledOk) { Serial.println("no OLED at 0x3C"); return; }
  oled.setTextWrap(false);              // long lines clip instead of reflowing
  oled.clearDisplay();
  oled.display();
#endif
}

void uiSleepDisplay() {
#if HAS_DISPLAY
  if (!oledOk) return;
  oled.clearDisplay();
  oled.display();
  oled.ssd1306_command(SSD1306_DISPLAYOFF);   // dark while we sleep
#endif
}

#if HAS_DISPLAY

// ---- centred text helpers ----
void ctr(const char* s, int y, int size) {
  if (!s || !s[0]) return;
  int w = (int)strlen(s) * 6 * size;
  int x = (OLED_W - w) / 2;
  if (x < 0) x = 0;
  oled.setTextSize(size);
  oled.setCursor(x, y);
  oled.print(s);
}

// The headline. Double height when it fits, otherwise it quietly drops to
// single height rather than running off both edges.
void ctrBig(const char* s, int y) {
  if (!s || !s[0]) return;
  if ((int)strlen(s) * 12 <= OLED_W - 4) ctr(s, y, 2);
  else                                   ctr(s, y + 4, 1);
}

void rule(int y, int w) {
  oled.drawFastHLine((OLED_W - w) / 2, y, w, SSD1306_WHITE);
}

// ---- eyes ----
void drawEye(int cx, int cy, float open, int look) {
  const int EW = 34, EH = 30;
  int h = (int)(EH * open);

  if (h < 4) {                                  // a closed eye is just a line
    oled.fillRoundRect(cx - EW / 2, cy - 1, EW, 3, 1, SSD1306_WHITE);
    return;
  }

  int r = min(10, h / 2);
  oled.fillRoundRect(cx - EW / 2, cy - h / 2, EW, h, r, SSD1306_WHITE);

  if (h >= 16) {                                // pupil, once there is room
    oled.fillCircle(cx + look, cy, 6, SSD1306_BLACK);
    oled.fillCircle(cx + look - 2, cy - 2, 1, SSD1306_WHITE);
  }
}

void uiEyes(float open, int look) {
  oled.clearDisplay();
  drawEye(38, 32, open, look);
  drawEye(90, 32, open, look);
  oled.display();
}

void uiEyesOpen() {
  for (int i = 0; i <= 14; i++) {
    float p = i / 14.0f;
    uiEyes(1.0f - (1.0f - p) * (1.0f - p), 0);  // ease out
    delay(22);
  }
  for (int i = 0; i < 12; i++) {                // a quick look around
    uiEyes(1.0f, (int)(7 * sinf(i / 11.0f * 6.2832f)));
    delay(35);
  }
}

void uiEyesClose() {
  for (int i = 14; i >= 0; i--) {
    uiEyes(i / 14.0f, 0);
    delay(22);
  }
  delay(120);
}

// ---- the broadcast mark: a core with rings travelling outward ----
void drawBeacon(int cx, int cy, int phase) {
  oled.fillCircle(cx, cy, 3, SSD1306_WHITE);
  for (int i = 0; i < 3; i++) {
    if (i <= phase % 4) oled.drawCircle(cx, cy, 7 + i * 5, SSD1306_WHITE);
  }
}

// Wake-up mark: the beacon pulsing with the node's name under it
void uiHello(const char* name) {
  for (int f = 0; f < 10; f++) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    drawBeacon(OLED_W / 2, 26, f);
    ctr(name, 50, 1);
    oled.display();
    delay(90);
  }
}

// Scanning / waiting screen: title, beacon, one note under it
void uiBeacon(const char* title, const char* note, int phase) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  ctr(title, 2, 1);
  rule(13, 70);
  drawBeacon(OLED_W / 2, 32, phase);
  ctr(note, 52, 1);
  oled.display();
}

// The standard result card, everything centred
void uiCard(const char* title, const char* big, const char* l2, const char* l3) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  ctr(title, 2, 1);
  rule(13, 70);
  ctrBig(big, 21);
  ctr(l2, 41, 1);
  ctr(l3, 52, 1);
  oled.display();
}

// The same card, with dots filling while we wait for the hub
void uiCardWaiting(const char* title, const char* big, const char* note, int phase) {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  ctr(title, 2, 1);
  rule(13, 70);
  ctrBig(big, 21);
  ctr(note, 41, 1);

  for (int i = 0; i < 5; i++) {
    int x = OLED_W / 2 - 22 + i * 11;
    if (i <= phase % 6) oled.fillCircle(x, 56, 2, SSD1306_WHITE);
    else                oled.drawCircle(x, 56, 2, SSD1306_WHITE);
  }
  oled.display();
}

#else   // no display: the calls become no-ops

void uiEyesOpen() {}
void uiEyesClose() {}
void uiHello(const char*) {}
void uiBeacon(const char*, const char*, int) {}
void uiCard(const char*, const char*, const char*, const char*) {}
void uiCardWaiting(const char*, const char*, const char*, int) {}

#endif

// ---------------- plumbing ----------------

void fill(NowPacket& p, uint8_t type) {
  memset(&p, 0, sizeof(p));
  p.magic   = NOW_MAGIC;
  p.version = NOW_VERSION;
  p.type    = type;
  p.seq     = ++mySeq;
  strncpy(p.from, NODE_NAME, sizeof(p.from) - 1);
  p.temp = NAN;
  p.hum  = NAN;
  for (int i = 0; i < 5; i++) p.prayer[i] = 0xFFFF;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  const uint8_t* mac = info->src_addr;
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(NowPacket)) return;

  NowPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != NOW_MAGIC) return;
  p.from[sizeof(p.from) - 1] = 0;
  p.text[sizeof(p.text) - 1] = 0;

  memcpy(hubMac, mac, 6);
  haveHub = true;

  if (p.type == PKT_ACK) {
    ackSeq   = p.seq;
    ackCount = p.prayer[0];
    ackTotal = p.prayer[1];
    ackEpoch = p.epoch;
    strncpy(ackNote, p.text, sizeof(ackNote) - 1);
    gotAck   = true;
  } else if (p.type == PKT_MSG) {
    if (inboxCount < 4) {                       // keep the first few for the screen
      strncpy(inboxFrom[inboxCount], p.from, sizeof(inboxFrom[0]) - 1);
      strncpy(inboxText[inboxCount], p.text, sizeof(inboxText[0]) - 1);
    }
    inboxCount++;
    Serial.printf("   inbox: [%s] %s\n", p.from, p.text);
  } else if (p.type == PKT_SYNC) {
    Serial.printf("   sync: city=%s epoch=%lu\n", p.text, (unsigned long)p.epoch);
  }
}

void addBroadcastPeer() {
  if (esp_now_is_peer_exist(BROADCAST)) return;
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = 0;                     // 0 = follow whatever channel we set
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  esp_now_add_peer(&peer);
}

void setChannel(int ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  delay(5);
}

// Spins the waiting dots until the flag comes up or we run out of time
bool waitForAck(int waitMs, const char* title, const char* big) {
  unsigned long t0 = millis();
  int phase = 0;
  while (!gotAck && millis() - t0 < (unsigned long)waitMs) {
    if (title) uiCardWaiting(title, big, "waiting for the hub", phase++);
    delay(title ? 60 : 2);
  }
  return gotAck;
}

// Sends one packet and waits for the hub's reply
bool sendAndWait(NowPacket& p, int waitMs, const char* title = nullptr,
                 const char* big = nullptr) {
  gotAck = false;
  if (esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p)) != ESP_OK) return false;
  return waitForAck(waitMs, title, big);
}

// ---------------- the three exchanges ----------------

bool doPing(uint32_t number, int waitMs, bool show = false) {
  NowPacket p;
  fill(p, PKT_PING);
  p.seq = number;                       // the hub echoes number + 1
  char num[16];
  snprintf(num, sizeof(num), "%lu", (unsigned long)number);
  return sendAndWait(p, waitMs, show ? "PING" : nullptr, show ? num : nullptr);
}

bool doMessage(const char* text) {
  NowPacket p;
  fill(p, PKT_MSG);
  strncpy(p.text, text, sizeof(p.text) - 1);
  return sendAndWait(p, ACK_WAIT_MS, "SEND", "MSG");
}

// Asks the hub for anything this node has not collected yet
int doPoll() {
  inboxCount = 0;

  NowPacket p;
  fill(p, PKT_POLL);
  gotAck = false;
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));

  // messages arrive first, then the ack telling us how many there were
  waitForAck(MAILBOX_WAIT, "INBOX", "ASK");

  return gotAck ? ackCount : -1;
}

// Tries the remembered channel first, then sweeps. Returns the channel or 0.
int findHub() {
  char note[24];
  int phase = 0;

  if (savedChannel >= CHANNEL_MIN && savedChannel <= CHANNEL_MAX) {
    snprintf(note, sizeof(note), "channel %d", savedChannel);
    uiBeacon("LOOKING FOR HUB", note, phase++);
    setChannel(savedChannel);
    if (doPing(esp_random(), ACK_WAIT_MS)) return savedChannel;
    Serial.printf("channel %d went quiet, scanning...\n", savedChannel);
  }

  for (int ch = CHANNEL_MIN; ch <= CHANNEL_MAX; ch++) {
    snprintf(note, sizeof(note), "channel %d", ch);
    uiBeacon("SCANNING", note, phase++);
    setChannel(ch);
    if (doPing(esp_random(), 120)) {
      Serial.printf("found the hub on channel %d\n", ch);
      return ch;
    }
  }
  return 0;
}

// ---------------- one full visit ----------------

void talkToHub() {
  char big[20], l2[24], l3[24];

  int ch = findHub();
  if (ch == 0) {
    Serial.println("no answer on any channel.");
    Serial.println("  - is the hub powered up and joined to WiFi?");
    Serial.println("  - ESP-NOW only starts once the hub is on the network");
    uiCard("NO HUB", "----", "is it on WiFi?", "trying again later");
    delay(2200);
    savedChannel = 0;
    return;
  }
  savedChannel = ch;
  snprintf(l2, sizeof(l2), "channel %d", ch);
  uiCard("LINKED", "HUB", l2, "");
  delay(800);

  // ---- 1. ping with a random number
  uint32_t n = esp_random() % 9000 + 1000;       // a readable 4 digit number
  Serial.printf("ping  %lu ...\n", (unsigned long)n);
  if (doPing(n, ACK_WAIT_MS, true)) {
    bool ok = (ackSeq == n + 1);
    Serial.printf("  hub replied %lu  (expected %lu)  %s\n",
                  (unsigned long)ackSeq, (unsigned long)(n + 1), ok ? "OK" : "MISMATCH");
    snprintf(big, sizeof(big), "%lu", (unsigned long)ackSeq);
    snprintf(l2,  sizeof(l2),  "sent %lu", (unsigned long)n);
    uiCard("REPLY", big, l2, ok ? "link is good" : "MISMATCH");
  } else {
    Serial.println("  no reply");
    uiCard("REPLY", "----", "no answer", "");
  }
  delay(1600);

  // ---- 2. send something for the hub to store
  char line[64];
  snprintf(line, sizeof(line), "C3 awake, wake #%lu", (unsigned long)wakeCount);
  Serial.printf("send  \"%s\" ...\n", line);
  if (doMessage(line)) {
    Serial.printf("  hub says %s, holding %d\n", ackNote, ackTotal);
    snprintf(l2, sizeof(l2), "hub says %s", ackNote);
    snprintf(l3, sizeof(l3), "queue holds %d", ackTotal);
    uiCard("SENT", "OK", l2, l3);
  } else {
    Serial.println("  no reply");
    uiCard("SENT", "----", "no answer", "");
  }
  delay(1600);

  // ---- 3. collect anything waiting for us
  Serial.println("poll  ...");
  int got = doPoll();

  if (got < 0) {
    Serial.println("  no reply");
    uiCard("INBOX", "----", "no answer", "");
    delay(1800);
  } else if (got == 0) {
    // nothing queued, but the hub still says something back
    Serial.printf("  nothing waiting, hub says \"%s\"\n", ackNote);
    uiCard("HUB SAYS", ackNote[0] ? ackNote : "hey", "no messages waiting", "");
    delay(2200);
  } else {
    Serial.printf("  received %d message(s)\n", got);
    int show = min(got, 4);
    for (int i = 0; i < show; i++) {
      snprintf(big, sizeof(big), "%d/%d", i + 1, got);
      // split the text over the two small lines so longer notes still fit
      char a1[22] = {0}, a2[22] = {0};
      strncpy(a1, inboxText[i], 21);
      if (strlen(inboxText[i]) > 21) strncpy(a2, inboxText[i] + 21, 21);
      char hdr[22];
      snprintf(hdr, sizeof(hdr), "FROM %s", inboxFrom[i]);
      uiCard(hdr, big, a1, a2);
      delay(2400);
    }
  }

  // ---- hold the last screen for a moment before the eyes close
  delay(2000);
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  delay(300);
  wakeCount++;

  Serial.printf("\n=== NEXUS node %s, wake #%lu ===\n", NODE_NAME, (unsigned long)wakeCount);

  uiBegin();
  uiEyesOpen();                         // good morning
  uiHello(NODE_NAME);                   // beacon mark + who we are

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                    // ESP-NOW only, never join a network
  esp_wifi_set_ps(WIFI_PS_NONE);        // power save would drop replies

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    uiCard("ERROR", "ESPNOW", "init failed", "");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  addBroadcastPeer();

  Serial.printf("my MAC %s\n", WiFi.macAddress().c_str());

  talkToHub();

  uiEyesClose();                        // good night

#if DEEP_SLEEP_SECONDS > 0
  Serial.printf("sleeping %d s\n\n", DEEP_SLEEP_SECONDS);
  Serial.flush();
  uiSleepDisplay();
  esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void loop() {
#if DEEP_SLEEP_SECONDS == 0
  static unsigned long last = 0;
  if (millis() - last >= AWAKE_REPEAT_MS) {
    last = millis();
    wakeCount++;
    Serial.printf("\n--- round %lu ---\n", (unsigned long)wakeCount);
    uiEyesOpen();
    uiHello(NODE_NAME);
    talkToHub();
    uiEyesClose();
  }
  delay(20);
#endif
}
