/*
  ================================================================
   NEXUS SERVO NODE  -  ESP32-C3 + servo + RGB LED, over WiFi/MQTT
  ================================================================
   Joins your WiFi, connects to the same MQTT broker as the hub, and
   listens on the same message topic.

     payload "ON"   servo sweeps 90 -> 180 -> 90 and stops
     payload "OFF"  servo sweeps 90 ->   0 -> 90 and stops

   It rests at 90 degrees and holds there. The RGB LED shows what is
   happening at a glance.

   WIRING
     Servo signal .......... GPIO 4     (servo V+ to 5V, GND to GND)
     RGB LED red ........... GPIO 5
     RGB LED common ground . GPIO 6     (driven LOW, acts as ground)
     RGB LED green ......... GPIO 7
     RGB LED blue .......... GPIO 8

   If your colours come out swapped, change LED_R / LED_G / LED_B
   below - module pinouts vary.

   COLOURS
     blue breathing .... joining WiFi
     cyan flash ........ WiFi joined
     yellow ............ connecting to the broker
     green dim ......... subscribed and idle, waiting for commands
     white flash ....... a message arrived
     green bright ...... ON, servo sweeping up
     red bright ........ OFF, servo sweeping down
     red breathing ..... lost the network or the broker

   LIBRARIES
     ArduinoMqttClient   (Library Manager)
     Nothing else. The servo is driven straight from LEDC, so no
     servo library is required.

   Board: any ESP32-C3 board, ESP32 Arduino core 2.x or 3.x
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoMqttClient.h>

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
const char* CLIENT_ID = "nexus-servo-c3";      // must differ from every other client

// ---- pins ----
#define SERVO_PIN   4
#define LED_R       5
#define LED_GND     6                          // held LOW, this is the LED's ground
#define LED_G       7
#define LED_B       8

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
String topicPub()   { return String(MQTT_BASE) + "/servo"; }

bool          mqttUp       = false;
unsigned long nextMqttTry  = 0;
int           currentAngle = ANGLE_REST;

// ================================================================
//  RGB LED
// ================================================================

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define LED_ATTACH(pin)      ledcAttach(pin, 5000, 8)
  #define LED_WRITE(pin, duty) ledcWrite(pin, duty)
#else
  static int _ledCh = 0;
  #define LED_ATTACH(pin)      do { ledcSetup(_ledCh, 5000, 8); ledcAttachPin(pin, _ledCh); _ledCh++; } while (0)
  #define LED_WRITE(pin, duty) ledcWrite(pin == LED_R ? 0 : (pin == LED_G ? 1 : 2), duty)
#endif

void ledBegin() {
  pinMode(LED_GND, OUTPUT);
  digitalWrite(LED_GND, LOW);          // this pin is the LED's ground
  LED_ATTACH(LED_R);
  LED_ATTACH(LED_G);
  LED_ATTACH(LED_B);
}

void rgb(uint8_t r, uint8_t g, uint8_t b) {
  LED_WRITE(LED_R, r);
  LED_WRITE(LED_G, g);
  LED_WRITE(LED_B, b);
}

void rgbOff() { rgb(0, 0, 0); }

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

// what the LED sits at when nothing is happening
void ledIdle() { rgb(0, 24, 0); }                        // dim green: listening

// ================================================================
//  SERVO, driven straight from LEDC
// ================================================================
//  50 Hz, 16 bit. A 0.5 ms pulse is 0 degrees, 2.5 ms is 180.

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define SERVO_ATTACH()      ledcAttach(SERVO_PIN, 50, 16)
  #define SERVO_WRITE(duty)   ledcWrite(SERVO_PIN, duty)
  #define SERVO_DETACH()      ledcDetach(SERVO_PIN)
#else
  #define SERVO_CH 3
  #define SERVO_ATTACH()      do { ledcSetup(SERVO_CH, 50, 16); ledcAttachPin(SERVO_PIN, SERVO_CH); } while (0)
  #define SERVO_WRITE(duty)   ledcWrite(SERVO_CH, duty)
  #define SERVO_DETACH()      ledcDetachPin(SERVO_PIN)
#endif

uint32_t angleToDuty(int deg) {
  deg = constrain(deg, 0, 180);
  int us = map(deg, 0, 180, 500, 2500);               // pulse width in microseconds
  return (uint32_t)((uint64_t)us * 65536ULL / 20000ULL);  // 20 ms period
}

void servoGoto(int deg) {
  SERVO_WRITE(angleToDuty(deg));
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
  delay(120);
}

void servoBegin() {
  SERVO_ATTACH();
  servoGoto(ANGLE_REST);              // rest at 90 the moment we power up
  delay(600);
  SERVO_DETACH();                     // stop the pulses so it does not buzz
}

// Out to the target, back to rest, then stop driving it
void servoRun(int target) {
  SERVO_ATTACH();
  servoGoto(currentAngle);            // re-assert where we are before moving
  delay(60);
  servoSweep(target);
  servoSweep(ANGLE_REST);
  SERVO_DETACH();                     // parked at 90 and quiet
}

// ================================================================
//  MQTT
// ================================================================

void publishState(const char* cmd, const char* state) {
  if (!mqttUp) return;
  String body = String("{\"node\":\"") + CLIENT_ID + "\",\"cmd\":\"" + cmd +
                "\",\"angle\":" + String(currentAngle) + ",\"state\":\"" + state + "\"}";
  if (mqtt.beginMessage(topicPub())) {
    mqtt.print(body);
    mqtt.endMessage();
  }
  Serial.println("pub " + topicPub() + " " + body);
}

void handleCommand(const String& raw) {
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();

  if (cmd == "ON") {
    Serial.println("command ON  -> 90 to 180 and back");
    rgb(0, 255, 0);                                  // bright green while moving
    publishState("ON", "moving");
    servoRun(ANGLE_ON);
    publishState("ON", "done");
    flash(0, 255, 0, 2, 90);
  } else if (cmd == "OFF") {
    Serial.println("command OFF -> 90 to 0 and back");
    rgb(255, 0, 0);                                  // bright red while moving
    publishState("OFF", "moving");
    servoRun(ANGLE_OFF);
    publishState("OFF", "done");
    flash(255, 0, 0, 2, 90);
  } else {
    Serial.println("message (not a command): " + cmd);
    flash(120, 120, 120, 1, 120);                    // white blip, nothing to do
  }
  ledIdle();
}

void onMqttMessage(int size) {
  String topic = mqtt.messageTopic();

  String payload;
  payload.reserve(size + 1);
  while (mqtt.available()) payload += (char)mqtt.read();

  Serial.println("MQTT [" + topic + "] " + payload);
  flash(160, 160, 160, 1, 70);                       // white flash: something arrived

  handleCommand(payload);
}

bool mqttConnect() {
  Serial.printf("MQTT connecting to %s:%d\n", MQTT_HOST, MQTT_PORT);
  rgb(140, 90, 0);                                   // yellow while connecting

  net.setInsecure();                                 // no certificate pinning
  mqtt.setId(CLIENT_ID);
  mqtt.setUsernamePassword(MQTT_USER, MQTT_PASS);
  mqtt.setKeepAliveInterval(60000);
  mqtt.setConnectionTimeout(8000);
  mqtt.onMessage(onMqttMessage);

  if (!mqtt.connect(MQTT_HOST, MQTT_PORT)) {
    Serial.printf("MQTT failed, error %d\n", mqtt.connectError());
    rgbOff();
    return false;
  }

  mqttUp = true;
  Serial.println("MQTT connected");

  mqtt.subscribe(topicSub(), 1);
  mqtt.subscribe(topicSubAll(), 1);
  Serial.println("subscribed to " + topicSub());

  flash(0, 200, 0, 3, 80);                           // green triple: subscribed
  publishState("boot", "ready");
  ledIdle();
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

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== NEXUS servo node ===");

  ledBegin();
  flash(60, 60, 60, 1, 150);                         // brief white: alive

  servoBegin();                                      // park at 90

  if (!wifiConnect(30000)) {
    // keep trying, the loop will pick it up
  }
}

void loop() {
  // ---- WiFi ----
  if (WiFi.status() != WL_CONNECTED) {
    mqttUp = false;
    breathe(200, 0, 0, 800);                         // red breathing: offline
    static unsigned long lastTry = 0;
    if (millis() - lastTry > 15000UL) {
      lastTry = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    return;
  }

  // ---- MQTT ----
  if (!mqttUp) {
    if (millis() >= nextMqttTry) {
      nextMqttTry = millis() + 10000UL;
      if (!mqttConnect()) breathe(200, 0, 0, 600);
    }
    return;
  }

  if (!mqtt.connected()) {
    Serial.println("MQTT lost");
    mqttUp = false;
    nextMqttTry = millis() + 3000UL;
    return;
  }

  mqtt.poll();
  delay(5);
}
