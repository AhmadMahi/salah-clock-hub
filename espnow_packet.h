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
  PKT_MSG   = 1,   // a text message   node -> hub, or hub -> node
  PKT_TELEM = 2,   // temperature / humidity from a sensor node
  PKT_SYNC  = 3,   // hub -> nodes: time, prayer times, weather
  PKT_PING  = 4,   // node -> hub: "here is a number, show it and answer me"
  PKT_ACK   = 5,   // hub -> node: the reply to PING, MSG or POLL
  PKT_POLL  = 6    // node -> hub: "send me anything I have not collected"
};

/*
  HOW A NODE TALKS TO THE HUB

  PING   set seq to any number. The hub shows it on its screen and
         replies PKT_ACK with seq = your number + 1. Proves the link.

  MSG    set text. The hub stores it in its 15 slot queue and replies
         PKT_ACK with seq = your seq, echoed back.

  POLL   send an empty PKT_POLL. The hub unicasts every message you
         have not collected yet, oldest first, up to 5 per poll, then
         one PKT_ACK. Each node is tracked separately, so two nodes
         never steal each other's messages and nothing arrives twice.

  ACK    from the hub, always carries:
           seq        your number + 1 after PING, your seq after MSG,
                      0 after POLL
           prayer[0]  the count that matters: messages the hub holds
                      after PING/MSG, messages just sent to you after
                      POLL
           prayer[1]  how many messages the hub holds in total
           epoch      unix time, or 0 if the hub has no time yet
           text       a short word: "pong", "stored" or "delivered".
                      After a POLL that found nothing, this is a short
                      greeting instead, so the node always has a line
                      to show.
*/

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

#endif
