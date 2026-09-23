/*
  ================================================================
   NEXUS SERVO NODE  -  ESP32-C3 + servo + RGB LED
  ================================================================
   Normally it sleeps. Each wake it joins WiFi, connects to the
   broker, announces itself, collects whatever is waiting, acts on
   it, and sleeps again.

     "ON"       servo sweeps rest -> 180 -> rest and stops
     "ON 37"    same, then turns itself OFF 37 minutes later
     "OFF"      servo sweeps rest ->   0 -> rest and stops
     "CONFIG"   stays awake and serves a settings page instead

   HOW IT SLEEPS  (auto mode)
     counting down   5 minute hops while 5 or more minutes remain,
                     then 1 minute hops. "ON 37" becomes seven 5s and
                     two 1s, then it turns itself off.
     after auto off  20 minutes for 5 hours, then 10 minutes for 5
                     hours, then 8 minutes for ever.
     after manual off, or plain ON, or nothing at all
                     every 10 minutes.
   Every one of those numbers is editable on the config page. Manual
   mode replaces the whole thing with a fixed cycle: awake one minute,
   asleep one minute, so commands land almost straight away. There is
   nothing to configure for it.

   Nothing published while it sleeps is lost. It connects with a
   PERSISTENT SESSION and a fixed client id, so the broker holds
   QoS 1 messages for it and hands them over on the next connect.

   >> PUBLISH WITH QoS 1. <<  A QoS 0 message is dropped for an
   offline client, so it would only arrive if the node were awake.

   CONFIG MODE
   Publish "CONFIG" to the subscribe topic. On its next wake the node
   stays up, publishes its IP to the ack topic, and serves a page
   where you can change the sleep time, the topics, the servo angles,
   the LED brightness and the WiFi and broker details. Save and it
   reboots straight back into the normal cycle.

   WIRING
     Servo signal .......... GPIO 4     (servo V+ to 5V, GND to GND)
     RGB LED red ........... GPIO 5
     RGB LED common ground . GPIO 6     (driven LOW, acts as ground)
     RGB LED green ......... GPIO 7
     RGB LED blue .......... GPIO 8

   COLOURS
     white blip ........ awake
     blue breathing .... joining WiFi
     cyan flash ........ WiFi joined
     yellow ............ connecting to the broker
     green flash ....... subscribed
     white flash ....... a message arrived
     green bright ...... ON, servo sweeping up
     red bright ........ OFF, servo sweeping down
     blue blip ......... nothing waiting, going back to sleep
     magenta breathing . config mode, web page is up
     red breathing ..... could not reach the network or the broker

   LIBRARIES (Library Manager)
     ArduinoMqttClient
     ESP32Servo

   Board: any ESP32-C3 board, ESP32 Arduino core 2.x or 3.x
   If it does not fit, choose Tools > Partition Scheme > Minimal SPIFFS.
  ================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoMqttClient.h>
#include <ESP32Servo.h>

// ================================================================
//  DEFAULTS  -  only used on a fresh board. After that the values
//  saved from the config page win.
// ================================================================

#define DEF_WIFI_SSID   "YOUR_WIFI_NAME"
#define DEF_WIFI_PASS   "YOUR_WIFI_PASSWORD"

#define DEF_MQTT_HOST   "your-broker.s1.eu.hivemq.cloud"
#define DEF_MQTT_PORT   8883
#define DEF_MQTT_USER   "YOUR_MQTT_USER"
#define DEF_MQTT_PASS   "YOUR_MQTT_PASSWORD"

#define DEF_SUB_TOPIC   "nexus/msg"
#define DEF_PUB_TOPIC   "nexus/ack"
#define DEF_CLIENT_ID   "nexus-servo-c3"

#define DEF_MODE        0         // 0 = auto schedule, 1 = manual
#define DEF_LISTEN_MS   1500      // how long to wait for a queued message

// Manual mode is deliberately fixed and has nothing to configure:
// awake for a minute so commands land almost at once, asleep for a minute.
#define MANUAL_AWAKE_SEC 60
#define MANUAL_SLEEP_SEC 60

// ---- auto schedule, all in minutes unless noted ----
#define DEF_NORMAL_MIN  10        // idle interval when nothing special is going on
#define DEF_STEP_LONG   5         // hop size while plenty of time remains
#define DEF_STEP_SHORT  1         // hop size near the end
#define DEF_SHORT_BELOW 5         // switch to short hops below this many minutes
#define DEF_D1_MIN      20        // after an automatic off: first tier
#define DEF_D1_HOURS    5
#define DEF_D2_MIN      10        // second tier
#define DEF_D2_HOURS    5
#define DEF_D3_MIN      8         // and this one for ever
#define DEF_ANGLE_REST  90
#define DEF_ANGLE_ON    180
#define DEF_ANGLE_OFF   0
#define DEF_LED_BRIGHT  20        // 0 to 255, deliberately low

// ---- pins ----
#define SERVO_PIN   4
#define LED_R       5
#define LED_GND     6             // held LOW, this is the LED's ground
#define LED_G       7
#define LED_B       8

#define SWEEP_STEP       2        // degrees per step
#define SWEEP_DELAY     12        // ms per step, lower is faster
#define WAIT_FOR_MORE_MS 700      // keep listening after something arrives

// ================================================================
//  SETTINGS, loaded from flash
// ================================================================

Preferences prefs;

String cfgWifiSsid, cfgWifiPass;
String cfgMqttHost, cfgMqttUser, cfgMqttPass;
int    cfgMqttPort;
String cfgSubTopic, cfgPubTopic, cfgClientId;
int    cfgMode, cfgListenMs;
int    cfgNormalMin, cfgStepLong, cfgStepShort, cfgShortBelow;
int    cfgD1Min, cfgD1Hours, cfgD2Min, cfgD2Hours, cfgD3Min;
int    cfgAngleRest, cfgAngleOn, cfgAngleOff;
int    cfgLedBright;
bool   cfgResetSession;           // clear stale subscriptions once after a topic change

void loadSettings() {
  prefs.begin("servo", false);
  cfgWifiSsid = prefs.getString("wssid", DEF_WIFI_SSID);
  cfgWifiPass = prefs.getString("wpass", DEF_WIFI_PASS);
  cfgMqttHost = prefs.getString("mhost", DEF_MQTT_HOST);
  cfgMqttPort = prefs.getInt   ("mport", DEF_MQTT_PORT);
  cfgMqttUser = prefs.getString("muser", DEF_MQTT_USER);
  cfgMqttPass = prefs.getString("mpass", DEF_MQTT_PASS);
  cfgSubTopic = prefs.getString("sub",   DEF_SUB_TOPIC);
  cfgPubTopic = prefs.getString("pub",   DEF_PUB_TOPIC);
  cfgClientId = prefs.getString("cid",   DEF_CLIENT_ID);
  cfgMode     = prefs.getInt   ("mode",  DEF_MODE);
  cfgListenMs = prefs.getInt   ("listen",DEF_LISTEN_MS);
  cfgNormalMin  = prefs.getInt("nmin",  DEF_NORMAL_MIN);
  cfgStepLong   = prefs.getInt("slong", DEF_STEP_LONG);
  cfgStepShort  = prefs.getInt("sshort",DEF_STEP_SHORT);
  cfgShortBelow = prefs.getInt("sbelow",DEF_SHORT_BELOW);
  cfgD1Min      = prefs.getInt("d1m",   DEF_D1_MIN);
  cfgD1Hours    = prefs.getInt("d1h",   DEF_D1_HOURS);
  cfgD2Min      = prefs.getInt("d2m",   DEF_D2_MIN);
  cfgD2Hours    = prefs.getInt("d2h",   DEF_D2_HOURS);
  cfgD3Min      = prefs.getInt("d3m",   DEF_D3_MIN);
  cfgAngleRest= prefs.getInt   ("arest", DEF_ANGLE_REST);
  cfgAngleOn  = prefs.getInt   ("aon",   DEF_ANGLE_ON);
  cfgAngleOff = prefs.getInt   ("aoff",  DEF_ANGLE_OFF);
  cfgLedBright= prefs.getInt   ("led",   DEF_LED_BRIGHT);
  cfgResetSession = prefs.getBool("rsess", false);

  cfgMqttPort  = constrain(cfgMqttPort, 1, 65535);
  cfgListenMs  = constrain(cfgListenMs, 300, 30000);
  cfgAngleRest = constrain(cfgAngleRest, 0, 180);
  cfgAngleOn   = constrain(cfgAngleOn,   0, 180);
  cfgAngleOff  = constrain(cfgAngleOff,  0, 180);
  cfgLedBright = constrain(cfgLedBright, 1, 255);
  cfgMode       = constrain(cfgMode, 0, 1);
  cfgNormalMin  = constrain(cfgNormalMin, 1, 1440);
  cfgStepLong   = constrain(cfgStepLong, 1, 240);
  cfgStepShort  = constrain(cfgStepShort, 1, 240);
  cfgShortBelow = constrain(cfgShortBelow, 1, 240);
  cfgD1Min      = constrain(cfgD1Min, 1, 1440);
  cfgD2Min      = constrain(cfgD2Min, 1, 1440);
  cfgD3Min      = constrain(cfgD3Min, 1, 1440);
  cfgD1Hours    = constrain(cfgD1Hours, 0, 240);
  cfgD2Hours    = constrain(cfgD2Hours, 0, 240);
  if (cfgSubTopic.length() == 0) cfgSubTopic = DEF_SUB_TOPIC;
  if (cfgPubTopic.length() == 0) cfgPubTopic = DEF_PUB_TOPIC;
  if (cfgClientId.length() == 0) cfgClientId = DEF_CLIENT_ID;
}

// ================================================================

WiFiClientSecure net;
MqttClient       mqtt(net);
WebServer        web(80);

bool     mqttUp      = false;
bool     configMode  = false;
bool     gotAnything = false;
int      currentAngle = DEF_ANGLE_REST;
String   lastCommand  = "none";

// ---- state that has to survive deep sleep ----
enum { ST_NORMAL = 0, ST_COUNTDOWN = 1, ST_DECAY = 2 };

RTC_DATA_ATTR uint32_t wakeCount   = 0;
RTC_DATA_ATTR int      rtcState    = ST_NORMAL;
RTC_DATA_ATTR int32_t  rtcRemain   = 0;    // seconds left before the automatic off
RTC_DATA_ATTR int32_t  rtcSinceOff = 0;    // seconds since that automatic off

const char* stateName() {
  switch (rtcState) {
    case ST_COUNTDOWN: return "countdown";
    case ST_DECAY:     return "decay";
    default:           return "normal";
  }
}

// ================================================================
//  RGB LED
// ================================================================

ESP32PWM pwmR, pwmG, pwmB;

uint8_t dimmed(uint8_t v) { return (uint8_t)(((uint16_t)v * cfgLedBright) / 255); }

void ledBegin() {
  pinMode(LED_GND, OUTPUT);
  digitalWrite(LED_GND, LOW);
  pwmR.attachPin(LED_R, 5000, 8);
  pwmG.attachPin(LED_G, 5000, 8);
  pwmB.attachPin(LED_B, 5000, 8);
}

void rgb(uint8_t r, uint8_t g, uint8_t b) {
  pwmR.write(dimmed(r));
  pwmG.write(dimmed(g));
  pwmB.write(dimmed(b));
}
void rgbOff() { pwmR.write(0); pwmG.write(0); pwmB.write(0); }

void breathe(uint8_t r, uint8_t g, uint8_t b, int ms) {
  const int steps = 24;
  for (int i = 0; i < steps; i++) {
    float p = sinf(i / (float)steps * 3.1416f);
    rgb((uint8_t)(r * p), (uint8_t)(g * p), (uint8_t)(b * p));
    delay(ms / steps);
  }
  rgbOff();
}

void flash(uint8_t r, uint8_t g, uint8_t b, int times, int ms) {
  for (int i = 0; i < times; i++) { rgb(r, g, b); delay(ms); rgbOff(); delay(ms); }
}

// ================================================================
//  SERVO  (ESP32Servo)
// ================================================================

Servo servo;

void servoGoto(int deg) {
  deg = constrain(deg, 0, 180);
  servo.write(deg);
  currentAngle = deg;
}

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
  servo.attach(SERVO_PIN, 500, 2500);
  servo.write(cfgAngleRest);
  currentAngle = cfgAngleRest;
  delay(700);
  servo.detach();                       // quiet when parked
}

void servoRun(int target) {
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2500);
  servo.write(currentAngle);
  delay(80);
  servoSweep(target);
  servoSweep(cfgAngleRest);
  delay(150);
  servo.detach();
}

// ================================================================
//  MQTT
// ================================================================

void say(const String& event, const String& extra) {
  if (!mqttUp) return;
  String body = String("{\"node\":\"") + cfgClientId + "\",\"event\":\"" + event +
                "\",\"wake\":" + String(wakeCount) +
                ",\"state\":\"" + stateName() + "\"" +
                ",\"remain_min\":" + String((rtcRemain + 59) / 60) +
                ",\"angle\":" + String(currentAngle);
  if (extra.length()) body += "," + extra;
  body += "}";

  if (mqtt.beginMessage(cfgPubTopic, false, 1)) {     // QoS 1
    mqtt.print(body);
    mqtt.endMessage();
  }
  Serial.println("ack " + body);
}

void enterConfigMode();                                // forward

// Turn the servo on or off and remember which way it is
void servoOn()  { rgb(0, 255, 0); servoRun(cfgAngleOn);  flash(0, 255, 0, 2, 90); rgbOff(); }
void servoOff() { rgb(255, 0, 0); servoRun(cfgAngleOff); flash(255, 0, 0, 2, 90); rgbOff(); }

// "ON 37" -> verb "ON", minutes 37.  "ON37", "ON  37" and "ON=37" work too.
void parseCommand(const String& in, String& verb, long& minutes) {
  verb = "";
  minutes = 0;
  int i = 0;
  while (i < (int)in.length() && isAlpha(in.charAt(i))) verb += in.charAt(i++);
  while (i < (int)in.length() && !isDigit(in.charAt(i))) i++;
  if (i < (int)in.length()) minutes = in.substring(i).toInt();
}

void handleCommand(const String& raw) {
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();
  lastCommand = cmd;

  String verb;
  long minutes = 0;
  parseCommand(cmd, verb, minutes);

  if (verb.startsWith("CONFIG")) {
    enterConfigMode();
    return;
  }

  say("received", "\"msg\":\"" + cmd + "\"");

  if (verb == "ON") {
    if (minutes > 0) {
      // run for this long, then turn myself off
      rtcState  = ST_COUNTDOWN;
      rtcRemain = (int32_t)minutes * 60;
      Serial.printf("ON for %ld minutes\n", minutes);
      servoOn();
      say("on", "\"for_min\":" + String(minutes) + ",\"until_sec\":" + String(rtcRemain));
    } else {
      rtcState  = ST_NORMAL;      // plain ON, no timer
      rtcRemain = 0;
      Serial.println("ON (no timer)");
      servoOn();
      say("on", "\"for_min\":0");
    }
  } else if (verb == "OFF") {
    // a hand sent OFF cancels any countdown and goes back to the plain interval
    bool wasCounting = (rtcState == ST_COUNTDOWN);
    rtcState  = ST_NORMAL;
    rtcRemain = 0;
    Serial.println(wasCounting ? "OFF (countdown cancelled)" : "OFF");
    servoOff();
    say("off", String("\"by\":\"manual\",\"cancelled\":") + (wasCounting ? "true" : "false"));
  } else {
    Serial.println("message (not a command): " + cmd);
    say("ignored", "\"msg\":\"" + cmd + "\"");
    flash(120, 120, 120, 1, 120);
    rgbOff();
  }
}

void onMqttMessage(int size) {
  String topic = mqtt.messageTopic();
  String payload;
  payload.reserve(size + 1);
  while (mqtt.available()) payload += (char)mqtt.read();

  Serial.println("MQTT [" + topic + "] " + payload);
  gotAnything = true;
  flash(160, 160, 160, 1, 70);
  handleCommand(payload);
}

bool mqttConnect() {
  Serial.println("MQTT connecting to " + cfgMqttHost + ":" + String(cfgMqttPort));
  rgb(140, 90, 0);                                   // yellow

  net.setInsecure();
  mqtt.setId(cfgClientId.c_str());
  mqtt.setUsernamePassword(cfgMqttUser.c_str(), cfgMqttPass.c_str());

  // Normally we keep the session so the broker queues for us while we
  // sleep. Once after a topic change we connect clean, to drop the
  // subscriptions we no longer want.
  mqtt.setCleanSession(cfgResetSession);
  mqtt.setKeepAliveInterval(120000);
  mqtt.setConnectionTimeout(8000);
  mqtt.onMessage(onMqttMessage);

  if (!mqtt.connect(cfgMqttHost.c_str(), cfgMqttPort)) {
    Serial.printf("MQTT failed, error %d\n", mqtt.connectError());
    rgbOff();
    return false;
  }

  mqttUp = true;
  Serial.println("MQTT connected");

  if (cfgResetSession) {
    cfgResetSession = false;
    prefs.putBool("rsess", false);
    Serial.println("subscriptions reset after a topic change");
  }

  mqtt.subscribe(cfgSubTopic, 1);
  mqtt.subscribe(cfgSubTopic + "/#", 1);
  Serial.println("subscribed to " + cfgSubTopic);

  flash(0, 200, 0, 2, 70);
  return true;
}

// ================================================================
//  WIFI
// ================================================================

bool wifiConnect(unsigned long timeoutMs) {
  Serial.println("WiFi joining " + cfgWifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    breathe(0, 0, 200, 600);
  }
  if (WiFi.status() != WL_CONNECTED) { Serial.println("WiFi failed"); return false; }

  Serial.print("WiFi ok, IP ");
  Serial.println(WiFi.localIP());
  flash(0, 180, 180, 2, 120);
  return true;
}

// ================================================================
//  CONFIG WEB PAGE
// ================================================================

const char CONFIG_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Servo node setup</title><style>
:root{--bg:#eef2f5;--card:#fff;--fg:#16222e;--muted:#5f6f7e;--line:#d8e0e6;--band:#10202f;--acc:#0f766e;--accfg:#fff}
@media(prefers-color-scheme:dark){:root{--bg:#0b1218;--card:#111c25;--fg:#e5edf3;--muted:#8a9aa8;--line:#1f2d3a;--band:#0f1b27;--acc:#2dd4bf;--accfg:#04201c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 "Segoe UI",system-ui,-apple-system,Roboto,sans-serif}
.band{background:var(--band);color:#eaf1f6;padding:20px 18px}.wrap{max-width:520px;margin:0 auto}
.band h1{margin:0;font-size:20px}.band p{margin:4px 0 0;font-size:13px;opacity:.75}
main{padding:16px 18px 90px}
h2{font-size:16px;margin:20px 0 6px}
.grid{display:grid;grid-template-columns:auto 1fr;gap:4px 14px;font-size:14px;background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
.grid span:nth-child(odd){color:var(--muted)}
.grid span:nth-child(even){text-align:right;font-variant-numeric:tabular-nums;word-break:break-all}
label span{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}
input,select{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg);font:inherit}
.two{display:grid;grid-template-columns:1fr 1fr;gap:10px}
button{font:inherit;font-weight:600;padding:12px 16px;border:0;border-radius:8px;background:var(--acc);color:var(--accfg);cursor:pointer;width:100%;margin-top:14px}
button.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}
.btns{display:flex;gap:8px}.btns button{margin-top:10px}
#msg{margin-top:12px;font-size:14px;color:var(--acc);min-height:20px}
.hint{color:var(--muted);font-size:13px;margin:6px 0 0}
</style></head><body>
<div class="band"><div class="wrap"><h1>Servo node setup</h1><p id="sub">config mode</p></div></div>
<main class="wrap">
  <h2>Status</h2><div class="grid" id="st"></div>

  <h2>Test the servo</h2>
  <p class="hint">Move it now, without publishing anything.</p>
  <div class="btns">
    <button class="ghost" onclick="test('on')">Run ON</button>
    <button class="ghost" onclick="test('off')">Run OFF</button>
    <button class="ghost" onclick="test('rest')">Go to rest</button>
  </div>

  <h2>Wake schedule</h2>
  <label><span>Mode</span>
    <select id="mode" onchange="modeUI()">
      <option value="0">Auto - follow the ON timer, then wind down</option>
      <option value="1">Manual - awake 1 min, asleep 1 min</option>
    </select>
  </label>

  <p class="hint" id="manualNote">Manual keeps it awake for a minute and asleep for a minute, so commands land almost straight away. Nothing to set.</p>

  <div id="autoBox">
    <p class="hint">While an ON timer is running it hops in long steps, then short ones near the end. "ON 37" becomes seven 5s and two 1s.</p>
    <div class="two">
      <label><span>Long hop, min</span><input id="slong" type="number" min="1" max="240"></label>
      <label><span>Short hop, min</span><input id="sshort" type="number" min="1" max="240"></label>
    </div>
    <label><span>Use short hops below, min</span><input id="sbelow" type="number" min="1" max="240"></label>
    <p class="hint">After the timer turns it off by itself, the interval winds down through these tiers.</p>
    <div class="two">
      <label><span>Tier 1, min</span><input id="d1m" type="number" min="1" max="1440"></label>
      <label><span>for, hours</span><input id="d1h" type="number" min="0" max="240"></label>
    </div>
    <div class="two">
      <label><span>Tier 2, min</span><input id="d2m" type="number" min="1" max="1440"></label>
      <label><span>for, hours</span><input id="d2h" type="number" min="0" max="240"></label>
    </div>
    <label><span>Then for ever, min</span><input id="d3m" type="number" min="1" max="1440"></label>
    <label><span>Idle interval otherwise, min</span><input id="nmin" type="number" min="1" max="1440"></label>
  </div>

  <label><span>Listen window each wake, ms</span><input id="listen" type="number" min="300" max="30000"></label>

  <h2>Topics</h2>
  <label><span>Subscribe to</span><input id="sub_t" maxlength="80"></label>
  <label><span>Publish to</span><input id="pub_t" maxlength="80"></label>
  <label><span>Client ID (must be unique on the broker)</span><input id="cid" maxlength="60"></label>

  <h2>Servo</h2>
  <div class="two">
    <label><span>Rest angle</span><input id="arest" type="number" min="0" max="180"></label>
    <label><span>LED brightness</span><input id="led" type="number" min="1" max="255"></label>
  </div>
  <div class="two">
    <label><span>ON angle</span><input id="aon" type="number" min="0" max="180"></label>
    <label><span>OFF angle</span><input id="aoff" type="number" min="0" max="180"></label>
  </div>

  <h2>WiFi</h2>
  <label><span>Network</span><input id="wssid" maxlength="32"></label>
  <label><span>Password <small>(blank keeps the saved one)</small></span><input id="wpass" type="password" placeholder="unchanged" maxlength="63"></label>

  <h2>Broker</h2>
  <label><span>Host</span><input id="mhost" maxlength="80"></label>
  <div class="two">
    <label><span>Port</span><input id="mport" type="number" min="1" max="65535"></label>
    <label><span>Username</span><input id="muser" maxlength="40"></label>
  </div>
  <label><span>Password <small>(blank keeps the saved one)</small></span><input id="mpass" type="password" placeholder="unchanged" maxlength="60"></label>

  <button onclick="save()">Save and reboot</button>
  <div class="btns"><button class="ghost" onclick="reboot()">Reboot without saving</button></div>
  <p id="msg"></p>
</main>
<script>
const $=s=>document.getElementById(s);
const F=['mode','listen','nmin','slong','sshort','sbelow','d1m','d1h','d2m','d2h','d3m',
         'sub_t','pub_t','cid','arest','aon','aoff','led','wssid','mhost','mport','muser'];
window.esc = function(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
window.modeUI = function(){const a=$('mode').value==='0';$('autoBox').style.display=a?'':'none';$('manualNote').style.display=a?'none':''}
let filled=false;
 window.load = async function(){
  const s=await (await fetch('/api/cfg',{cache:'no-store'})).json();
  // Fill the form ONCE. Refilling on every poll would overwrite whatever
  // you are in the middle of changing, a second after you change it.
  if(!filled){
    F.forEach(k=>{if(s[k]!==undefined)$(k).value=s[k]});
    modeUI();
    filled=true;
  }
  $('sub').textContent='config mode  ·  '+s.ip;
  const rows={'IP':s.ip,'MAC':s.mac,'Signal':s.rssi+' dBm','Wakes':s.wake,
              'State':s.state,
              'Time left':s.state==='countdown'?(s.remain+' min'):'-',
              'Next wake':s.next_min+' min ('+s.sched+')',
              'Servo angle':s.angle+'°','Last command':s.last,
              'Broker':s.mqtt?'connected':'not connected','Uptime':s.up+' s'};
  $('st').innerHTML=Object.entries(rows).map(([k,v])=>'<span>'+k+'</span><span>'+esc(v)+'</span>').join('');
}
 window.post = async function(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
 window.test = async function(w){$('msg').textContent='moving...';await post('/api/test',{w:w});$('msg').textContent='done';load()}
 window.save = async function(){
  const d={};F.forEach(k=>d[k]=$(k).value);
  if($('wpass').value)d.wpass=$('wpass').value;
  if($('mpass').value)d.mpass=$('mpass').value;
  $('msg').textContent='saving...';
  await post('/api/save',d);
  filled=false;                    // re-read the saved values if we are still up
  $('msg').textContent='Saved. Rebooting into the normal sleep cycle.';
}
 window.reboot = async function(){await post('/api/reboot',{});$('msg').textContent='Rebooting.'}
load();setInterval(load,5000);
</script></body></html>
)HTML";

int nextSleepSeconds(String& reason);   // defined below

void sendCfg() {
  String o = "{";
  o += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  o += "\"mac\":\"" + WiFi.macAddress() + "\",";
  o += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  o += "\"wake\":" + String(wakeCount) + ",";
  o += "\"angle\":" + String(currentAngle) + ",";
  o += "\"last\":\"" + lastCommand + "\",";
  o += "\"mqtt\":" + String(mqttUp ? "true" : "false") + ",";
  o += "\"up\":" + String(millis() / 1000UL) + ",";
  o += "\"state\":\"" + String(stateName()) + "\",";
  o += "\"remain\":" + String((rtcRemain + 59) / 60) + ",";
  { String r; o += "\"next_min\":" + String(nextSleepSeconds(r) / 60) + ",";
              o += "\"sched\":\"" + r + "\","; }
  o += "\"mode\":" + String(cfgMode) + ",";
  o += "\"nmin\":" + String(cfgNormalMin) + ",";
  o += "\"slong\":" + String(cfgStepLong) + ",";
  o += "\"sshort\":" + String(cfgStepShort) + ",";
  o += "\"sbelow\":" + String(cfgShortBelow) + ",";
  o += "\"d1m\":" + String(cfgD1Min) + ",";
  o += "\"d1h\":" + String(cfgD1Hours) + ",";
  o += "\"d2m\":" + String(cfgD2Min) + ",";
  o += "\"d2h\":" + String(cfgD2Hours) + ",";
  o += "\"d3m\":" + String(cfgD3Min) + ",";
  o += "\"listen\":" + String(cfgListenMs) + ",";
  o += "\"sub_t\":\"" + cfgSubTopic + "\",";
  o += "\"pub_t\":\"" + cfgPubTopic + "\",";
  o += "\"cid\":\"" + cfgClientId + "\",";
  o += "\"arest\":" + String(cfgAngleRest) + ",";
  o += "\"aon\":" + String(cfgAngleOn) + ",";
  o += "\"aoff\":" + String(cfgAngleOff) + ",";
  o += "\"led\":" + String(cfgLedBright) + ",";
  o += "\"wssid\":\"" + cfgWifiSsid + "\",";
  o += "\"mhost\":\"" + cfgMqttHost + "\",";
  o += "\"mport\":" + String(cfgMqttPort) + ",";
  o += "\"muser\":\"" + cfgMqttUser + "\"}";
  web.send(200, "application/json", o);
}

int argInt(const char* k, int cur, int lo, int hi) {
  if (!web.hasArg(k)) return cur;
  return constrain((int)web.arg(k).toInt(), lo, hi);
}
String argStr(const char* k, const String& cur, int maxLen) {
  if (!web.hasArg(k)) return cur;
  String v = web.arg(k);
  v.trim();
  if (v.length() == 0) return cur;
  if ((int)v.length() > maxLen) v = v.substring(0, maxLen);
  return v;
}

void handleSave() {
  String oldSub = cfgSubTopic, oldCid = cfgClientId;

  cfgMode      = argInt("mode",   cfgMode, 0, 1);
  cfgNormalMin = argInt("nmin",   cfgNormalMin, 1, 1440);
  cfgStepLong  = argInt("slong",  cfgStepLong, 1, 240);
  cfgStepShort = argInt("sshort", cfgStepShort, 1, 240);
  cfgShortBelow= argInt("sbelow", cfgShortBelow, 1, 240);
  cfgD1Min     = argInt("d1m",    cfgD1Min, 1, 1440);
  cfgD1Hours   = argInt("d1h",    cfgD1Hours, 0, 240);
  cfgD2Min     = argInt("d2m",    cfgD2Min, 1, 1440);
  cfgD2Hours   = argInt("d2h",    cfgD2Hours, 0, 240);
  cfgD3Min     = argInt("d3m",    cfgD3Min, 1, 1440);
  cfgListenMs  = argInt("listen", cfgListenMs, 300, 30000);
  cfgSubTopic  = argStr("sub_t",  cfgSubTopic, 80);
  cfgPubTopic  = argStr("pub_t",  cfgPubTopic, 80);
  cfgClientId  = argStr("cid",    cfgClientId, 60);
  cfgAngleRest = argInt("arest",  cfgAngleRest, 0, 180);
  cfgAngleOn   = argInt("aon",    cfgAngleOn,   0, 180);
  cfgAngleOff  = argInt("aoff",   cfgAngleOff,  0, 180);
  cfgLedBright = argInt("led",    cfgLedBright, 1, 255);
  cfgWifiSsid  = argStr("wssid",  cfgWifiSsid, 32);
  cfgMqttHost  = argStr("mhost",  cfgMqttHost, 80);
  cfgMqttPort  = argInt("mport",  cfgMqttPort, 1, 65535);
  cfgMqttUser  = argStr("muser",  cfgMqttUser, 40);
  if (web.hasArg("wpass") && web.arg("wpass").length()) cfgWifiPass = web.arg("wpass");
  if (web.hasArg("mpass") && web.arg("mpass").length()) cfgMqttPass = web.arg("mpass");

  prefs.putString("wssid", cfgWifiSsid);
  prefs.putString("wpass", cfgWifiPass);
  prefs.putString("mhost", cfgMqttHost);
  prefs.putInt   ("mport", cfgMqttPort);
  prefs.putString("muser", cfgMqttUser);
  prefs.putString("mpass", cfgMqttPass);
  prefs.putString("sub",   cfgSubTopic);
  prefs.putString("pub",   cfgPubTopic);
  prefs.putString("cid",   cfgClientId);
  prefs.putInt   ("mode",  cfgMode);
  prefs.putInt   ("nmin",  cfgNormalMin);
  prefs.putInt   ("slong", cfgStepLong);
  prefs.putInt   ("sshort",cfgStepShort);
  prefs.putInt   ("sbelow",cfgShortBelow);
  prefs.putInt   ("d1m",   cfgD1Min);
  prefs.putInt   ("d1h",   cfgD1Hours);
  prefs.putInt   ("d2m",   cfgD2Min);
  prefs.putInt   ("d2h",   cfgD2Hours);
  prefs.putInt   ("d3m",   cfgD3Min);
  prefs.putInt   ("listen",cfgListenMs);
  prefs.putInt   ("arest", cfgAngleRest);
  prefs.putInt   ("aon",   cfgAngleOn);
  prefs.putInt   ("aoff",  cfgAngleOff);
  prefs.putInt   ("led",   cfgLedBright);

  // A changed topic or client id leaves stale subscriptions in the broker's
  // saved session, so ask for one clean connect next boot to clear them.
  if (cfgSubTopic != oldSub || cfgClientId != oldCid) prefs.putBool("rsess", true);

  web.send(200, "application/json", "{\"ok\":true}");
  Serial.println("settings saved, rebooting");
  delay(400);
  ESP.restart();
}

void handleTest() {
  String w = web.arg("w");
  if      (w == "on")   servoRun(cfgAngleOn);
  else if (w == "off")  servoRun(cfgAngleOff);
  else                  servoRun(cfgAngleRest);
  web.send(200, "application/json", "{\"ok\":true}");
}

void enterConfigMode() {
  configMode = true;
  Serial.println("=== CONFIG MODE ===");
  Serial.println("open http://" + WiFi.localIP().toString());

  say("config", "\"ip\":\"" + WiFi.localIP().toString() +
                "\",\"url\":\"http://" + WiFi.localIP().toString() + "/\"");

  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", CONFIG_PAGE); });
  web.on("/api/cfg",    HTTP_GET,  sendCfg);
  web.on("/api/save",   HTTP_POST, handleSave);
  web.on("/api/test",   HTTP_POST, handleTest);
  web.on("/api/reboot", HTTP_POST, []() {
    web.send(200, "application/json", "{\"ok\":true}");
    delay(300);
    ESP.restart();
  });
  web.onNotFound([]() { web.send(404, "text/plain", "not found"); });
  web.begin();
}

// ================================================================
//  THE NORMAL ROUND
// ================================================================

// How long to sleep next, in seconds, and why.
// Manual mode ignores all of this and uses the one fixed interval.
int nextSleepSeconds(String& reason) {
  if (cfgMode == 1) { reason = "manual"; return MANUAL_SLEEP_SEC; }

  if (rtcState == ST_COUNTDOWN && rtcRemain > 0) {
    // Compare in whole minutes, rounded up. The few seconds each wake
    // costs would otherwise drop "10 minutes left" to 4:55 and kick us
    // into short hops a whole step early.
    int32_t remainMin = (rtcRemain + 59) / 60;
    if (remainMin >= (int32_t)cfgShortBelow) {
      reason = "countdown-long";
      return cfgStepLong * 60;
    }
    reason = "countdown-short";
    return cfgStepShort * 60;
  }

  if (rtcState == ST_DECAY) {
    int32_t t1 = (int32_t)cfgD1Hours * 3600;
    int32_t t2 = t1 + (int32_t)cfgD2Hours * 3600;
    if (rtcSinceOff < t1) { reason = "decay-1"; return cfgD1Min * 60; }
    if (rtcSinceOff < t2) { reason = "decay-2"; return cfgD2Min * 60; }
    reason = "decay-3";
    return cfgD3Min * 60;
  }

  reason = "normal";
  return cfgNormalMin * 60;
}

void goToSleep(const char* why) {
  String reason;
  int sleepSec = nextSleepSeconds(reason);

  if (sleepSec <= 0) {                    // configured never to sleep
    Serial.printf("staying awake (%s)\n", why);
    return;
  }

  // Charge this round's awake time plus the sleep we are about to take
  // against whichever clock is running, so the totals track real time.
  int32_t spent = (int32_t)(millis() / 1000UL) + sleepSec;
  if      (rtcState == ST_COUNTDOWN) rtcRemain   -= spent;
  else if (rtcState == ST_DECAY)     rtcSinceOff += spent;

  Serial.printf("sleeping %d s (%s, %s), state %s, %d s left\n\n",
                sleepSec, why, reason.c_str(), stateName(), (int)rtcRemain);

  if (mqttUp) {
    say("sleep", String("\"sec\":") + String(sleepSec) +
                 ",\"why\":\"" + why + "\",\"sched\":\"" + reason + "\"");
    delay(150);
    mqtt.stop();
  }
  rgbOff();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void doRound() {
  gotAnything = false;

  if (!wifiConnect(20000)) { breathe(200, 0, 0, 700); goToSleep("no wifi");   return; }
  if (!mqttConnect())      { breathe(200, 0, 0, 700); goToSleep("no broker"); return; }

  say("wake", "");

  // The timer ran out while we were asleep: turn off by ourselves and
  // start the decaying idle schedule.
  if (rtcState == ST_COUNTDOWN && rtcRemain <= 0) {
    Serial.println("countdown finished, turning off");
    rtcState    = ST_DECAY;
    rtcRemain   = 0;
    rtcSinceOff = 0;
    servoOff();
    say("off", "\"by\":\"timer\"");
    lastCommand = "AUTO OFF";
  }

  // Manual mode simply stays up for a minute, handling whatever turns up
  // as it turns up, then sleeps for a minute.
  if (cfgMode == 1) {
    Serial.printf("manual mode: awake for %d s\n", MANUAL_AWAKE_SEC);
    unsigned long tm = millis();
    while (millis() - tm < (unsigned long)MANUAL_AWAKE_SEC * 1000UL) {
      mqtt.poll();
      if (configMode) return;             // a CONFIG message arrived, stay up
      if (!mqtt.connected()) break;       // link dropped, no point waiting it out
      delay(10);
    }
    goToSleep("manual");
    return;
  }

  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)cfgListenMs) {
    mqtt.poll();
    if (gotAnything) break;
    delay(10);
  }
  if (configMode) return;                 // a CONFIG message arrived, stay up

  if (!gotAnything) {
    Serial.println("nothing waiting");
    flash(0, 0, 180, 1, 80);
    goToSleep("idle");
    return;
  }

  unsigned long t1 = millis();
  while (millis() - t1 < WAIT_FOR_MORE_MS) {
    mqtt.poll();
    if (configMode) return;
    delay(10);
  }
  goToSleep("done");
}

// ================================================================
//  SETUP / LOOP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(300);
  wakeCount++;

  loadSettings();
  Serial.printf("\n=== NEXUS servo node, wake #%lu ===\n", (unsigned long)wakeCount);

  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);

  ledBegin();
  flash(60, 60, 60, 1, 90);
  servoBegin();

  doRound();                              // deep sleeps at the end, unless
}                                         // config mode or sleep is 0

void loop() {
  if (configMode) {
    web.handleClient();
    if (mqttUp && mqtt.connected()) mqtt.poll();

    static unsigned long last = 0;        // magenta breathing: page is up
    if (millis() - last > 2200) { last = millis(); breathe(180, 0, 180, 700); }
    delay(2);
    return;
  }

  // Only reached if a round returned without sleeping, which means the
  // link failed. Try the whole round again shortly.
  static unsigned long retry = 0;
  if (millis() - retry > 10000UL) { retry = millis(); mqttUp = false; doRound(); }
  delay(5);
}
