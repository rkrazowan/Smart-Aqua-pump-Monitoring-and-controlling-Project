/*
  ESP32 Smart Water Pump & Soil Monitor  -  VERSION 3
  ---------------------------------------------------
  Access:
    Home Wi-Fi : http://waterpump.local   (or the fixed IP you set below)
    Hotspot    : join "ESP32_WaterPump"; any address opens the dashboard
    Anywhere   : remote_dashboard.html through a free MQTT cloud broker

  Features:
    - Auto irrigation by soil moisture
    - Daily scheduled start at a set time
    - Green indicator: the ESP32 sends a reply for every command it receives
    - Settings saved in flash (survive power cuts)

  Library to install: PubSubClient (Nick O'Leary). Everything else is built in.
  Board: ESP32 Dev Module | Serial: 115200
  Wiring: Relay IN -> GPIO23, Soil AO -> GPIO34, LED = GPIO2 (built-in)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <time.h>
#include <sys/time.h>

// =============== EDIT THESE (one time) ===============
const char* MQTT_HOST = "";        // e.g. "abc123.s1.eu.hivemq.cloud"  ("" = remote access off)
const int   MQTT_PORT = 8883;      // TLS port
const char* MQTT_USER = "";
const char* MQTT_PASS = "";
const char* DEVICE_ID = "pump1";   // must match the remote dashboard
const int   FIXED_IP_LAST = 0;     // 0 = automatic. e.g. 200 -> 192.168.x.200 on your router
const char* AP_SSID = "ESP32_WaterPump";
const char* AP_PASS = "12345678";  // change if neighbours are nearby
const int   SOAK_MIN = 10;         // minutes to wait after a run before auto-watering again
const bool  RELAY_ACTIVE_LOW = true;  // most relay modules: LOW = ON. Set false if reversed.
// =====================================================

#define RELAY_PIN   23
#define SENSOR_PIN  34
#define LED_PIN     2
const char* HOSTNAME = "waterpump";    // http://waterpump.local

const unsigned long STA_BOOT_TIMEOUT = 15000;
const unsigned long LOST_BEFORE_AP   = 30000;
const unsigned long AP_RETRY_STA     = 60000;
const unsigned long INTERNET_CHECK   = 30000;
const unsigned long MQTT_RETRY       = 20000;
const unsigned long MQTT_PUBLISH     = 5000;

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

// ---------- Saved settings ----------
String wifiSsid, wifiPass;
int  thresholdPct = 30;
int  runSec       = 10;
int  delaySec     = 5;
bool autoMode     = true;
int  dryVal       = 3500;
int  wetVal       = 1500;
bool schedOn      = false;
int  schedMin     = 360;      // minutes after midnight (360 = 06:00)
int  tzMin        = 0;        // minutes east of UTC, learned from your phone/browser
int  lastSchedKey = -1;
String tStatus, tCmd, tOnline, tAck;

// ---------- Runtime ----------
enum PumpState { IDLE, COUNTDOWN, RUNNING };
PumpState pumpState = IDLE;
unsigned long stateStart = 0, lastRunEnd = 0;
bool hasRun = false;
bool apActive = false, internetOk = false, mdnsOn = false, ntpStarted = false;
unsigned long lastConnected = 0, lastRetry = 0, lastNetCheck = 0, lastNetInet = 0;
unsigned long lastAutoCheck = 0, lastMqttTry = 0, lastMqttPub = 0, lastSchedCheck = 0;
int lastRaw = 0, lastPct = 0;

// ---------- Web page ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Water Pump</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:auto;padding:12px;background:#f2f5f3;color:#222}
.c{background:#fff;border-radius:10px;padding:14px;margin:10px 0;box-shadow:0 1px 3px #0002}
h1{font-size:1.3em;text-align:center} h3{margin:0 0 8px}
.big{font-size:2.4em;text-align:center;font-weight:bold}
button{width:100%;padding:12px;margin:5px 0;font-size:1em;border:0;border-radius:8px;color:#fff;background:#2e7d32}
button.r{background:#c62828} button.g{background:#546e7a} button.o{background:#ef6c00}
input{width:100%;padding:8px;margin:3px 0 8px;box-sizing:border-box}
.bar{height:14px;background:#ddd;border-radius:7px;overflow:hidden}
.bar div{height:100%;background:#1e88e5;width:0}
small{color:#666}
.led{display:inline-block;width:14px;height:14px;border-radius:50%;background:#bbb;vertical-align:middle;margin-right:6px}
.led.ok{background:#2e7d32;animation:bl .4s 5}
.led.wait{background:#f9a825}.led.bad{background:#c62828}
@keyframes bl{50%{opacity:.15}}
</style></head><body>
<h1>&#128167; Smart Water Pump</h1>

<div class="c"><h3>Soil Moisture</h3>
<div class="big" id="pct">--%</div>
<div class="bar"><div id="bar"></div></div>
<small>Raw: <span id="raw">-</span></small></div>

<div class="c"><h3>Pump</h3>
<div>Status: <b id="st">-</b> <span id="rem"></span></div>
<div><span class="led" id="led"></span><small id="ledtxt">Ready</small></div>
<button onclick="cmd('/on')">Turn ON (with delay timer)</button>
<button class="r" onclick="cmd('/off')">Turn OFF Immediately</button></div>

<div class="c"><h3>Daily Schedule</h3>
<label><input id="son" type="checkbox" style="width:auto"> Start pump every day at</label>
<input id="stime" type="time">
<small>ESP32 clock: <b id="clk">-</b></small></div>

<div class="c"><h3>Settings</h3>
Moisture threshold (%)<input id="thr" type="number">
Run duration (sec)<input id="run" type="number">
Countdown delay (sec)<input id="dly" type="number">
<label><input id="auto" type="checkbox" style="width:auto"> Automatic irrigation</label>
<hr><b>Home Wi-Fi</b> <small>(leave blank to keep current)</small>
<input id="ssid" placeholder="Wi-Fi name (SSID)">
<input id="pass" type="password" placeholder="Wi-Fi password">
<button onclick="save()">Save &amp; Apply</button></div>

<div class="c"><h3>Network</h3>
<div>Mode: <b id="mode">-</b></div>
<div>Address: <b id="ip">-</b> &nbsp;or&nbsp; <b>waterpump.local</b></div>
<div>Internet: <b id="net">-</b></div>
<div>Remote (cloud): <b id="mq">-</b></div></div>

<div class="c"><h3>Sensor calibration</h3>
<small>Sensor in dry air &rarr; DRY. Sensor in water &rarr; WET.</small>
<button class="g" onclick="cmd('/calDry')">Set current as DRY</button>
<button class="g" onclick="cmd('/calWet')">Set current as WET</button>
<button class="o" onclick="forget()">Forget Wi-Fi &amp; Reset AP</button></div>
<div id="msg" style="text-align:center"></div>

<script>
let filled=false,tm;
function blink(cls,txt){led.className='led '+cls;ledtxt.textContent=txt;clearTimeout(tm);
 if(cls!='wait')tm=setTimeout(()=>{led.className='led';ledtxt.textContent='Ready'},3500);}
async function call(u,opt){blink('wait','Sending...');
 try{const r=await fetch(u,opt);blink(r.ok?'ok':'bad',r.ok?'ESP32 responded \u2713':'Error');return r;}
 catch(e){blink('bad','No response');}}
async function cmd(u){await call(u);refresh();}
function sync(){fetch('/settime?epoch='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()));}
async function refresh(){
 try{
  const j=await (await fetch('/status')).json();
  pct.textContent=j.pct+'%';bar.style.width=j.pct+'%';raw.textContent=j.raw;
  st.textContent=j.state;rem.textContent=j.rem>0?('('+j.rem+' s left)'):'';
  mode.textContent=j.mode;ip.textContent=j.ip;
  net.textContent=j.mode=='Hotspot'?'n/a':(j.net?'Yes':'No (local only)');
  mq.textContent=['Not set up','Connected','Not connected'][j.mq];
  clk.textContent=j.clk||'not set';
  if(!filled){thr.value=j.thr;run.value=j.run;dly.value=j.dly;auto.checked=j.auto;
   son.checked=j.son;stime.value=j.stime;filled=true;sync();}
 }catch(e){}
}
async function save(){
 const p=new URLSearchParams({thr:thr.value,run:run.value,dly:dly.value,auto:auto.checked?1:0,
  son:son.checked?1:0,stime:stime.value||'06:00',ssid:ssid.value,pass:pass.value});
 const r=await call('/save',{method:'POST',body:p});
 if(r)msg.textContent=await r.text();
}
async function forget(){
 if(confirm('Erase saved Wi-Fi and restart in hotspot mode?')){
  await fetch('/forget');msg.textContent='Restarting in hotspot mode. Join ESP32_WaterPump.';}
}
setInterval(refresh,2000);refresh();
</script></body></html>
)rawliteral";

// ---------- Helpers ----------
void relayWrite(bool on) { digitalWrite(RELAY_PIN, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW); }

int readRaw() {
  long sum = 0;
  for (int i = 0; i < 10; i++) { sum += analogRead(SENSOR_PIN); delay(5); }
  return sum / 10;
}

int rawToPct(int raw) {
  if (dryVal == wetVal) return 0;
  long p = (long)(dryVal - raw) * 100 / (dryVal - wetVal);
  return constrain((int)p, 0, 100);
}

void flashLed(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}

void ackBlink() { digitalWrite(LED_PIN, HIGH); delay(80); digitalWrite(LED_PIN, LOW); }

String jsonEsc(String s) { s.replace("\\", "\\\\"); s.replace("\"", "\\\""); return s; }

String safeId(String s) {
  String o = "";
  for (unsigned int i = 0; i < s.length() && i < 16; i++)
    if (isalnum(s[i])) o += s[i];
  return o;
}

String hm(int m) { char b[8]; snprintf(b, sizeof(b), "%02d:%02d", m / 60, m % 60); return String(b); }

bool parseHM(String s, int &mins) {
  int c = s.indexOf(':');
  if (c < 1) return false;
  int h = s.substring(0, c).toInt(), m = s.substring(c + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  mins = h * 60 + m;
  return true;
}

bool clockSet() { return time(nullptr) > 1700000000; }

String clockStr() {
  if (!clockSet()) return "";
  time_t l = time(nullptr) + (time_t)tzMin * 60;
  struct tm t; gmtime_r(&l, &t);
  return hm(t.tm_hour * 60 + t.tm_min);
}

void setClock(long epoch, int tz) {
  tz = constrain(tz, -720, 840);
  if (tz != tzMin) { tzMin = tz; prefs.putInt("tz", tzMin); }
  if (!clockSet() && epoch > 1700000000) {      // only if NTP has not set it already
    struct timeval tv = { (time_t)epoch, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("Clock set from browser");
  }
}

void loadSettings() {
  prefs.begin("pump", false);
  wifiSsid     = prefs.getString("ssid", "");
  wifiPass     = prefs.getString("pass", "");
  thresholdPct = prefs.getInt("thr", 30);
  runSec       = prefs.getInt("run", 10);
  delaySec     = prefs.getInt("dly", 5);
  autoMode     = prefs.getBool("auto", true);
  dryVal       = prefs.getInt("dry", 3500);
  wetVal       = prefs.getInt("wet", 1500);
  schedOn      = prefs.getBool("son", false);
  schedMin     = prefs.getInt("smin", 360);
  tzMin        = prefs.getInt("tz", 0);
  lastSchedKey = prefs.getInt("skey", -1);
  String base = String("waterpump/") + DEVICE_ID;
  tStatus = base + "/status"; tCmd = base + "/cmd";
  tOnline = base + "/online"; tAck = base + "/ack";
}

// ---------- Pump control ----------
void pumpStopNow() {
  relayWrite(false);
  if (pumpState == RUNNING) { lastRunEnd = millis(); hasRun = true; }
  pumpState = IDLE;
  digitalWrite(LED_PIN, LOW);
  Serial.println("Pump OFF");
}

void pumpStartCountdown() {
  if (pumpState != IDLE) return;
  pumpState = COUNTDOWN;
  stateStart = millis();
  Serial.println("Countdown started");
}

void pumpRun() {
  flashLed(2);
  relayWrite(true);
  pumpState = RUNNING;
  stateStart = millis();
  Serial.println("Pump ON");
}

void updatePump() {
  unsigned long now = millis();
  if (pumpState == COUNTDOWN) {
    digitalWrite(LED_PIN, (now / 100) % 2);
    if (now - stateStart >= (unsigned long)delaySec * 1000UL) pumpRun();
  } else if (pumpState == RUNNING) {
    digitalWrite(LED_PIN, HIGH);
    if (now - stateStart >= (unsigned long)runSec * 1000UL) pumpStopNow();
  }
}

void autoIrrigationCheck() {
  unsigned long now = millis();
  if (now - lastAutoCheck < 10000) return;
  lastAutoCheck = now;
  lastRaw = readRaw();
  lastPct = rawToPct(lastRaw);
  Serial.printf("Moisture: %d%% (raw %d)\n", lastPct, lastRaw);
  if (!autoMode || pumpState != IDLE) return;
  if (lastRaw < 50) { Serial.println("Sensor fault? skipping"); return; }
  bool soaked = !hasRun || (now - lastRunEnd >= (unsigned long)SOAK_MIN * 60000UL);
  if (lastPct < thresholdPct && soaked) pumpStartCountdown();
}

// Daily schedule: starts the normal countdown + run at the chosen time
void scheduleCheck() {
  unsigned long now = millis();
  if (now - lastSchedCheck < 1000) return;
  lastSchedCheck = now;
  if (!schedOn || !clockSet() || pumpState != IDLE) return;
  time_t l = time(nullptr) + (time_t)tzMin * 60;
  struct tm t; gmtime_r(&l, &t);
  int nowMin = t.tm_hour * 60 + t.tm_min;
  int key = (t.tm_year * 366 + t.tm_yday) * 1440 + schedMin;
  if (nowMin == schedMin && key != lastSchedKey) {
    lastSchedKey = key;
    prefs.putInt("skey", key);
    Serial.println("Scheduled start");
    pumpStartCountdown();
  }
}

// ---------- Status JSON ----------
String buildStatus() {
  const char* st = pumpState == IDLE ? "OFF" : (pumpState == COUNTDOWN ? "Countdown" : "RUNNING");
  long rem = 0;
  long el = (long)((millis() - stateStart) / 1000);
  if (pumpState == COUNTDOWN) rem = delaySec - el;
  if (pumpState == RUNNING)   rem = runSec - el;
  if (rem < 0) rem = 0;

  bool sta = WiFi.status() == WL_CONNECTED;
  String ip   = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String mode = sta ? "Home Wi-Fi: " + wifiSsid : "Hotspot";
  int mq = strlen(MQTT_HOST) == 0 ? 0 : (mqtt.connected() ? 1 : 2);

  String s = "{";
  s += "\"raw\":" + String(lastRaw);
  s += ",\"pct\":" + String(lastPct);
  s += ",\"state\":\"" + String(st) + "\"";
  s += ",\"rem\":" + String(rem);
  s += ",\"mode\":\"" + jsonEsc(mode) + "\"";
  s += ",\"ip\":\"" + ip + "\"";
  s += ",\"net\":" + String(internetOk ? "true" : "false");
  s += ",\"mq\":" + String(mq);
  s += ",\"thr\":" + String(thresholdPct);
  s += ",\"run\":" + String(runSec);
  s += ",\"dly\":" + String(delaySec);
  s += ",\"auto\":" + String(autoMode ? "true" : "false");
  s += ",\"son\":" + String(schedOn ? "true" : "false");
  s += ",\"stime\":\"" + hm(schedMin) + "\"";
  s += ",\"clk\":\"" + clockStr() + "\"";
  s += "}";
  return s;
}

// ---------- Remote commands ----------
bool applyCommand(String msg) {
  msg.trim();
  msg.toLowerCase();
  Serial.println("Command: " + msg);
  if (msg == "on")            { pumpStartCountdown(); return true; }
  if (msg == "off")           { pumpStopNow(); return true; }
  if (msg == "auto_on")       { autoMode = true;  prefs.putBool("auto", true);  return true; }
  if (msg == "auto_off")      { autoMode = false; prefs.putBool("auto", false); return true; }
  if (msg == "sched_on")      { schedOn = true;   prefs.putBool("son", true);   return true; }
  if (msg == "sched_off")     { schedOn = false;  prefs.putBool("son", false);  return true; }
  if (msg.startsWith("thr=")) { thresholdPct = constrain(msg.substring(4).toInt(), 1, 100); prefs.putInt("thr", thresholdPct); return true; }
  if (msg.startsWith("run=")) { runSec   = constrain(msg.substring(4).toInt(), 1, 3600);    prefs.putInt("run", runSec);        return true; }
  if (msg.startsWith("dly=")) { delaySec = constrain(msg.substring(4).toInt(), 0, 600);     prefs.putInt("dly", delaySec);      return true; }
  if (msg.startsWith("sched=")) {
    int m;
    if (parseHM(msg.substring(6), m)) { schedMin = m; prefs.putInt("smin", m); return true; }
    return false;
  }
  if (msg.startsWith("time=")) {
    String v = msg.substring(5);
    int c = v.indexOf(',');
    if (c > 0) { setClock(v.substring(0, c).toInt(), v.substring(c + 1).toInt()); return true; }
  }
  return false;
}

// ---------- MQTT ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  int h = msg.indexOf('#');
  String cmd = h >= 0 ? msg.substring(0, h) : msg;
  String id  = h >= 0 ? safeId(msg.substring(h + 1)) : "";

  bool ok = applyCommand(cmd);
  ackBlink();

  // Response back to the server -> the green indicator blinks on the dashboard
  String ack = "{\"id\":\"" + id + "\",\"cmd\":\"" + jsonEsc(cmd) + "\",\"ok\":" +
               String(ok ? "true" : "false") + ",\"state\":\"" +
               String(pumpState == IDLE ? "OFF" : (pumpState == COUNTDOWN ? "Countdown" : "RUNNING")) + "\"}";
  mqtt.publish(tAck.c_str(), ack.c_str());
  lastMqttPub = 0;                           // follow with a fresh status
}

void mqttLoop() {
  unsigned long now = millis();
  if (strlen(MQTT_HOST) == 0 || WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (!mqtt.connected()) {
    if (lastMqttTry != 0 && now - lastMqttTry < MQTT_RETRY) return;
    lastMqttTry = now;
    secureClient.setInsecure();              // encrypted; certificate not verified (hobby use)
    secureClient.setTimeout(5);
    secureClient.setHandshakeTimeout(5);
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(768);
    mqtt.setCallback(mqttCallback);
    String cid = "esp32pump-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.println("MQTT connecting...");
    if (mqtt.connect(cid.c_str(), MQTT_USER, MQTT_PASS, tOnline.c_str(), 1, true, "offline")) {
      mqtt.publish(tOnline.c_str(), "online", true);
      mqtt.subscribe(tCmd.c_str());
      lastMqttPub = 0;
      Serial.println("MQTT connected. Command topic: " + tCmd);
    } else {
      Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
  if (lastMqttPub == 0 || now - lastMqttPub >= MQTT_PUBLISH) {
    lastMqttPub = now;
    mqtt.publish(tStatus.c_str(), buildStatus().c_str(), true);
  }
}

// ---------- Network ----------
void startAP() {
  WiFi.mode(wifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());   // any name opens the dashboard
  apActive = true;
  lastRetry = millis();
  Serial.printf("Hotspot ON: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void startMdns() {
  if (mdnsOn) MDNS.end();
  mdnsOn = MDNS.begin(HOSTNAME);
  if (mdnsOn) MDNS.addService("http", "tcp", 80);
}

bool checkInternet() {
  WiFiClient c;
  bool ok = c.connect(IPAddress(8, 8, 8, 8), 53, 1500);
  c.stop();
  return ok;
}

bool waitSta(unsigned long ms) {
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < ms) {
    digitalWrite(LED_PIN, (millis() / 250) % 2);
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

void setupNetwork() {
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  bool connected = false;

  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    uint32_t gwSaved = prefs.getUInt("gw", 0), maskSaved = prefs.getUInt("mask", 0);
    bool usedStatic = false;

    if (FIXED_IP_LAST >= 2 && FIXED_IP_LAST <= 254 && gwSaved && maskSaved) {
      IPAddress gw(gwSaved), mask(maskSaved), ip(gwSaved);
      ip[3] = FIXED_IP_LAST;
      WiFi.config(ip, gw, mask, gw);
      usedStatic = true;
      Serial.println("Using fixed IP: " + ip.toString());
    }

    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("Connecting to %s\n", wifiSsid.c_str());
    connected = waitSta(STA_BOOT_TIMEOUT);

    if (!connected && usedStatic) {            // fixed IP failed -> automatic IP
      Serial.println("Fixed IP failed, using automatic IP");
      WiFi.disconnect();
      delay(200);
      WiFi.config(IPAddress((uint32_t)0), IPAddress((uint32_t)0), IPAddress((uint32_t)0));
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
      connected = waitSta(10000);
    }
  }

  if (connected) {
    lastConnected = millis();
    uint32_t g = (uint32_t)WiFi.gatewayIP(), m = (uint32_t)WiFi.subnetMask();
    if (g != prefs.getUInt("gw", 0) || m != prefs.getUInt("mask", 0)) {
      prefs.putUInt("gw", g);
      prefs.putUInt("mask", m);
    }
    startMdns();
    Serial.printf("Connected. IP: %s  (http://%s.local)\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
  } else {
    startAP();
  }
  digitalWrite(LED_PIN, LOW);
}

void handleNetwork() {
  unsigned long now = millis();
  if (now - lastNetCheck < 5000) return;
  lastNetCheck = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (apActive) {                       // router is back -> drop the hotspot
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apActive = false;
      Serial.println("Router back. Hotspot OFF");
    }
    if (!mdnsOn) startMdns();
    if (!ntpStarted) { configTime(0, 0, "pool.ntp.org", "time.google.com"); ntpStarted = true; }
    lastConnected = now;
    if (lastNetInet == 0 || now - lastNetInet > INTERNET_CHECK) {
      lastNetInet = now;
      internetOk = checkInternet();
    }
  } else {
    internetOk = false;
    mdnsOn = false;
    if (!apActive) {
      if (wifiSsid.length() == 0 || now - lastConnected > LOST_BEFORE_AP) startAP();
    } else if (wifiSsid.length() && now - lastRetry > AP_RETRY_STA) {
      lastRetry = now;
      Serial.println("Retrying router...");
      WiFi.disconnect();
      WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    }
  }
}

void updateIdleLed() {
  if (pumpState != IDLE) return;
  unsigned long now = millis();
  if (apActive)                            digitalWrite(LED_PIN, (now / 500) % 2);    // slow blink = hotspot
  else if (WiFi.status() == WL_CONNECTED)  digitalWrite(LED_PIN, (now % 3000) < 60);  // short blip = on router
  else                                     digitalWrite(LED_PIN, LOW);
}

// ---------- Web handlers ----------
void handleRoot()   { server.send_P(200, "text/html", PAGE); }
void handleStatus() { server.send(200, "application/json", buildStatus()); }
void handleOn()     { pumpStartCountdown(); ackBlink(); server.send(200, "text/plain", "OK"); }
void handleOff()    { pumpStopNow();        ackBlink(); server.send(200, "text/plain", "OK"); }

void handleCal(bool dry) {
  int raw = readRaw();
  if (dry) { dryVal = raw; prefs.putInt("dry", raw); }
  else     { wetVal = raw; prefs.putInt("wet", raw); }
  lastRaw = raw; lastPct = rawToPct(raw);
  server.send(200, "text/plain", "OK");
}

void handleSetTime() {
  setClock(server.arg("epoch").toInt(), server.arg("tz").toInt());
  server.send(200, "text/plain", "OK");
}

void handleSave() {
  if (server.hasArg("thr"))  { thresholdPct = constrain(server.arg("thr").toInt(), 1, 100);  prefs.putInt("thr", thresholdPct); }
  if (server.hasArg("run"))  { runSec   = constrain(server.arg("run").toInt(), 1, 3600);     prefs.putInt("run", runSec); }
  if (server.hasArg("dly"))  { delaySec = constrain(server.arg("dly").toInt(), 0, 600);      prefs.putInt("dly", delaySec); }
  if (server.hasArg("auto")) { autoMode = server.arg("auto") == "1";                         prefs.putBool("auto", autoMode); }
  if (server.hasArg("son"))  { schedOn  = server.arg("son") == "1";                          prefs.putBool("son", schedOn); }
  if (server.hasArg("stime")) {
    int m;
    if (parseHM(server.arg("stime"), m)) { schedMin = m; prefs.putInt("smin", m); }
  }

  String newSsid = server.arg("ssid");
  newSsid.trim();
  if (newSsid.length() > 0) {
    prefs.putString("ssid", newSsid);
    prefs.putString("pass", server.arg("pass"));
    server.send(200, "text/plain",
      "Saved. Rebooting... reconnect your phone to your home Wi-Fi, then open http://waterpump.local");
    delay(800);
    ESP.restart();
    return;
  }
  ackBlink();
  server.send(200, "text/plain", "Settings saved.");
}

void handleForget() {
  prefs.remove("ssid");
  prefs.remove("pass");
  server.send(200, "text/plain", "OK");
  delay(800);
  ESP.restart();
}

void handleNotFound() {
  if (apActive) {      // captive portal: any address opens the dashboard
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "Not found");
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  relayWrite(false);               // pump OFF at boot
  pinMode(LED_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);

  loadSettings();
  setupNetwork();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/on", handleOn);
  server.on("/off", handleOff);
  server.on("/calDry", []() { handleCal(true); });
  server.on("/calWet", []() { handleCal(false); });
  server.on("/settime", handleSetTime);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/forget", handleForget);
  server.onNotFound(handleNotFound);
  server.begin();

  lastRaw = readRaw();
  lastPct = rawToPct(lastRaw);
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  if (apActive) dnsServer.processNextRequest();
  updatePump();
  autoIrrigationCheck();
  scheduleCheck();
  handleNetwork();
  mqttLoop();
  updateIdleLed();
}
