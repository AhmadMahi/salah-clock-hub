/*
  ================================================================
   NEXUS SERVO NODE  -  ESP32-C3 + servo + RGB LED
  ================================================================
   Normally it sleeps. Each wake it joins WiFi, connects to the
   broker, announces itself, collects whatever is waiting, acts on
   it, and sleeps again.

     "ON"      servo sweeps rest -> 180 -> rest and stops
     "OFF"     servo sweeps rest ->   0 -> rest and stops
     "CONFIG"  stays awake and serves a settings page instead

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

#define DEF_SLEEP_SEC   60        // 0 keeps it awake all the time
#define DEF_LISTEN_MS   1500      // how long to wait for a queued message
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
int    cfgSleepSec, cfgListenMs;
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
  cfgSleepSec = prefs.getInt   ("sleep", DEF_SLEEP_SEC);
  cfgListenMs = prefs.getInt   ("listen",DEF_LISTEN_MS);
  cfgAngleRest= prefs.getInt   ("arest", DEF_ANGLE_REST);
  cfgAngleOn  = prefs.getInt   ("aon",   DEF_ANGLE_ON);
  cfgAngleOff = prefs.getInt   ("aoff",  DEF_ANGLE_OFF);
  cfgLedBright= prefs.getInt   ("led",   DEF_LED_BRIGHT);
  cfgResetSession = prefs.getBool("rsess", false);

  cfgMqttPort  = constrain(cfgMqttPort, 1, 65535);
  cfgSleepSec  = constrain(cfgSleepSec, 0, 86400);
  cfgListenMs  = constrain(cfgListenMs, 300, 30000);
  cfgAngleRest = constrain(cfgAngleRest, 0, 180);
  cfgAngleOn   = constrain(cfgAngleOn,   0, 180);
  cfgAngleOff  = constrain(cfgAngleOff,  0, 180);
  cfgLedBright = constrain(cfgLedBright, 1, 255);
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

RTC_DATA_ATTR uint32_t wakeCount = 0;      // survives deep sleep

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

void handleCommand(const String& raw) {
  String cmd = raw;
  cmd.trim();
  cmd.toUpperCase();
  lastCommand = cmd;

  if (cmd.startsWith("CONFIG")) {
    enterConfigMode();
    return;
  }

  say("received", "\"msg\":\"" + cmd + "\"");

  if (cmd == "ON") {
    Serial.println("command ON");
    rgb(0, 255, 0);
    servoRun(cfgAngleOn);
    say("done", "\"cmd\":\"ON\"");
    flash(0, 255, 0, 2, 90);
  } else if (cmd == "OFF") {
    Serial.println("command OFF");
    rgb(255, 0, 0);
    servoRun(cfgAngleOff);
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
input{width:100%;padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg);font:inherit}
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

  <h2>Timing</h2>
  <label><span>Sleep between wakes, seconds (0 = never sleep)</span><input id="sleep" type="number" min="0" max="86400"></label>
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
const F=['sleep','listen','sub_t','pub_t','cid','arest','aon','aoff','led','wssid','mhost','mport','muser'];
function esc(s){return String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]))}
async function load(){
  const s=await (await fetch('/api/cfg',{cache:'no-store'})).json();
  F.forEach(k=>{if(s[k]!==undefined)$(k).value=s[k]});
  $('sub').textContent='config mode  ·  '+s.ip;
  const rows={'IP':s.ip,'MAC':s.mac,'Signal':s.rssi+' dBm','Wakes':s.wake,
              'Servo angle':s.angle+'°','Last command':s.last,
              'Broker':s.mqtt?'connected':'not connected','Uptime':s.up+' s'};
  $('st').innerHTML=Object.entries(rows).map(([k,v])=>'<span>'+k+'</span><span>'+esc(v)+'</span>').join('');
}
async function post(u,d){return fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(d||{})})}
async function test(w){$('msg').textContent='moving...';await post('/api/test',{w:w});$('msg').textContent='done';load()}
async function save(){
  const d={};F.forEach(k=>d[k]=$(k).value);
  if($('wpass').value)d.wpass=$('wpass').value;
  if($('mpass').value)d.mpass=$('mpass').value;
  $('msg').textContent='saving...';
  await post('/api/save',d);
  $('msg').textContent='Saved. Rebooting into the normal sleep cycle.';
}
async function reboot(){await post('/api/reboot',{});$('msg').textContent='Rebooting.'}
load();setInterval(load,5000);
</script></body></html>
)HTML";

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
  o += "\"sleep\":" + String(cfgSleepSec) + ",";
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

  cfgSleepSec  = argInt("sleep",  cfgSleepSec, 0, 86400);
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
  prefs.putInt   ("sleep", cfgSleepSec);
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

void goToSleep(const char* why) {
  if (cfgSleepSec == 0) {                 // configured to stay awake
    Serial.printf("staying awake (%s)\n", why);
    return;
  }
  Serial.printf("sleeping %d s (%s)\n\n", cfgSleepSec, why);

  if (mqttUp) {
    say("sleep", String("\"sec\":") + String(cfgSleepSec) + ",\"why\":\"" + why + "\"");
    delay(150);
    mqtt.stop();
  }
  rgbOff();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)cfgSleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void doRound() {
  gotAnything = false;

  if (!wifiConnect(20000)) { breathe(200, 0, 0, 700); goToSleep("no wifi");   return; }
  if (!mqttConnect())      { breathe(200, 0, 0, 700); goToSleep("no broker"); return; }

  say("wake", "");

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

  // only reached when cfgSleepSec is 0, ie never sleep
  if (mqttUp && mqtt.connected()) mqtt.poll();
  else {
    static unsigned long retry = 0;
    if (millis() - retry > 10000UL) { retry = millis(); mqttUp = false; doRound(); }
  }
  delay(5);
}
