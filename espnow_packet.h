/*
  ================================================================
   ESP-NOW PACKET FORMAT  -  Salah Clock Hub
  ================================================================
   Copy this file to every node so the hub and the nodes agree on
   the layout, byte for byte. 148 bytes, well under the 250 byte
   ESP-NOW limit.

   The hub accepts two things:
     1. a NowPacket (recommended, gives you a sender name,
        sensor readings and message types)
     2. a plain ASCII string, which is stored as a message from
        "node". Handy for a two line test sketch.
  ================================================================
*/

#ifndef ESPNOW_PACKET_H
#define ESPNOW_PACKET_H

#include <Arduino.h>

#define NOW_MAGIC   0x5A4Cu      // 'ZL'
#define NOW_VERSION 1

enum {
  PKT_MSG   = 1,   // text message   node -> hub, or hub -> nodes
  PKT_TELEM = 2,   // temperature / humidity from a sensor node
  PKT_SYNC  = 3,   // hub -> nodes: time, prayer times, weather
  PKT_PING  = 4    // node announcing itself
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

#endif
