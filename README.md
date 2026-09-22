# Salah Clock Hub

An ESP32 prayer clock on a 128x64 SH1106 OLED that doubles as an ESP-NOW
master hub: it collects messages and sensor readings from other ESP32 nodes,
keeps a 15 message queue, and broadcasts time, prayer times and weather back
out to them. It updates its own firmware from this repo's releases.

## What it shows

**Clock face**

```
 10:45 PM
 32
 TUE 22 SEP                      ((•  -58
 28 C | 52% humidity | Asr 4:12 PM in 1h 12m | 2 new messages | Bengaluru
```

The big time sits at the top, the date and signal strength share a small row,
and a single line scrolls along the bottom with temperature, humidity, the
next prayer, unread messages and your city.

**Every few minutes** three cards slide past, then it returns to the clock:

1. **Next prayer** in large type with a countdown
2. **Temperature and humidity**
3. **Message queue**, newest first, cycling

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

## Flashing

**From a release (easiest)** — download `salah_clock_hub.bin` from
[Releases](../../releases) and flash it at offset `0x10000`, or use the
[ESP web flasher](https://espressif.github.io/esptool-js/). After the first
flash the clock updates itself.

**From source**

1. Install the **U8g2** and **ArduinoJson** libraries.
2. Copy `secrets.example.h` to `secrets.h` and put your WiFi details in it.
   `secrets.h` is git ignored, so your password stays on your machine.
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

See [NODE_EXAMPLE.md](NODE_EXAMPLE.md) for a ready to flash node sketch and
the packet format. Copy `espnow_packet.h` to each node so both sides agree on
the layout.

The hub sits on your router's WiFi channel and cannot move off it, so every
node must use that same channel. The panel shows it under **Nodes**.

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
