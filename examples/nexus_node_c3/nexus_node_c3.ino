/*
  ================================================================
   NEXUS NODE  -  ESP32-C3 over ESP-NOW only
  ================================================================
   This node never joins WiFi. It wakes, talks to the hub, and goes
   back to sleep.

   Each wake it does three things:
     1. PING   sends a random number. The hub shows it on its screen
               and answers with that number + 1.
     2. MSG    sends a line of text, which the hub stores in its
               15 slot queue.
     3. POLL   asks for anything waiting for this node, for example a
               message you typed on your phone. The hub replies with
               each one, then says how many it sent.

   THE CHANNEL
   The hub sits on your router's WiFi channel and cannot move off it.
   This node does not join WiFi, so it has no way to know that channel
   on its own. On the first wake it tries each channel until the hub
   answers, then remembers the winner in RTC memory, so later wakes
   are immediate. If the router ever changes channel, it simply scans
   again on the next wake.

   SETUP
     Board: any ESP32-C3 board (ESP32 Arduino core 2.x or 3.x)
     This is a single file. Nothing else to copy, no libraries to
     install beyond the ESP32 core itself. Paste it into a new sketch
     and upload.
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

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

// 0 keeps the node awake and repeats the whole exchange on a timer, which is
// the easiest way to watch it work in the Serial Monitor. Set it to 30 (or
// whatever you like) to deep sleep between wakes instead.
#define DEEP_SLEEP_SECONDS  0
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
uint8_t           hubMac[6]  = {0};
bool              haveHub    = false;

uint32_t mySeq = 0;

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

// Sends one packet and waits for the hub's reply
bool sendAndWait(NowPacket& p, int waitMs) {
  gotAck = false;
  if (esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p)) != ESP_OK) return false;

  unsigned long t0 = millis();
  while (!gotAck && millis() - t0 < (unsigned long)waitMs) delay(2);
  return gotAck;
}

// ---------------- the three exchanges ----------------

bool doPing(uint32_t number, int waitMs) {
  NowPacket p;
  fill(p, PKT_PING);
  p.seq = number;                       // the hub echoes number + 1
  return sendAndWait(p, waitMs);
}

bool doMessage(const char* text) {
  NowPacket p;
  fill(p, PKT_MSG);
  strncpy(p.text, text, sizeof(p.text) - 1);
  return sendAndWait(p, ACK_WAIT_MS);
}

// Asks the hub for anything this node has not collected yet
int doPoll() {
  inboxCount = 0;

  NowPacket p;
  fill(p, PKT_POLL);
  gotAck = false;
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));

  // messages arrive first, then the ack telling us how many there were
  unsigned long t0 = millis();
  while (!gotAck && millis() - t0 < (unsigned long)MAILBOX_WAIT) delay(2);

  return gotAck ? ackCount : -1;
}

// Tries the remembered channel first, then sweeps. Returns the channel or 0.
int findHub() {
  if (savedChannel >= CHANNEL_MIN && savedChannel <= CHANNEL_MAX) {
    setChannel(savedChannel);
    if (doPing(esp_random(), ACK_WAIT_MS)) return savedChannel;
    Serial.printf("channel %d went quiet, scanning...\n", savedChannel);
  }

  for (int ch = CHANNEL_MIN; ch <= CHANNEL_MAX; ch++) {
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
  int ch = findHub();
  if (ch == 0) {
    Serial.println("no answer on any channel.");
    Serial.println("  - is the hub powered up and joined to WiFi?");
    Serial.println("  - ESP-NOW only starts once the hub is on the network");
    savedChannel = 0;
    return;
  }
  savedChannel = ch;

  // 1. ping with a random number
  uint32_t n = esp_random() % 9000 + 1000;     // a readable 4 digit number
  Serial.printf("ping  %lu ...\n", (unsigned long)n);
  if (doPing(n, ACK_WAIT_MS)) {
    Serial.printf("  hub replied %lu  (expected %lu)  %s\n",
                  (unsigned long)ackSeq, (unsigned long)(n + 1),
                  ackSeq == n + 1 ? "OK" : "MISMATCH");
  } else {
    Serial.println("  no reply");
  }

  // 2. send something for the hub to store
  char line[64];
  snprintf(line, sizeof(line), "C3 awake, wake #%lu", (unsigned long)wakeCount);
  Serial.printf("send  \"%s\" ...\n", line);
  if (doMessage(line)) Serial.printf("  hub says %s, holding %d\n", ackNote, ackTotal);
  else                 Serial.println("  no reply");

  // 3. collect anything waiting for us
  Serial.println("poll  ...");
  int got = doPoll();
  if (got < 0)      Serial.println("  no reply");
  else if (got == 0) Serial.println("  nothing waiting");
  else               Serial.printf("  received %d message(s)\n", got);
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  delay(300);
  wakeCount++;

  Serial.printf("\n=== NEXUS node %s, wake #%lu ===\n", NODE_NAME, (unsigned long)wakeCount);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();                    // ESP-NOW only, never join a network
  esp_wifi_set_ps(WIFI_PS_NONE);        // power save would drop replies

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);
  addBroadcastPeer();

  Serial.printf("my MAC %s\n", WiFi.macAddress().c_str());

  talkToHub();

#if DEEP_SLEEP_SECONDS > 0
  Serial.printf("sleeping %d s\n\n", DEEP_SLEEP_SECONDS);
  Serial.flush();
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
    talkToHub();
  }
  delay(20);
#endif
}
