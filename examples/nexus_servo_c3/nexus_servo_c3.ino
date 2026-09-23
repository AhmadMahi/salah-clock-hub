/*
  ================================================================
   NEXUS SERVO NODE  -  ESP32-C3 + servo + RGB LED, over WiFi/MQTT
  ================================================================
   EACH WAKE it joins WiFi, connects to the broker, announces itself,
   collects anything waiting for it, acts on it, and goes back to sleep.

     payload "ON"   servo sweeps 90 -> 180 -> 90 and stops
     payload "OFF"  servo sweeps 90 ->   0 -> 90 and stops

   Nothing published while it sleeps is lost. It connects with a
   PERSISTENT SESSION and a fixed client id, so the broker holds QoS 1
   messages for it and hands them over the moment it reconnects.

   >> PUBLISH WITH QoS 1. <<  A QoS 0 message is dropped for an offline
   client, so it would only ever arrive if the node happened to be awake.

   It rests at 90 degrees. The RGB LED shows what is happening at a
   glance, and everything it does is reported on the ack topic.

   WIRING
     Servo signal .......... GPIO 4     (servo V+ to 5V, GND to GND)
     RGB LED red ........... GPIO 5
     RGB LED common ground . GPIO 6     (driven LOW, acts as ground)
     RGB LED green ......... GPIO 7
     RGB LED blue .......... GPIO 8

   If your colours come out swapped, change LED_R / LED_G / LED_B
   below - module pinouts vary.

   COLOURS
     white blip ........ awake
     blue breathing .... joining WiFi
     cyan flash ........ WiFi joined
     yellow ............ connecting to the broker
     green flash ....... subscribed, listening for anything queued
     white flash ....... a message arrived
     green bright ...... ON, servo sweeping up
     red bright ........ OFF, servo sweeping down
     blue blip ......... nothing waiting, going straight back to sleep
     red breathing ..... could not reach the network or the broker

   LIBRARIES (Library Manager)
     ArduinoMqttClient
     ESP32Servo

   The RGB LED is driven through ESP32Servo's own PWM class rather
   than the core LEDC calls, so the servo and the LED cannot end up
   fighting over the same hardware timer.

   Board: any ESP32-C3 board, ESP32 Arduino core 2.x or 3.x
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoMqttClient.h>
#include <ESP32Servo.h>

// ================================================================
//  CONFIGURATION  -  everything you need to change is here
// ================================================================

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* MQTT_HOST = "your-broker.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;
const char* MQTT_USER = "YOUR_MQTT_USER";       // needs subscribe AND publish
const char* MQTT_PASS = "YOUR_MQTT_PASSWORD";
const char* MQTT_BASE = "nexus";               // same prefix as the hub
const char* CLIENT_ID = "nexus-servo-c3";      // fixed: the broker queues against it

// ---- sleep cycle ----
// Seconds of deep sleep between wakes. Set to 0 to stay awake instead.
#define SLEEP_SECONDS      60

// How long to listen for queued messages before giving up and sleeping.
#define WAIT_FOR_MSG_MS    1500
// Once something has arrived, how long to keep listening for more.
#define WAIT_FOR_MORE_MS   700

// ---- pins ----
#define SERVO_PIN   4
#define LED_R       5
#define LED_GND     6                          // held LOW, this is the LED's ground
#define LED_G       7
#define LED_B       8

// ---- LED brightness, 0 to 255. Deliberately low; raise it if you want. ----
#define LED_BRIGHT  20

// ---- servo travel ----
#define ANGLE_REST  90
#define ANGLE_ON    180
#define ANGLE_OFF   0
#define SWEEP_STEP  2                          // degrees per step
#define SWEEP_DELAY 12                         // ms per step, lower is faster

// ================================================================

WiFiClientSecure net;
MqttClient       mqtt(net);

String topicSub()   { return String(MQTT_BASE) + "/msg"; }
String topicSubAll(){ return String(MQTT_BASE) + "/msg/#"; }
String topicAck()   { return String(MQTT_BASE) + "/ack"; }

bool     mqttUp       = false;
int      currentAngle = ANGLE_REST;
bool     gotAnything  = false;        // did anything arrive this wake?
RTC_DATA_ATTR uint32_t wakeCount = 0; // survives deep sleep

// ================================================================
//  RGB LED
// ================================================================
//  Colours below are written as normal 0-255 values and then scaled
//  down by LED_BRIGHT, so one knob dims everything at once.

ESP32PWM pwmR, pwmG, pwmB;

uint8_t dimmed(uint8_t v) {
  return (uint8_t)(((uint16_t)v * LED_BRIGHT) / 255);
}

void ledBegin() {
  pinMode(LED_GND, OUTPUT);
  digitalWrite(LED_GND, LOW);          // this pin is the LED's ground
  pwmR.attachPin(LED_R, 5000, 8);
  pwmG.attachPin(LED_G, 5000, 8);
  pwmB.attachPin(LED_B, 5000, 8);
}

void rgb(uint8_t r, uint8_t g, uint8_t b) {
  pwmR.write(dimmed(r));
  pwmG.write(dimmed(g));
  pwmB.write(dimmed(b));
}

void rgbOff() {
  pwmR.write(0);
  pwmG.write(0);
  pwmB.write(0);
}

// one breath in and out, used while we are waiting for something
void breathe(uint8_t r, uint8_t g, uint8_t b, int ms) {
  const int steps = 24;
  for (int i = 0; i < steps; i++) {
    float p = sinf(i / (float)steps * 3.1416f);          // 0 -> 1 -> 0
    rgb((uint8_t)(r * p), (uint8_t)(g * p), (uint8_t)(b * p));
    delay(ms / steps);
  }
  rgbOff();
}

void flash(uint8_t r, uint8_t g, uint8_t b, int times, int ms) {
  for (int i = 0; i < times; i++) {
    rgb(r, g, b);
    delay(ms);
    rgbOff();
    delay(ms);
  }
}

// what the LED sits at when nothing is happening: a faint green glow
void ledIdle() { rgb(0, 70, 0); }

// ================================================================
//  SERVO  (ESP32Servo)
// ================================================================

Servo servo;

void servoGoto(int deg) {
  deg = constrain(deg, 0, 180);
  servo.write(deg);
  currentAngle = deg;
}

// Smooth sweep from where we are to the target
void servoSweep(int target) {
  int step = (target > currentAngle) ? SWEEP_STEP : -SWEEP_STEP;
  for (int a = currentAngle; (step > 0) ? (a < target) : (a > target); a += step) {
    servoGoto(a);
    delay(SWEEP_DELAY);
  }
  servoGoto(target);
  delay(150);
}

void servoBegin() {
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2500);   // 0.5 ms to 2.5 ms covers 0 to 180
  servo.write(ANGLE_REST);              // rest at 90 the moment we power up
  currentAngle = ANGLE_REST;
  delay(700);
  servo.detach();                       // stop the pulses so it does not buzz
}

// Out to the target, back to rest, then stop driving it
void servoRun(int target) {
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2500);
  servo.write(currentAngle);            // re-assert where we are before moving
  delay(80);
  servoSweep(target);
  servoSweep(ANGLE_REST);
  delay(150);
  servo.detach();                       // parked at 90 and quiet
}

// ================================================================
//  MQTT
// ================================================================

// Everything this node has to say goes to the ack topic
void say(const String& event, const String& extra) {
  if (!mqttUp) return;
  String body = String("{\"node\":\"") + CLIENT_ID + "\",\"event\":\"" + event +
                "\",\"wake\":" + String(wakeCount) +
                ",\"angle\":" + String(currentAngle);
  if (extra.length()) body += "," + extra;
  body += "}";

  if (mqtt.beginMessage(topicAck(), false, 1)) {   // QoS 1 so it is not lost
    mqtt.print(body);
    mqtt.endMessage();
  }
  Serial.println("ack " + body);
}

void handleCommand(const String& raw) {
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();

  // tell the broker we have it, before we start moving
  say("received", "\"msg\":\"" + cmd + "\"");

  if (cmd == "ON") {
    Serial.println("command ON  -> 90 to 180 and back");
    rgb(0, 255, 0);
    servoRun(ANGLE_ON);
    say("done", "\"cmd\":\"ON\"");
    flash(0, 255, 0, 2, 90);
  } else if (cmd == "OFF") {
    Serial.println("command OFF -> 90 to 0 and back");
    rgb(255, 0, 0);
    servoRun(ANGLE_OFF);
    say("done", "\"cmd\":\"OFF\"");
    flash(255, 0, 0, 2, 90);
  } else {
    Serial.println("message (not a command): " + cmd);
    say("ignored", "\"msg\":\"" + cmd + "\"");
    flash(120, 120, 120, 1, 120);
  }
  rgbOff();
}

void onMqttMessage(int size) {
  String topic = mqtt.messageTopic();

  String payload;
  payload.reserve(size + 1);
  while (mqtt.available()) payload += (char)mqtt.read();

  Serial.println("MQTT [" + topic + "] " + payload);
  gotAnything = true;
  flash(160, 160, 160, 1, 70);                       // white flash: something arrived

  handleCommand(payload);
}

bool mqttConnect() {
  Serial.printf("MQTT connecting to %s:%d\n", MQTT_HOST, MQTT_PORT);
  rgb(140, 90, 0);                                   // yellow while connecting

  net.setInsecure();                                 // no certificate pinning
  mqtt.setId(CLIENT_ID);
  mqtt.setUsernamePassword(MQTT_USER, MQTT_PASS);
  mqtt.setCleanSession(false);                       // the broker holds messages
                                                     // for us while we sleep
  mqtt.setKeepAliveInterval(120000);
  mqtt.setConnectionTimeout(8000);
  mqtt.onMessage(onMqttMessage);

  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.printf("MQTT failed, error %d\n", mqtt.connectError());
    rgbOff();
    return false;
  }

  mqttUp = true;
  Serial.println("MQTT connected");

  mqtt.subscribe(topicSub(), 1);                     // QoS 1, so offline
  mqtt.subscribe(topicSubAll(), 1);                  // messages are queued for us
  Serial.println("subscribed to " + topicSub());

  flash(0, 200, 0, 2, 70);                           // green: subscribed
  return true;
}

// ================================================================
//  WIFI
// ================================================================

bool wifiConnect(unsigned long timeoutMs) {
  Serial.printf("WiFi joining %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    breathe(0, 0, 200, 600);                         // blue breathing: joining
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi failed");
    return false;
  }

  Serial.print("WiFi ok, IP ");
  Serial.println(WiFi.localIP());
  flash(0, 180, 180, 2, 120);                        // cyan double: WiFi joined
  return true;
}

// ================================================================
//  SETUP / LOOP
// ================================================================
//  Everything happens in setup(). The board wakes, does its round, and
//  sleeps again, so loop() only runs when SLEEP_SECONDS is 0.

void goToSleep(const char* why) {
  Serial.printf("sleeping %d s (%s)\n\n", SLEEP_SECONDS, why);

  if (mqttUp) {
    say("sleep", String("\"sec\":") + String(SLEEP_SECONDS) + ",\"why\":\"" + why + "\"");
    delay(150);                       // let the packet leave before we drop the link
    mqtt.stop();
  }

  rgbOff();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();

#if SLEEP_SECONDS > 0
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

// One full round: connect, announce, collect, act, sleep
void doRound() {
  gotAnything = false;

  if (!wifiConnect(20000)) {
    breathe(200, 0, 0, 700);
    goToSleep("no wifi");
    return;
  }

  if (!mqttConnect()) {
    breathe(200, 0, 0, 700);
    goToSleep("no broker");
    return;
  }

  // "I am awake" - this is the line you watch on the ack topic
  say("wake", "");

  // Anything published while we were asleep is delivered right after the
  // subscribe, so a short listen is enough.
  unsigned long t0 = millis();
  while (millis() - t0 < WAIT_FOR_MSG_MS) {
    mqtt.poll();
    if (gotAnything) break;
    delay(10);
  }

  if (!gotAnything) {
    Serial.println("nothing waiting");
    flash(0, 0, 180, 1, 80);          // blue blip, then straight back to sleep
    goToSleep("idle");
    return;
  }

  // something arrived and has been acted on: give a moment for more
  unsigned long t1 = millis();
  while (millis() - t1 < WAIT_FOR_MORE_MS) {
    mqtt.poll();
    delay(10);
  }

  goToSleep("done");
}

void setup() {
  Serial.begin(115200);
  delay(300);
  wakeCount++;
  Serial.printf("\n=== NEXUS servo node, wake #%lu ===\n", (unsigned long)wakeCount);

  // hand every LEDC timer to the ESP32Servo library, which then shares
  // them between the servo and the three LED channels
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  ledBegin();
  flash(60, 60, 60, 1, 90);           // white blip: awake

  servoBegin();                       // park at 90

  doRound();                          // with SLEEP_SECONDS > 0 this never returns
}

void loop() {
#if SLEEP_SECONDS == 0
  static unsigned long last = 0;
  if (millis() - last > 15000UL) {    // stay-awake mode: repeat every 15 s
    last = millis();
    wakeCount++;
    doRound();
  }
  if (mqttUp) mqtt.poll();
  delay(10);
#endif
}
