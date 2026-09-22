# NEXUS - ESP32 master hub

A 128x64 OLED hub on an ESP32. It takes messages in from the internet and
from other ESP32 nodes over ESP-NOW, keeps the last 15 of them, and passes
them back out to whichever nodes want them. It also broadcasts the time,
prayer times and weather so every node can show the same data, and it updates
its own firmware from this repo's releases.

The Salah clock is what it shows while it is idle.

## What it shows

**Idle face** - the time, centred, and one scrolling line:

```
        10:45 PM
              32

 TUE 22 SEP | 28 C | 52% humidity | Asr 4:12 PM in 1h 12m |
 2 new messages | Bengaluru | signal -47 dBm
```

**Every minute** it slides over to the message queue, cycles through what has
arrived, and slides back. An empty queue just says `No messages`.

The next-prayer and weather cards are still there as extra slides, switched
off by default because the scrolling line already carries both. Turn them on
under **Screen** in the web panel.

**At prayer time** an alert takes over: a pulsing announcement, then a Quran
verse. Fajr and Maghrib add a sunrise or sunset animation and a reminder to
read the morning or evening duas.

## Hardware

| | |
| --- | --- |
| Board | Any ESP32 dev board |
| Display | SH1106 128x64 I2C |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

## First run and WiFi

On boot the hub joins the network saved in its flash. If it cannot connect,
or nothing is saved yet, it opens its own setup access point and shows this
on the display:

```
            WIFI SETUP
 ------------------------------
 JOIN   NEXUS-A1B2
 PASS   nexus1234
 OPEN   192.168.4.1
```

Join that network from a phone, and the setup page opens by itself (it is a
captive portal). Pick your network from the scanned list, enter the password,
and the hub saves it and restarts.

Credentials live in flash, not in the firmware, so **they survive every
update**. While the portal is open the hub also retries the saved network
every two minutes, so a router reboot heals itself without you touching
anything.

To move the hub to a different network later, use **Change WiFi network**
under **Network** in the web panel.

## Flashing

**From a release (easiest)** — download `salah_clock_hub.bin` from
[Releases](../../releases) and flash it at offset `0x10000`, or use the
[ESP web flasher](https://espressif.github.io/esptool-js/). After the first
flash the clock updates itself.

**From source**

1. Install the **U8g2** and **ArduinoJson** libraries.
2. Optionally copy `secrets.example.h` to `secrets.h` and put your WiFi
   details in it. This only seeds a device that has nothing saved yet, so
   the first boot connects without visiting the setup portal. `secrets.h` is
   git ignored, so your password stays on your machine and never reaches the
   published firmware.
3. Set **Tools > Partition Scheme > Minimal SPIFFS (1.9MB APP with OTA)**.
   This matters: schemes without an OTA slot cannot self-update.
4. Upload.

Arduino wants the folder name to match the sketch name. If you cloned this
repo as `salah-clock-hub`, either rename the folder to `salah_clock` or let
the IDE move the sketch for you when it offers.

## Automatic updates

On boot, and once a day after that, the clock asks GitHub for the latest
release. If the tag is newer than the `FW_VERSION` it is running, it
downloads the firmware, shows a progress bar and reboots into it. A failed
update changes nothing; the running firmware stays put.

You can turn this off, or trigger a check by hand, under **Firmware** in the
web panel.

### Publishing a new version

1. Bump `FW_VERSION` in `salah_clock.ino`.
2. Commit, then tag with the same number and push:

```bash
git tag v3.1.1 && git push origin v3.1.1
```

GitHub Actions builds the firmware, checks the tag matches `FW_VERSION`, and
attaches `salah_clock_hub.bin` to the release. Every clock picks it up on its
next check.

## Web panel

`http://salah-clock.local`, or the IP shown during boot.

Prayer times and location, messages, ESP-NOW nodes, weather, alerts, firmware,
and panel calibration are all configurable there.

## Panel calibration

Many SH1106 modules have unreadable rows at the top, and text on the very last
rows sits awkwardly against the bezel. **Top safe rows** (default 10) and
**bottom safe rows** (default 8) fence off both ends; everything is then laid
out evenly inside what is left, never pushed against either edge.

Use **Show safe area** in the panel to see the boundaries on the display with
a pixel ruler, then adjust until every line is sharp.

## ESP-NOW nodes

`examples/nexus_node_c3/` is a complete ESP32-C3 node that never joins WiFi:
it wakes, pings the hub with a random number (which the hub shows on screen
and answers with number plus one), sends a message for the queue, collects
anything waiting for it, and sleeps. It finds the hub's channel by itself.

See [NODE_EXAMPLE.md](NODE_EXAMPLE.md) for that sketch and the packet format. Copy `espnow_packet.h` to each node so both sides agree on
the layout.

The hub sits on your router's WiFi channel and cannot move off it, so every
node must use that same channel. The panel shows it under **Nodes**, and the
example node scans for it automatically.

## HTTP API

```bash
# show a message on the clock, and forward it to every node
curl -X POST http://salah-clock.local/api/message \
  -d "message=Dinner is ready" -d "from=kitchen" -d "share=1"
```

`GET /api/state` returns everything as JSON.

## Credits

Prayer times from [AlAdhan](https://aladhan.com/prayer-times-api),
weather from [Open-Meteo](https://open-meteo.com/),
display driven by [U8g2](https://github.com/olikraus/u8g2).
