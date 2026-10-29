/*
  ESP32 Smart Water Pump & Soil Monitor
  --------------------------------------
  Network brain:
    1) Saved Wi-Fi found      -> Local Network mode (works with or without internet)
    2) No Wi-Fi / not saved   -> Hotspot "ESP32_WaterPump" (192.168.4.1)
    3) Wi-Fi lost later       -> Hotspot starts after 30 s, retries router every 60 s
  Pump logic runs independently of any network.

  Board: ESP32 Dev Module | Serial: 115200
  Wiring: Relay IN -> GPIO23, Soil AO -> GPIO34, LED = GPIO2 (built-in)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ---------- Pins & hardware options ----------
#define RELAY_PIN   23
#define SENSOR_PIN  34
#define LED_PIN     2
const bool RELAY_ACTIVE_LOW = true;   // most relay modules: LOW = ON. Set false if reversed.

// ---------- Network settings ----------
const char* AP_SSID = "ESP32_WaterPump";
const char* AP_PASS = "12345678";       // change this if neighbours are nearby
const char* HOSTNAME = "waterpump";     // http://waterpump.local
const unsigned long STA_BOOT_TIMEOUT = 15000;  // wait for router at boot
const unsigned long LOST_BEFORE_AP   = 30000;  // wifi lost this long -> start hotspot
const unsigned long AP_RETRY_STA     = 60000;  // while hotspot is on, retry router this often
const unsigned long INTERNET_CHECK   = 30000;

// ---------- Objects & settings ----------
WebServer server(80);
Preferences prefs;

String wifiSsid, wifiPass;
int  thresholdPct = 30;   // water when moisture below this %
int  runSec       = 10;   // pump run time
int  delaySec     = 5;    // countdown before pump starts
int  soakMin      = 10;   // minimum wait between automatic runs
bool autoMode     = true;
int  dryVal       = 3500; // raw reading in dry air
int  wetVal       = 1500; // raw reading in water

// ---------- Runtime state ----------
enum PumpState { IDLE, COUNTDOWN, RUNNING };
PumpState pumpState = IDLE;
unsigned long stateStart = 0;
unsigned long lastRunEnd = 0;
bool hasRun = false;

bool apActive = false;
bool internetOk = false;
bool mdnsOn = false;
unsigned long lastConnected = 0, lastRetry = 0, lastNetCheck = 0, lastNetInet = 0;
unsigned long lastAutoCheck = 0;
int lastRaw = 0;
int lastPct = 0;

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
<div>Internet: <b id="net">-</b></div></div>

<div class="c"><h3>Settings</h3>
Moisture threshold (%)<input id="thr" type="number">
Run duration (sec)<input id="run" type="number">
Countdown delay (sec)<input id="dly" type="number">
Soak wait between auto runs (min)<input id="soak" type="number">
<label><input id="auto" type="checkbox" style="width:auto"> Automatic irrigation</label>
<hr><b>Home Wi-Fi</b> <small>(leave blank to keep current)</small>
<input id="ssid" placeholder="Wi-Fi name (SSID)">
<input id="pass" type="password" placeholder="Wi-Fi password">
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
  if(!filled){thr.value=j.thr;run.value=j.run;dly.value=j.dly;soak.value=j.soak;auto.checked=j.auto;filled=true;}
 }catch(e){}
}
async function save(){
 const p=new URLSearchParams({thr:thr.value,run:run.value,dly:dly.value,soak:soak.value,auto:auto.checked?1:0,ssid:ssid.value,pass:pass.value});
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
    digitalWrite(LED_PIN, (now / 100) % 2);  // rapid pulse
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

// ---------- Network ----------
void startAP() {
  WiFi.mode(wifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
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

void setupNetwork() {
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    Serial.printf("Connecting to %s", wifiSsid.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < STA_BOOT_TIMEOUT) {
      digitalWrite(LED_PIN, (millis() / 250) % 2);
      delay(50);
    }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    lastConnected = millis();
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
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apActive = false;
      Serial.println("Router back. Hotspot OFF");
    }
    if (!mdnsOn) startMdns();
    lastConnected = now;
    if (now - lastNetInet > INTERNET_CHECK || lastNetInet == 0) {
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
  if (apActive)                       digitalWrite(LED_PIN, (now / 500) % 2);  // slow blink = hotspot
  else if (WiFi.status() == WL_CONNECTED) digitalWrite(LED_PIN, (now % 3000) < 60); // short blip = on router
  else                                digitalWrite(LED_PIN, LOW);
}

// ---------- Web handlers ----------
void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleStatus() {
  const char* st = pumpState == IDLE ? "OFF" : (pumpState == COUNTDOWN ? "Countdown" : "RUNNING");
  long rem = 0;
  unsigned long el = millis() - stateStart;
  if (pumpState == COUNTDOWN) rem = delaySec - el / 1000;
  if (pumpState == RUNNING)   rem = runSec - el / 1000;
  if (rem < 0) rem = 0;

  bool sta = WiFi.status() == WL_CONNECTED;
  String ip = sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String mode = sta ? "Home Wi-Fi (" + wifiSsid + ")" : "Hotspot";
  if (!sta) mode = "Hotspot";

  char buf[400];
  snprintf(buf, sizeof(buf),
    "{\"raw\":%d,\"pct\":%d,\"state\":\"%s\",\"rem\":%ld,\"mode\":\"%s\",\"ip\":\"%s\","
    "\"net\":%s,\"thr\":%d,\"run\":%d,\"dly\":%d,\"soak\":%d,\"auto\":%s}",
    lastRaw, lastPct, st, rem, mode.c_str(), ip.c_str(),
    internetOk ? "true" : "false", thresholdPct, runSec, delaySec, soakMin,
    autoMode ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleOn()  { pumpStartCountdown(); server.send(200, "text/plain", "OK"); }
void handleOff() { pumpStopNow();        server.send(200, "text/plain", "OK"); }

void handleCal(bool dry) {
  int raw = readRaw();
  if (dry) { dryVal = raw; prefs.putInt("dry", raw); }
  else     { wetVal = raw; prefs.putInt("wet", raw); }
  lastRaw = raw; lastPct = rawToPct(raw);
  server.send(200, "text/plain", "OK");
}

void handleSave() {
  if (server.hasArg("thr"))  { thresholdPct = constrain(server.arg("thr").toInt(), 1, 100);  prefs.putInt("thr", thresholdPct); }
  if (server.hasArg("run"))  { runSec   = constrain(server.arg("run").toInt(), 1, 3600);     prefs.putInt("run", runSec); }
  if (server.hasArg("dly"))  { delaySec = constrain(server.arg("dly").toInt(), 0, 600);      prefs.putInt("dly", delaySec); }
  if (server.hasArg("soak")) { soakMin  = constrain(server.arg("soak").toInt(), 0, 1440);    prefs.putInt("soak", soakMin); }
  if (server.hasArg("auto")) { autoMode = server.arg("auto") == "1";                         prefs.putBool("auto", autoMode); }

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
  server.send(200, "text/plain", "Settings saved.");
}

void handleForget() {
  prefs.remove("ssid");
  prefs.remove("pass");
  server.send(200, "text/plain", "OK");
  delay(800);
  ESP.restart();
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
  server.begin();

  lastRaw = readRaw();
  lastPct = rawToPct(lastRaw);
  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();
  updatePump();
  autoIrrigationCheck();
  handleNetwork();
  updateIdleLed();
}
