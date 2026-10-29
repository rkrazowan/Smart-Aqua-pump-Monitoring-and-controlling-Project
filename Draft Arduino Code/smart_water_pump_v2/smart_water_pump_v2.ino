/*
  ESP32 Smart Water Pump & Soil Monitor  -  VERSION 2
  ---------------------------------------------------
  Access paths:
    1) Home Wi-Fi  : http://waterpump.local  (or your chosen fixed IP)
    2) Hotspot     : join "ESP32_WaterPump", any address opens the dashboard
    3) Anywhere    : cloud MQTT broker (needs internet on the ESP32)

  Network brain:
    Saved Wi-Fi found    -> Local Network mode (works with or without internet)
    No Wi-Fi / not saved -> Hotspot (192.168.4.1)
    Wi-Fi lost later     -> Hotspot after 30 s, retries router every 60 s
  Pump logic runs independently of any network.

  Libraries to install: PubSubClient (Nick O'Leary). Everything else is built in.
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

// ---------- Pins & hardware options ----------
#define RELAY_PIN   23
#define SENSOR_PIN  34
#define LED_PIN     2
const bool RELAY_ACTIVE_LOW = true;   // most relay modules: LOW = ON. Set false if reversed.

// ---------- Network settings ----------
const char* AP_SSID  = "ESP32_WaterPump";
const char* AP_PASS  = "12345678";      // change this if neighbours are nearby
const char* HOSTNAME = "waterpump";     // http://waterpump.local
const unsigned long STA_BOOT_TIMEOUT = 15000;
const unsigned long LOST_BEFORE_AP   = 30000;
const unsigned long AP_RETRY_STA     = 60000;
const unsigned long INTERNET_CHECK   = 30000;
const unsigned long MQTT_RETRY       = 20000;
const unsigned long MQTT_PUBLISH     = 5000;

// ---------- Objects ----------
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
int  soakMin      = 10;
bool autoMode     = true;
int  dryVal       = 3500;
int  wetVal       = 1500;
int  fixedLast    = 0;          // 0 = automatic IP, else last number of fixed IP
String mqttHost, mqttUser, mqttPass;
int  mqttPort     = 8883;
String devName    = "pump1";
String tStatus, tCmd, tOnline;

// ---------- Runtime state ----------
enum PumpState { IDLE, COUNTDOWN, RUNNING };
PumpState pumpState = IDLE;
unsigned long stateStart = 0, lastRunEnd = 0;
bool hasRun = false;

bool apActive = false, internetOk = false, mdnsOn = false;
unsigned long lastConnected = 0, lastRetry = 0, lastNetCheck = 0, lastNetInet = 0;
unsigned long lastAutoCheck = 0, lastMqttTry = 0, lastMqttPub = 0;
int lastRaw = 0, lastPct = 0;

// ---------- Local web page ----------
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
</style></head><body>
<h1>&#128167; Smart Water Pump</h1>
<div class="c"><h3>Soil Moisture</h3>
<div class="big" id="pct">--%</div>
<div class="bar"><div id="b"></div></div>
<small>Raw: <span id="raw">-</span></small></div>

<div class="c"><h3>Pump</h3>
<div>Status: <b id="st">-</b> <span id="rem"></span></div>
<button onclick="cmd('/on')">Turn ON (with delay timer)</button>
<button class="r" onclick="cmd('/off')">Turn OFF Immediately</button></div>

<div class="c"><h3>Network</h3>
<div>Mode: <b id="mode">-</b></div>
<div>Address: <b id="ip">-</b></div>
<div>Name: <b>http://waterpump.local</b></div>
<div>Internet: <b id="net">-</b></div>
<div>Cloud (remote): <b id="mq">-</b></div></div>

<div class="c"><h3>Pump settings</h3>
Moisture threshold (%)<input id="thr" type="number">
Run duration (sec)<input id="run" type="number">
Countdown delay (sec)<input id="dly" type="number">
Soak wait between auto runs (min)<input id="soak" type="number">
<label><input id="auto" type="checkbox" style="width:auto"> Automatic irrigation</label></div>

<div class="c"><h3>Home Wi-Fi</h3>
<small>Leave blank to keep current.</small>
<input id="ssid" placeholder="Wi-Fi name (SSID)">
<input id="pass" type="password" placeholder="Wi-Fi password">
Fixed address last number (0 = automatic)
<input id="fixed" type="number" placeholder="e.g. 200">
<small>Takes effect after reboot, once the board has connected to your router once.</small></div>

<div class="c"><h3>Remote access (cloud)</h3>
<small>Works from any network. Needs a free MQTT broker (e.g. HiveMQ Cloud).</small>
MQTT broker host<input id="mhost" placeholder="xxxx.s1.eu.hivemq.cloud">
Port (TLS)<input id="mport" type="number">
MQTT username<input id="muser">
MQTT password<input id="mpass" type="password" placeholder="blank = keep current">
Device name<input id="dev">
<button onclick="save()">Save &amp; Apply</button></div>

<div class="c"><h3>Sensor calibration</h3>
<small>Sensor in dry air &rarr; tap DRY. Sensor in water &rarr; tap WET.</small>
<button class="g" onclick="cmd('/calDry')">Set current as DRY</button>
<button class="g" onclick="cmd('/calWet')">Set current as WET</button></div>

<div class="c"><button class="o" onclick="forget()">Forget Wi-Fi &amp; Reset AP</button></div>
<div id="msg" style="text-align:center"></div>

<script>
let filled=false;
async function cmd(u){await fetch(u);refresh();}
async function refresh(){
 try{
  const j=await (await fetch('/status')).json();
  pct.textContent=j.pct+'%';b.style.width=j.pct+'%';raw.textContent=j.raw;
  st.textContent=j.state;rem.textContent=j.rem>0?('('+j.rem+' s left)'):'';
  mode.textContent=j.mode;ip.textContent=j.ip;
  net.textContent=j.mode=='Hotspot'?'n/a':(j.net?'Yes':'No (local only)');
  mq.textContent=j.mq?'Connected':(j.mhost?'Not connected':'Not set up');
  if(!filled){thr.value=j.thr;run.value=j.run;dly.value=j.dly;soak.value=j.soak;auto.checked=j.auto;
   mhost.value=j.mhost;mport.value=j.mport;muser.value=j.muser;dev.value=j.dev;fixed.value=j.fixed;filled=true;}
 }catch(e){}
}
async function save(){
 const p=new URLSearchParams({thr:thr.value,run:run.value,dly:dly.value,soak:soak.value,auto:auto.checked?1:0,
  ssid:ssid.value,pass:pass.value,fixed:fixed.value||0,mhost:mhost.value,mport:mport.value,muser:muser.value,mpass:mpass.value,dev:dev.value});
 const t=await (await fetch('/save',{method:'POST',body:p})).text();
 msg.textContent=t;
}
async function forget(){
 if(confirm('Erase saved Wi-Fi and restart in hotspot mode?')){
  await fetch('/forget');msg.textContent='Restarting in hotspot mode. Join ESP32_WaterPump.';}
}
setInterval(refresh,2000);refresh();
</script></body></html>
)rawliteral";

// ---------- Helpers ----------
void relayWrite(bool on) {
  digitalWrite(RELAY_PIN, (on ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

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

String cleanName(String s) {
  s.trim();
  String o = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_') o += c;
  }
  if (o.length() == 0) o = "pump1";
  return o;
}

String jsonEsc(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  return s;
}

void applyTopics() {
  String base = "waterpump/" + devName;
  tStatus = base + "/status";
  tCmd    = base + "/cmd";
  tOnline = base + "/online";
}

void loadSettings() {
  prefs.begin("pump", false);
  wifiSsid     = prefs.getString("ssid", "");
  wifiPass     = prefs.getString("pass", "");
  thresholdPct = prefs.getInt("thr", 30);
  runSec       = prefs.getInt("run", 10);
  delaySec     = prefs.getInt("dly", 5);
  soakMin      = prefs.getInt("soak", 10);
  autoMode     = prefs.getBool("auto", true);
  dryVal       = prefs.getInt("dry", 3500);
  wetVal       = prefs.getInt("wet", 1500);
  fixedLast    = prefs.getInt("fixed", 0);
  mqttHost     = prefs.getString("mhost", "");
  mqttPort     = prefs.getInt("mport", 8883);
  mqttUser     = prefs.getString("muser", "");
  mqttPass     = prefs.getString("mpass", "");
  devName      = cleanName(prefs.getString("dev", "pump1"));
  applyTopics();
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
  bool soaked = !hasRun || (now - lastRunEnd >= (unsigned long)soakMin * 60000UL);
  if (lastPct < thresholdPct && soaked) pumpStartCountdown();
}

// ---------- Status JSON (used by web page and MQTT) ----------
String buildStatus() {
  const char* st = pumpState == IDLE ? "OFF" : (pumpState == COUNTDOWN ? "Countdown" : "RUNNING");
  long rem = 0;
  unsigned long el = millis() - stateStart;
  if (pumpState == COUNTDOWN) rem = (long)delaySec - (long)(el / 1000);
  if (pumpState == RUNNING)   rem = (long)runSec - (long)(el / 1000);
  if (rem < 0) rem = 0;

  bool sta = WiFi.status() == WL_CONNECTED;
  String ip   = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String mode = sta ? "Home Wi-Fi: " + wifiSsid : "Hotspot";

  String s = "{";
  s += "\"raw\":" + String(lastRaw);
  s += ",\"pct\":" + String(lastPct);
  s += ",\"state\":\"" + String(st) + "\"";
  s += ",\"rem\":" + String(rem);
  s += ",\"mode\":\"" + jsonEsc(mode) + "\"";
  s += ",\"ip\":\"" + ip + "\"";
  s += ",\"net\":" + String(internetOk ? "true" : "false");
  s += ",\"mq\":" + String(mqtt.connected() ? "true" : "false");
  s += ",\"thr\":" + String(thresholdPct);
  s += ",\"run\":" + String(runSec);
  s += ",\"dly\":" + String(delaySec);
  s += ",\"soak\":" + String(soakMin);
  s += ",\"auto\":" + String(autoMode ? "true" : "false");
  s += ",\"fixed\":" + String(fixedLast);
  s += ",\"mhost\":\"" + jsonEsc(mqttHost) + "\"";
  s += ",\"mport\":" + String(mqttPort);
  s += ",\"muser\":\"" + jsonEsc(mqttUser) + "\"";
  s += ",\"dev\":\"" + jsonEsc(devName) + "\"";
  s += "}";
  return s;
}

// ---------- Commands (shared by MQTT) ----------
void applyCommand(String msg) {
  msg.trim();
  msg.toLowerCase();
  Serial.println("Command: " + msg);
  if (msg == "on")            pumpStartCountdown();
  else if (msg == "off")      pumpStopNow();
  else if (msg == "auto_on")  { autoMode = true;  prefs.putBool("auto", true); }
  else if (msg == "auto_off") { autoMode = false; prefs.putBool("auto", false); }
  else if (msg.startsWith("thr="))  { thresholdPct = constrain(msg.substring(4).toInt(), 1, 100);  prefs.putInt("thr", thresholdPct); }
  else if (msg.startsWith("run="))  { runSec   = constrain(msg.substring(4).toInt(), 1, 3600);     prefs.putInt("run", runSec); }
  else if (msg.startsWith("dly="))  { delaySec = constrain(msg.substring(4).toInt(), 0, 600);      prefs.putInt("dly", delaySec); }
  else if (msg.startsWith("soak=")) { soakMin  = constrain(msg.substring(5).toInt(), 0, 1440);     prefs.putInt("soak", soakMin); }
}

// ---------- MQTT (remote access) ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg = "";
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  applyCommand(msg);
  lastMqttPub = 0;  // publish fresh status right away
}

void mqttLoop() {
  unsigned long now = millis();
  if (mqttHost.length() == 0 || WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (!mqtt.connected()) {
    if (lastMqttTry != 0 && now - lastMqttTry < MQTT_RETRY) return;
    lastMqttTry = now;
    secureClient.setInsecure();            // encrypted, but certificate not verified (hobby use)
    secureClient.setTimeout(5);
    secureClient.setHandshakeTimeout(5);
    mqtt.setServer(mqttHost.c_str(), mqttPort);
    mqtt.setBufferSize(768);
    mqtt.setCallback(mqttCallback);
    String cid = "esp32pump-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    Serial.println("MQTT connecting...");
    if (mqtt.connect(cid.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                     tOnline.c_str(), 1, true, "offline")) {
      mqtt.publish(tOnline.c_str(), "online", true);
      mqtt.subscribe(tCmd.c_str());
      lastMqttPub = 0;
      Serial.println("MQTT connected. Topic: " + tCmd);
    } else {
      Serial.printf("MQTT failed, rc=%d\n", mqtt.state());
    }
    return;
  }
  mqtt.loop();
  if (now - lastMqttPub >= MQTT_PUBLISH || lastMqttPub == 0) {
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

    if (fixedLast >= 2 && gwSaved && maskSaved) {
      IPAddress gw(gwSaved), mask(maskSaved), ip(gwSaved);
      ip[3] = fixedLast;
      WiFi.config(ip, gw, mask, gw);
      usedStatic = true;
      Serial.println("Using fixed IP: " + ip.toString());
    }

    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("Connecting to %s\n", wifiSsid.c_str());
    connected = waitSta(STA_BOOT_TIMEOUT);

    if (!connected && usedStatic) {            // fixed IP failed -> fall back to automatic
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
    lastConnected = now;
    if (lastNetInet == 0 || now - lastNetInet > INTERNET_CHECK) {
      lastNetInet = now;
      internetOk = checkInternet();
    }
  } else {
    internetOk = false;
    mdnsOn = false;
    if (!apActive) {
      bool noCreds = wifiSsid.length() == 0;
      if (noCreds || now - lastConnected > LOST_BEFORE_AP) startAP();
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
  if (apActive)                            digitalWrite(LED_PIN, (now / 500) % 2);   // slow blink = hotspot
  else if (WiFi.status() == WL_CONNECTED)  digitalWrite(LED_PIN, (now % 3000) < 60); // short blip = on router
  else                                     digitalWrite(LED_PIN, LOW);
}

// ---------- Web handlers ----------
void handleRoot()   { server.send_P(200, "text/html", PAGE); }
void handleStatus() { server.send(200, "application/json", buildStatus()); }
void handleOn()     { pumpStartCountdown(); server.send(200, "text/plain", "OK"); }
void handleOff()    { pumpStopNow();        server.send(200, "text/plain", "OK"); }

void handleCal(bool dry) {
  int raw = readRaw();
  if (dry) { dryVal = raw; prefs.putInt("dry", raw); }
  else     { wetVal = raw; prefs.putInt("wet", raw); }
  lastRaw = raw; lastPct = rawToPct(raw);
  server.send(200, "text/plain", "OK");
}

void handleSave() {
  bool needReboot = false;

  if (server.hasArg("thr"))  { thresholdPct = constrain(server.arg("thr").toInt(), 1, 100);  prefs.putInt("thr", thresholdPct); }
  if (server.hasArg("run"))  { runSec   = constrain(server.arg("run").toInt(), 1, 3600);     prefs.putInt("run", runSec); }
  if (server.hasArg("dly"))  { delaySec = constrain(server.arg("dly").toInt(), 0, 600);      prefs.putInt("dly", delaySec); }
  if (server.hasArg("soak")) { soakMin  = constrain(server.arg("soak").toInt(), 0, 1440);    prefs.putInt("soak", soakMin); }
  if (server.hasArg("auto")) { autoMode = server.arg("auto") == "1";                         prefs.putBool("auto", autoMode); }

  if (server.hasArg("fixed")) {
    int f = server.arg("fixed").toInt();
    if (f != 0) f = constrain(f, 2, 254);
    if (f != fixedLast) { fixedLast = f; prefs.putInt("fixed", f); needReboot = true; }
  }

  if (server.hasArg("mhost")) {
    mqttHost = server.arg("mhost"); mqttHost.trim();
    mqttPort = server.arg("mport").toInt();
    if (mqttPort <= 0) mqttPort = 8883;
    mqttUser = server.arg("muser"); mqttUser.trim();
    if (server.arg("mpass").length() > 0) { mqttPass = server.arg("mpass"); prefs.putString("mpass", mqttPass); }
    devName = cleanName(server.arg("dev"));
    prefs.putString("mhost", mqttHost);
    prefs.putInt("mport", mqttPort);
    prefs.putString("muser", mqttUser);
    prefs.putString("dev", devName);
    applyTopics();
    if (mqtt.connected()) mqtt.disconnect();
    lastMqttTry = 0;                       // reconnect with new details
  }

  String newSsid = server.arg("ssid");
  newSsid.trim();
  if (newSsid.length() > 0) {
    prefs.putString("ssid", newSsid);
    prefs.putString("pass", server.arg("pass"));
    needReboot = true;
  }

  if (needReboot) {
    server.send(200, "text/plain",
      "Saved. Rebooting... reconnect your phone to your home Wi-Fi, then open http://waterpump.local");
    delay(800);
    ESP.restart();
    return;
  }
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
  if (apActive) {      // captive portal: send any address to the dashboard
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
  handleNetwork();
  mqttLoop();
  updateIdleLed();
}
