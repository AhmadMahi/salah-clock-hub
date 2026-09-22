# Talking to the Salah Clock hub over ESP-NOW

The clock is the **master hub**. Any number of ESP32 boards ("nodes") can
send it messages and sensor readings, and the hub broadcasts messages and a
sync packet back out to all of them.

## Ready-made node sketch

`examples/nexus_node_c3/` is a complete ESP32-C3 node that never joins WiFi.
Open it, set `NODE_NAME`, and flash. Each wake it:

1. **pings** the hub with a random number. The hub shows that number on its
   display and answers with the number plus one.
2. **sends** a line of text, which the hub stores in its 15 slot queue.
3. **polls** for anything waiting for it, such as a message you typed on your
   phone, and the hub hands those over.

It finds the hub's channel by itself on the first wake and remembers it in
RTC memory, so later wakes are immediate.

Leave `DEEP_SLEEP_SECONDS` at `0` for the first test: the node stays awake and
repeats the exchange every 15 seconds so you can watch it in the Serial
Monitor. Set it to `30` once you have seen it work, and it will deep sleep
between wakes instead.

Expected output:

```
=== NEXUS node c3-test, wake #1 ===
my MAC A0:B7:65:11:22:33
found the hub on channel 6
ping  4821 ...
  hub replied 4822  (expected 4822)  OK
send  "C3 awake, wake #1" ...
  hub says stored, holding 3
poll  ...
   inbox: [panel] Dinner is ready
  received 1 message(s)
```

## 1. Match the channel


ESP-NOW only works when both boards are on the same WiFi channel. The hub is
joined to your router, so it sits on the router's channel and cannot move.

1. Open the web panel and look at **Nodes**. It shows the hub MAC and the
   channel, for example `channel 6`.
2. Set that same channel on every node with `esp_wifi_set_channel(...)`, as
   in the sketch below.
3. If you change your router's channel, re-read the panel and update the nodes.

The node sketch above does this scan for you. The rest of this page is for
writing your own node from scratch.

## 2. Copy the packet header

Copy `espnow_packet.h` from this folder into your node's sketch folder. Both
sides must use the exact same struct.

## 3. Node sketch (sender + receiver)

Create a new sketch folder, drop `espnow_packet.h` beside it, and flash this.

```cpp
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "espnow_packet.h"

// ---- set these two ----
const int   HUB_CHANNEL = 6;          // from the hub web panel, Nodes section
const char* NODE_NAME   = "kitchen";  // shows up next to your message

uint8_t BROADCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint32_t seq = 0;

void fill(NowPacket& p, uint8_t type) {
  memset(&p, 0, sizeof(p));
  p.magic   = NOW_MAGIC;
  p.version = NOW_VERSION;
  p.type    = type;
  p.seq     = ++seq;
  strncpy(p.from, NODE_NAME, sizeof(p.from) - 1);
  p.temp = NAN;
  p.hum  = NAN;
  for (int i = 0; i < 5; i++) p.prayer[i] = 0xFFFF;
}

// Send a text message. It lands in the hub's 15 slot queue and pops up
// on the OLED straight away.
void sendMessage(const char* text) {
  NowPacket p;
  fill(p, PKT_MSG);
  strncpy(p.text, text, sizeof(p.text) - 1);
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));
}

// Send a temperature / humidity reading. The hub shows it on the weather
// card and stops using the internet forecast.
void sendTelemetry(float tempC, float humPct) {
  NowPacket p;
  fill(p, PKT_TELEM);
  p.temp = tempC;
  p.hum  = humPct;
  esp_now_send(BROADCAST, (uint8_t*)&p, sizeof(p));
}

// Anything the hub broadcasts arrives here: relayed messages and the
// once a minute sync packet with time, prayer times and weather.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != sizeof(NowPacket)) return;
  NowPacket p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != NOW_MAGIC) return;
  p.from[15] = 0;
  p.text[100] = 0;

  if (p.type == PKT_MSG) {
    Serial.printf("Message from %s: %s\n", p.from, p.text);
  } else if (p.type == PKT_SYNC) {
    Serial.printf("Sync from %s  city=%s  epoch=%lu\n",
                  p.from, p.text, (unsigned long)p.epoch);
    for (int i = 0; i < 5; i++) {
      if (p.prayer[i] != 0xFFFF) {
        Serial.printf("  prayer %d = %02d:%02d\n", i, p.prayer[i] / 60, p.prayer[i] % 60);
      }
    }
    if (!isnan(p.temp)) Serial.printf("  temp %.1f C  hum %.0f %%\n", p.temp, p.hum);
  }
}

void setup() {
  Serial.begin(115200);

  // Station mode, NOT joined to WiFi, parked on the hub's channel
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(HUB_CHANNEL, WIFI_SECOND_CHAN_NONE);
  WiFi.setSleep(false);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = HUB_CHANNEL;
  peer.encrypt = false;
  peer.ifidx   = WIFI_IF_STA;
  esp_now_add_peer(&peer);

  Serial.printf("Node %s ready on channel %d\n", NODE_NAME, HUB_CHANNEL);
  sendMessage("Kitchen node is online");
}

void loop() {
  // type a line in the Serial Monitor to send it to the clock
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.length()) sendMessage(line.c_str());
  }

  // send a reading every 5 minutes (swap in your real sensor)
  static unsigned long last = 0;
  if (millis() - last > 300000UL) {
    last = millis();
    // sendTelemetry(readTemp(), readHum());
  }

  delay(10);
}
```

## 4. Quick test without the header

The hub also accepts a bare ASCII string, so a two line test works:

```cpp
const char* msg = "hello from node";
esp_now_send(BROADCAST, (uint8_t*)msg, strlen(msg));
```

It is stored as a message from `node`.

## 5. Sending from the internet or a script

The hub exposes an HTTP endpoint, so anything on your network can post to it:

```bash
curl -X POST http://salah-clock.local/api/message \
  -d "message=Dinner is ready" -d "from=home-assistant" -d "share=1"
```

`share=1` also forwards the message to every ESP-NOW node. A plain GET works
too, which is handy for IFTTT style services:

```
http://salah-clock.local/api/message?message=Guests%20arriving&share=1
```

## Packet reference

| Field | Meaning |
| --- | --- |
| `magic` | always `NOW_MAGIC` (`0x5A4C`), packets without it are treated as plain text |
| `version` | `NOW_VERSION` |
| `type` | `PKT_MSG`, `PKT_TELEM`, `PKT_SYNC`, `PKT_PING` |
| `seq` | your own counter, used to spot retries |
| `from` | node name, up to 15 characters |
| `text` | message text, up to 100 characters |
| `temp` / `hum` | Celsius and percent, `NAN` when unused |
| `prayer[5]` | Fajr to Isha as minutes since midnight, `0xFFFF` when unknown |
| `epoch` | unix time, filled by the hub only |

## Behaviour notes

- The queue holds **15 messages**, newest first. Number 16 pushes the oldest out.
- An identical message from the same sender inside 5 seconds is ignored, so
  ESP-NOW retries do not create duplicates.
- **Relay node messages** in the panel makes the hub rebroadcast whatever a
  node sends, so every other node sees it too.
- **Share time and prayer times** broadcasts `PKT_SYNC` once a minute.
- A node sending `PKT_TELEM` overrides the online forecast for 15 minutes.
