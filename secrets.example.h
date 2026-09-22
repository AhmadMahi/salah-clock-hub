// Copy this file to secrets.h and fill in your own details.
// secrets.h is git ignored, so nothing here reaches the repo or the
// published firmware. Everything is also editable from the web panel,
// and what you set there is stored in flash and survives updates.
#pragma once

#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// MQTT broker. The hub needs publish rights if you want it to report
// delivery confirmations back to the broker; subscribe alone is enough
// for receiving messages.
#define MQTT_HOST "your-broker.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USER "YOUR_MQTT_USER"
#define MQTT_PASS "YOUR_MQTT_PASSWORD"
#define MQTT_BASE "nexus"
