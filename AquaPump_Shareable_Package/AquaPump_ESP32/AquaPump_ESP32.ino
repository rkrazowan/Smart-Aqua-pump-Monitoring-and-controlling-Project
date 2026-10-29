/*
  ESP32 Smart Water Pump & Soil Monitor  -  VERSION 6
  ---------------------------------------------------
  NEW in V6:
    - Phone + PC friendly dashboard with Light / Dark / Auto theme.
    - Weather (temperature, rain chance, 5-day forecast) for any country and
      district, shown in the dashboard (needs internet on your phone/PC only).
    - Sensor calibration also works from the remote dashboard.

  From V5:
    - Modern dashboard runs on the ESP32 itself: http://waterpump.local
      The pump controls work with NO internet (also in hotspot mode).

  From V4:
    - POWER-CUT RESUME: if power fails while the pump is in countdown or
      running, the ESP32 continues with the REMAINING time after power returns.
    - Daily schedule has its OWN run duration and countdown delay.

  Access:
    Home Wi-Fi : http://waterpump.local   (or the fixed IP you set below)
    Hotspot    : join "ESP32_WaterPump"; any address opens the dashboard
    Anywhere   : remote_dashboard.html through a free MQTT cloud broker

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
const char* MQTT_HOST = "YOUR-BROKER-ADDRESS.emqxsl.com";        // ("" = remote access off)
const int   MQTT_PORT = 8883;      // TLS port
const char* MQTT_USER = "YOUR_USERNAME";
const char* MQTT_PASS = "YOUR_PASSWORD";
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
const unsigned long JOB_SAVE_EVERY   = 5000;   // progress saved to flash every 5 s while pump is active

WebServer server(80);
DNSServer dnsServer;
Preferences prefs;
WiFiClientSecure secureClient;
PubSubClient mqtt(secureClient);

// ---------- Saved settings ----------
String wifiSsid, wifiPass;
int  thresholdPct = 30;
int  runSec       = 10;       // normal run time (Turn ON button + auto irrigation)
int  delaySec     = 5;        // normal countdown delay
bool autoMode     = true;
int  dryVal       = 3500;
int  wetVal       = 1500;
bool schedOn      = false;
int  schedMin     = 360;      // minutes after midnight (360 = 06:00)
int  schedRunSec  = 10;       // daily schedule: its own run time
int  schedDelaySec = 5;       // daily schedule: its own countdown delay
int  tzMin        = 0;        // minutes east of UTC, learned from your phone/browser
int  lastSchedKey = -1;
String tStatus, tCmd, tOnline, tAck;

// ---------- Runtime ----------
enum PumpState { IDLE = 0, COUNTDOWN = 1, RUNNING = 2 };
PumpState pumpState = IDLE;
int curDelay = 5, curRun = 10;          // values used by the CURRENT job
unsigned long stateStart = 0, lastRunEnd = 0, lastJobSave = 0;
bool hasRun = false;
bool apActive = false, internetOk = false, mdnsOn = false, ntpStarted = false;
unsigned long lastConnected = 0, lastRetry = 0, lastNetCheck = 0, lastNetInet = 0;
unsigned long lastAutoCheck = 0, lastMqttTry = 0, lastMqttPub = 0, lastSchedCheck = 0;
int lastRaw = 0, lastPct = 0;

// ---------- Web page (modern UI, no internet needed) ----------
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b1220" id="tcolor">
<title>AquaPump Control</title>
<style>
:root{--bg:#0b1220;--bg2:#0f1a2e;--card:#121d33;--card2:#172542;--line:#22335a;--txt:#e8eefc;--mute:#8da0c8;--acc:#3ddc97;--acc2:#2bb6ff;--warn:#ffb020;--bad:#ff5d6c;--track:#1d2d52;--inp:#0b1220;--glow1:#16315f55;--glow2:#0f5a4a40;--on:#06231a;--r:18px}
:root[data-theme=light]{--bg:#eef3f9;--bg2:#ffffff;--card:#ffffff;--card2:#eaf0fa;--line:#d5dfee;--txt:#14213d;--mute:#5d6f93;--acc:#14a76c;--acc2:#1479e0;--warn:#d98600;--bad:#d93a4a;--track:#e1e9f5;--inp:#f5f8fc;--glow1:#cfe3ff80;--glow2:#c9f3e270;--on:#ffffff}
@media(prefers-color-scheme:light){:root:not([data-theme]){--bg:#eef3f9;--bg2:#ffffff;--card:#ffffff;--card2:#eaf0fa;--line:#d5dfee;--txt:#14213d;--mute:#5d6f93;--acc:#14a76c;--acc2:#1479e0;--warn:#d98600;--bad:#d93a4a;--track:#e1e9f5;--inp:#f5f8fc;--glow1:#cfe3ff80;--glow2:#c9f3e270;--on:#ffffff}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
[hidden]{display:none!important}
html{-webkit-text-size-adjust:100%}
body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;color:var(--txt);background:radial-gradient(1200px 600px at 80% -10%,var(--glow1),transparent),radial-gradient(900px 500px at -10% 110%,var(--glow2),transparent),var(--bg);min-height:100vh}
.app{display:grid;grid-template-columns:230px minmax(0,1fr);min-height:100vh}
nav{position:sticky;top:0;height:100vh;padding:20px 14px;border-right:1px solid var(--line);background:var(--bg2);display:flex;flex-direction:column;gap:6px}
.nav-brand{display:flex;align-items:center;gap:12px;margin-bottom:18px;padding:0 6px}
.logo{width:42px;height:42px;border-radius:13px;background:linear-gradient(135deg,var(--acc2),var(--acc));display:grid;place-items:center;font-size:22px;box-shadow:0 6px 20px #2bb6ff44;flex:none}
.sub{font-size:.75rem;color:var(--mute)}
.tabbtn{display:flex;align-items:center;gap:12px;padding:13px 14px;border-radius:14px;border:0;background:transparent;color:var(--mute);font:inherit;font-weight:600;cursor:pointer;width:100%;text-align:left;min-height:48px}
.tabbtn .ic{font-size:1.25rem;width:26px;text-align:center}
.tabbtn.on{background:var(--card2);color:var(--txt)}
.tabbtn:hover{color:var(--txt)}
.nav-foot{margin-top:auto;padding:6px}
main{padding:22px 26px 44px;max-width:1120px;width:100%;margin:0 auto}
header{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:16px;flex-wrap:wrap}
.hdr-brand{display:none;align-items:center;gap:10px}
.hdr-brand b{font-size:1.05rem}
.chips{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-left:auto}
.chip{display:flex;align-items:center;gap:7px;background:var(--card);border:1px solid var(--line);padding:8px 12px;border-radius:99px;font-size:.78rem;color:var(--mute);white-space:nowrap}
.thbtn{background:var(--card);border:1px solid var(--line);color:var(--txt);border-radius:99px;padding:8px 12px;font:inherit;font-size:.78rem;cursor:pointer;min-height:36px}
.dot{width:9px;height:9px;border-radius:50%;background:#7a869f;flex:none}
.dot.ok{background:var(--acc);animation:pulse 2s infinite}.dot.bad{background:var(--bad)}.dot.wait{background:var(--warn)}
@keyframes pulse{0%{box-shadow:0 0 0 0 #3ddc9788}70%{box-shadow:0 0 0 8px #3ddc9700}100%{box-shadow:0 0 0 0 #3ddc9700}}
.tab{display:none}.tab.active{display:block;animation:fade .25s}
@keyframes fade{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px;margin-top:14px}
.card{background:linear-gradient(180deg,var(--card),var(--bg2));border:1px solid var(--line);border-radius:var(--r);padding:18px;min-width:0}
.card h3{margin:0 0 14px;font-size:.78rem;text-transform:uppercase;letter-spacing:1.4px;color:var(--mute);font-weight:600;display:flex;justify-content:space-between;gap:8px}
.c4{grid-column:span 4}.c5{grid-column:span 5}.c6{grid-column:span 6}.c7{grid-column:span 7}.c8{grid-column:span 8}.c12{grid-column:span 12}
.tiles{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}
.tile{background:var(--card);border:1px solid var(--line);border-radius:16px;padding:14px;cursor:pointer}
.tile small{display:block;color:var(--mute);font-size:.68rem;text-transform:uppercase;letter-spacing:1px}
.tile b{display:block;font-size:1.5rem;margin-top:4px}
.advice{margin-top:14px;padding:13px 15px;border-radius:14px;border:1px solid var(--line);border-left-width:5px;background:var(--card);font-size:.9rem}
.advice.ok{border-left-color:var(--acc)}.advice.warn{border-left-color:var(--warn)}.advice.info{border-left-color:var(--acc2)}
.gauge{position:relative;width:210px;height:210px;margin:auto}.gauge svg,.ring svg{transform:rotate(-90deg)}
.trk{stroke:var(--track)}
.mid{position:absolute;inset:0;display:grid;place-items:center;text-align:center}
.gauge .v{font-size:3rem;font-weight:700;line-height:1}.gauge .l{font-size:.75rem;color:var(--mute);margin-top:4px}
.soil{margin-top:14px;text-align:center;font-size:.92rem;font-weight:600}
.meta{display:flex;justify-content:space-between;margin-top:12px;font-size:.75rem;color:var(--mute)}
.pstate{display:flex;align-items:center;gap:18px;flex-wrap:wrap}
.ring{position:relative;width:130px;height:130px;flex:none}
.ring .t{font-size:1.9rem;font-weight:700}.ring .u{font-size:.7rem;color:var(--mute)}
.pinfo{flex:1;min-width:150px}
.badge{display:inline-flex;padding:8px 15px;border-radius:99px;font-weight:700;font-size:.85rem;background:var(--card2);color:var(--mute)}
.badge.run{background:#0e3d2c;color:#3ddc97}.badge.cd{background:#47340a;color:#ffb020}
.btns{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:16px}
button{font:inherit}
.btn{border:0;border-radius:14px;padding:15px;font-weight:700;color:var(--on);cursor:pointer;transition:transform .08s,filter .2s;width:100%;min-height:52px}
.btn:active{transform:scale(.97)}.btn:hover{filter:brightness(1.08)}
.b-on{background:linear-gradient(135deg,var(--acc),#20b97a)}
.b-off{background:linear-gradient(135deg,#ff7a86,var(--bad))}
.b-sec{background:var(--card2);color:var(--txt);border:1px solid var(--line);margin-top:10px}
.b-warn{background:transparent;color:var(--warn);border:1px solid var(--warn);margin-top:10px}
.b-save{margin-top:10px;background:linear-gradient(135deg,var(--acc2),#4a7dff);color:#fff}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:12px 0;border-bottom:1px solid var(--line);min-height:56px}
.row:last-of-type{border-bottom:0}.row .n{font-size:.95rem}.row .d{font-size:.72rem;color:var(--mute)}
.sw{position:relative;width:54px;height:32px;flex:none}.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;background:var(--track);border-radius:99px;transition:.25s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:26px;height:26px;left:3px;top:3px;border-radius:50%;background:#fff;transition:.25s;box-shadow:0 1px 3px #0004}
.sw input:checked+span{background:var(--acc)}.sw input:checked+span:before{transform:translateX(22px)}
.num,.txt,.sel{background:var(--inp);border:1px solid var(--line);color:var(--txt);border-radius:12px;padding:11px 12px;font:inherit;font-size:16px;min-height:46px}
.num{width:110px;text-align:right}
.txt,.sel{width:100%;margin-top:8px}
.num:focus,.txt:focus,.sel:focus{outline:2px solid var(--acc2);border-color:transparent}
.chartbox{height:160px}canvas{width:100%;height:100%;display:block}
.kv{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.kv div{background:var(--inp);border:1px solid var(--line);border-radius:12px;padding:10px 12px}
.kv small{display:block;color:var(--mute);font-size:.68rem;text-transform:uppercase;letter-spacing:1px}.kv b{font-size:.92rem;word-break:break-word}
.wxnow{display:flex;align-items:center;gap:18px;flex-wrap:wrap}
.wxico{font-size:4rem;line-height:1}
.wxtemp{font-size:3rem;font-weight:700;line-height:1}
.fc{display:grid;grid-template-columns:repeat(5,1fr);gap:10px}
.fc>div{background:var(--inp);border:1px solid var(--line);border-radius:14px;padding:12px 6px;text-align:center;font-size:.8rem}
.fc .e{font-size:1.7rem;margin:4px 0}
.fc b{display:block}
.res{display:flex;flex-direction:column;gap:6px;margin-top:10px}
.res button{background:var(--inp);border:1px solid var(--line);color:var(--txt);border-radius:12px;padding:12px;text-align:left;cursor:pointer;min-height:48px}
.seg{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.seg button{background:var(--inp);border:1px solid var(--line);color:var(--txt);border-radius:12px;padding:12px;cursor:pointer;min-height:48px}
.seg button.on{border-color:var(--acc2);background:var(--card2);font-weight:700}
.offline .card,.offline .tile{filter:saturate(.35)}
#toast{position:fixed;left:50%;bottom:90px;transform:translate(-50%,80px);background:var(--card2);color:var(--txt);border:1px solid var(--line);padding:12px 18px;border-radius:14px;font-size:.88rem;transition:.3s;opacity:0;z-index:50;max-width:90%;text-align:center;box-shadow:0 8px 24px #0005}
#toast.show{transform:translate(-50%,0);opacity:1}
#setup{position:fixed;inset:0;background:var(--bg);z-index:60;overflow:auto;padding:20px}
#setup .card{max-width:440px;margin:30px auto}
@media(max-width:899px){
.app{display:block}
nav{position:fixed;top:auto;bottom:0;left:0;right:0;height:auto;flex-direction:row;padding:6px 6px calc(6px + env(safe-area-inset-bottom));border-right:0;border-top:1px solid var(--line);z-index:30;justify-content:space-around;gap:2px}
.tabbtn{flex-direction:column;gap:2px;padding:7px 2px;font-size:.7rem;text-align:center;justify-content:center;min-height:54px;flex:1}
.nav-brand,.nav-foot{display:none}
.hdr-brand{display:flex}
main{padding:14px 14px 110px}
.c4,.c5,.c6,.c7,.c8{grid-column:span 12}
.tiles{grid-template-columns:repeat(2,1fr)}
.fc{display:flex;overflow-x:auto;gap:10px;padding-bottom:6px}
.fc>div{min-width:92px;flex:none}
.chip{font-size:.72rem;padding:7px 10px}
}
@media(max-width:380px){.gauge{width:180px;height:180px}.gauge svg{width:180px;height:180px}.wxtemp{font-size:2.4rem}}
</style></head><body>
<div class="app">
<nav>
<div class="nav-brand"><div class="logo">&#128167;</div><div><b>AquaPump</b><div class="sub">local dashboard</div></div></div>
<button class="tabbtn on" data-t="home" onclick="tab('home')"><span class="ic">&#127968;</span><span>Home</span></button>
<button class="tabbtn" data-t="weather" onclick="tab('weather')"><span class="ic">&#127780;</span><span>Weather</span></button>
<button class="tabbtn" data-t="plan" onclick="tab('plan')"><span class="ic">&#128197;</span><span>Plan</span></button>
<button class="tabbtn" data-t="device" onclick="tab('device')"><span class="ic">&#9881;</span><span>Device</span></button>
<div class="nav-foot sub">Smart irrigation controller</div>
</nav>
<main>
<header>
<div class="hdr-brand"><div class="logo" style="width:36px;height:36px;font-size:18px">&#128167;</div><b>AquaPump</b></div>
<div class="chips">
<div class="chip"><span class="dot wait" id="dDev"></span><span id="tDev">Connecting...</span></div>
<div class="chip"><span class="dot" id="dCloud"></span><span id="tCloud">Cloud</span></div>
<button class="thbtn" id="thBtn" onclick="cycleTheme()">Theme</button>
</div>
</header>
<div id="body">
<!-- HOME -->
<section class="tab active" data-t="home">
<div class="tiles">
<div class="tile"><small>Soil moisture</small><b id="tSoil">--%</b></div>
<div class="tile"><small>Pump</small><b id="tPump">--</b></div>
<div class="tile" onclick="tab('weather')"><small>Temperature</small><b id="tTemp">--</b></div>
<div class="tile" onclick="tab('weather')"><small>Rain today</small><b id="tRain">--</b></div>
</div>
<div class="advice" id="advH" hidden></div>
<div class="grid">
<section class="card c5"><h3>Soil moisture <span id="upd">--</span></h3>
<div class="gauge"><svg width="210" height="210" viewBox="0 0 210 210">
<defs><linearGradient id="gg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2bb6ff"/><stop offset="1" stop-color="#3ddc97"/></linearGradient></defs>
<circle class="trk" cx="105" cy="105" r="88" fill="none" stroke-width="16"/>
<circle id="garc" cx="105" cy="105" r="88" fill="none" stroke="url(#gg)" stroke-width="16" stroke-linecap="round" stroke-dasharray="552.9" stroke-dashoffset="552.9" style="transition:stroke-dashoffset .8s ease"/></svg>
<div class="mid"><div><div class="v" id="pct">--%</div><div class="l">moisture</div></div></div></div>
<div class="soil" id="soil">Waiting for data...</div>
<div class="meta"><span>Raw sensor: <b id="raw">-</b></span><span>Threshold: <b id="thrShow">-</b>%</span></div>
</section>
<section class="card c7"><h3>Pump</h3>
<div class="pstate">
<div class="ring"><svg width="130" height="130" viewBox="0 0 130 130">
<circle class="trk" cx="65" cy="65" r="56" fill="none" stroke-width="10"/>
<circle id="rarc" cx="65" cy="65" r="56" fill="none" stroke="#3ddc97" stroke-width="10" stroke-linecap="round" stroke-dasharray="351.9" stroke-dashoffset="351.9" style="transition:stroke-dashoffset .9s linear,stroke .3s"/></svg>
<div class="mid"><div><div class="t" id="remT">0</div><div class="u">sec left</div></div></div></div>
<div class="pinfo"><div class="badge" id="badge">OFF</div><div class="sub" style="margin-top:10px" id="pnote">Pump is idle.</div></div>
</div>
<div class="btns">
<button class="btn b-on" onclick="act(T.pump(true),'Start command sent')">&#9654; Turn ON</button>
<button class="btn b-off" onclick="act(T.pump(false),'Pump stopped')">&#9632; Stop now</button>
</div>
</section>
<section class="card c12"><h3>Moisture history <span>live &middot; last 10 min</span></h3>
<div class="chartbox"><canvas id="cv"></canvas></div></section>
</div>
</section>
<!-- WEATHER -->
<section class="tab" data-t="weather">
<div class="advice" id="advW" hidden></div>
<div class="grid">
<section class="card c7"><h3>Weather now <span id="wxPlace">--</span></h3>
<div class="wxnow">
<div class="wxico" id="wxIco">&#127780;</div>
<div><div class="wxtemp" id="wxTemp">--</div><div id="wxDesc" class="sub">Loading...</div></div>
</div>
<div class="kv" style="margin-top:16px">
<div><small>Feels like</small><b id="wxFeel">--</b></div>
<div><small>Humidity</small><b id="wxHum">--</b></div>
<div><small>Wind</small><b id="wxWind">--</b></div>
<div><small>Rain now</small><b id="wxPrec">--</b></div>
<div><small>Sunrise</small><b id="wxRise">--</b></div>
<div><small>Sunset</small><b id="wxSet">--</b></div>
</div>
<div class="sub" id="wxStatus" style="margin-top:12px"></div>
</section>
<section class="card c5"><h3>Location</h3>
<div class="sub">Pick your country, then type your district or city.</div>
<select class="sel" id="country"></select>
<input class="txt" id="place" placeholder="District or city (e.g. Khulna)" enterkeyhint="search" onkeydown="if(event.key==='Enter')searchPlace()">
<button class="btn b-save" onclick="searchPlace()">Search place</button>
<button class="btn b-sec" onclick="useGps()">Use my GPS location</button>
<div class="res" id="results"></div>
</section>
<section class="card c12"><h3>5-day forecast <span>rain chance</span></h3>
<div class="fc" id="fc"></div></section>
</div>
</section>
<!-- PLAN -->
<section class="tab" data-t="plan">
<div class="grid">
<section class="card c6"><h3>Daily schedule</h3>
<div class="row"><div><div class="n">Run every day</div><div class="d">Uses its own timings below</div></div>
<label class="sw"><input type="checkbox" id="son" onchange="act(T.sched(this.checked),'Schedule '+(this.checked?'enabled':'disabled'))"><span></span></label></div>
<div class="row"><div class="n">Start time</div><input class="num" type="time" id="stime" style="width:130px"></div>
<div class="row"><div class="n">Run duration (sec)</div><input class="num" type="number" inputmode="numeric" id="srun" min="1" max="3600"></div>
<div class="row"><div class="n">Countdown delay (sec)</div><input class="num" type="number" inputmode="numeric" id="sdly" min="0" max="600"></div>
<div class="sub" style="margin-top:6px">Pump device clock: <b id="clk">-</b></div>
<button class="btn b-save" onclick="saveSched()">Save schedule</button>
</section>
<section class="card c6"><h3>Automatic irrigation</h3>
<div class="row"><div><div class="n">Auto mode</div><div class="d">Water when soil is too dry</div></div>
<label class="sw"><input type="checkbox" id="auto" onchange="act(T.auto(this.checked),'Auto mode '+(this.checked?'on':'off'))"><span></span></label></div>
<div class="row"><div class="n">Moisture threshold (%)</div><input class="num" type="number" inputmode="numeric" id="thr" min="1" max="100"></div>
<div class="row"><div class="n">Run duration (sec)</div><input class="num" type="number" inputmode="numeric" id="run" min="1" max="3600"></div>
<div class="row"><div class="n">Countdown delay (sec)</div><input class="num" type="number" inputmode="numeric" id="dly" min="0" max="600"></div>
<button class="btn b-save" onclick="saveSet()">Save settings</button>
</section>
</div>
</section>
<!-- DEVICE -->
<section class="tab" data-t="device">
<div class="grid">
<section class="card c6"><h3>Sensor calibration <span>live raw: <b id="rawLive">-</b></span></h3>
<div class="sub">Hold the sensor in dry air and press DRY. Put it in water (up to the line) and press WET. A raw value near 0 means a wiring problem.</div>
<button class="btn b-sec" onclick="act(T.calDry(),'DRY value saved')">Set current as DRY</button>
<button class="btn b-sec" onclick="act(T.calWet(),'WET value saved')">Set current as WET</button>
</section>
<section class="card c6"><h3>Appearance</h3>
<div class="sub">Auto follows your phone or computer setting.</div>
<div class="seg" style="margin-top:12px">
<button id="th-auto" onclick="setTheme('auto')">Auto</button>
<button id="th-light" onclick="setTheme('light')">Light</button>
<button id="th-dark" onclick="setTheme('dark')">Dark</button>
</div>
</section>
<section class="card c6" data-local hidden><h3>Home Wi-Fi</h3>
<div class="sub">Leave blank to keep the current network. The pump restarts after saving.</div>
<input class="txt" id="ssid" placeholder="Wi-Fi name (SSID)" autocapitalize="off" autocomplete="off">
<input class="txt" id="pass" type="password" placeholder="Wi-Fi password" autocomplete="off">
<button class="btn b-save" onclick="saveWifi()">Save Wi-Fi &amp; restart</button>
<button class="btn b-warn" onclick="forget()">Forget Wi-Fi &amp; reset hotspot</button>
<div class="sub" id="wmsg" style="margin-top:8px"></div>
</section>
<section class="card c6" data-remote hidden><h3>Cloud connection</h3>
<div class="sub">This page talks to your pump through your MQTT cloud broker.</div>
<button class="btn b-sec" onclick="T.showSetup()">Change connection settings</button>
</section>
<section class="card c12"><h3>Network</h3>
<div class="kv">
<div><small>Mode</small><b id="iMode">-</b></div>
<div><small>Pump address</small><b id="iIp">-</b></div>
<div><small>Also at</small><b>waterpump.local</b></div>
<div><small>Pump internet</small><b id="iNet">-</b></div>
</div></section>
</div>
</section>
</div>
</main>
</div>
<div id="toast"></div>
<div id="setup" hidden>
<div class="card"><h3>Connect to your pump</h3>
<div class="sub">Enter the details from your EMQX Cloud deployment. They are saved only in this browser.</div>
<input class="txt" id="cHost" placeholder="Broker address (xxxx.emqxsl.com)" autocapitalize="off" autocomplete="off">
<input class="txt" id="cPort" placeholder="WebSocket TLS port" inputmode="numeric" value="8084">
<input class="txt" id="cUser" placeholder="Username" autocapitalize="off" autocomplete="off">
<input class="txt" id="cPass" type="password" placeholder="Password" autocomplete="off">
<input class="txt" id="cDev" placeholder="Device ID" value="pump1" autocapitalize="off">
<button class="btn b-save" onclick="T.saveCfg()">Save &amp; connect</button>
</div>
</div>
<script>
const $=id=>document.getElementById(id);
const store={get(k,d){try{const v=localStorage.getItem(k);return v===null?d:v}catch(e){return d}},set(k,v){try{localStorage.setItem(k,v)}catch(e){}}};
/* ---------- transport: talks directly to the ESP32 on this network ---------- */
const T=(function(){
let fails=0,cbS=null,cbL=null;
async function ok(u,o){try{const r=await fetch(u,o);return r.ok?r:null}catch(e){return null}}
const post=o=>ok('/save',{method:'POST',body:new URLSearchParams(o)});
async function poll(){
const r=await ok('/status');
if(!r){fails++;if(fails>2)cbL({dev:'bad',devTxt:'No response',cloud:'off',cloudTxt:'Cloud'});return;}
let j;try{j=await r.json()}catch(e){return}
fails=0;const m=j.mq;
cbL({dev:'ok',devTxt:'Pump device online',cloud:m===1?'ok':(m===2?'bad':'off'),cloudTxt:m===1?'Cloud connected':(m===2?'Cloud not connected':'Cloud off')});
cbS(j);
}
return{
local:true,
start(onS,onL){cbS=onS;cbL=onL;poll();setInterval(poll,2000);},
refresh(){return poll();},
async pump(on){return !!await ok(on?'/on':'/off')},
async auto(b){return !!await post({auto:b?1:0})},
async sched(b){return !!await post({son:b?1:0})},
async saveSet(o){return !!await post(o)},
async saveSched(o){return !!await post(o)},
async calDry(){return !!await ok('/calDry')},
async calWet(){return !!await ok('/calWet')},
async wifi(s,p){const r=await post({ssid:s,pass:p});return r?await r.text():null},
async forget(){return !!await ok('/forget')},
sync(){ok('/settime?epoch='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()));}
};
})();
/* ---------- helpers ---------- */
const G=552.9,R=351.9;
let filled=false,maxRem=1,prev='',hist=[],lastPct=0,lastThr=30,W=null,found=[],tt;
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('show');clearTimeout(tt);tt=setTimeout(()=>e.classList.remove('show'),2400);}
async function act(p,msg){const r=await p;toast(r?msg:'Could not reach the pump');if(T.refresh)T.refresh();}
/* ---------- theme ---------- */
function applyTheme(){
const t=store.get('aq_theme','auto'),d=document.documentElement;
if(t==='auto')d.removeAttribute('data-theme');else d.setAttribute('data-theme',t);
$('thBtn').textContent=({auto:'Theme: Auto',light:'Theme: Light',dark:'Theme: Dark'})[t];
['auto','light','dark'].forEach(k=>$('th-'+k).classList.toggle('on',k===t));
$('tcolor').content=getComputedStyle(d).getPropertyValue('--bg').trim()||'#0b1220';
drawChart();
}
function setTheme(t){store.set('aq_theme',t);applyTheme();}
function cycleTheme(){const o=['auto','light','dark'];setTheme(o[(o.indexOf(store.get('aq_theme','auto'))+1)%3]);}
if(window.matchMedia)try{matchMedia('(prefers-color-scheme: light)').addEventListener('change',applyTheme)}catch(e){}
/* ---------- tabs ---------- */
function tab(n){
document.querySelectorAll('.tab').forEach(e=>e.classList.toggle('active',e.dataset.t===n));
document.querySelectorAll('.tabbtn').forEach(e=>e.classList.toggle('on',e.dataset.t===n));
store.set('aq_tab',n);window.scrollTo(0,0);if(n==='home')drawChart();
}
/* ---------- link state ---------- */
function setLink(l){
$('dDev').className='dot '+l.dev;$('tDev').textContent=l.devTxt;
$('dCloud').className='dot '+(l.cloud==='off'?'':l.cloud);$('tCloud').textContent=l.cloudTxt;
$('body').classList.toggle('offline',l.dev==='bad');
}
/* ---------- pump status ---------- */
function onStatus(j){
$('upd').textContent='updated '+new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
$('pct').textContent=j.pct+'%';$('tSoil').textContent=j.pct+'%';
$('garc').style.strokeDashoffset=G*(1-j.pct/100);
$('raw').textContent=j.raw;$('rawLive').textContent=j.raw;$('thrShow').textContent=j.thr;
const bad=j.raw<50;
$('soil').textContent=bad?'Sensor not detected - check wiring':(j.pct<j.thr?'Soil is dry':(j.pct<j.thr+25?'Soil is okay':'Soil is well watered'));
$('soil').style.color=bad?'var(--bad)':(j.pct<j.thr?'var(--warn)':'var(--acc)');
const st=j.state;
if(st!==prev){maxRem=Math.max(j.rem,1);prev=st;}
if(j.rem>maxRem)maxRem=j.rem;
const b=$('badge');
b.textContent=st==='RUNNING'?'RUNNING':(st==='Countdown'?'STARTING SOON':'OFF');
b.className='badge'+(st==='RUNNING'?' run':(st==='Countdown'?' cd':''));
$('tPump').textContent=st==='RUNNING'?'Running':(st==='Countdown'?'Starting':'Off');
$('pnote').textContent=st==='RUNNING'?'Pump is watering now.':(st==='Countdown'?'Countdown before the pump starts.':'Pump is idle.');
$('remT').textContent=st==='OFF'?'0':j.rem;
const ring=$('rarc');ring.style.stroke=st==='Countdown'?'#ffb020':'#3ddc97';
ring.style.strokeDashoffset=st==='OFF'?R:R*(1-(j.rem/maxRem));
$('iMode').textContent=j.mode;$('iIp').textContent=j.ip;$('clk').textContent=j.clk||'not set';
$('iNet').textContent=j.mode==='Hotspot'?'n/a':(j.net?'Yes':'No (local only)');
if(!filled){
$('thr').value=j.thr;$('run').value=j.run;$('dly').value=j.dly;$('auto').checked=j.auto;
$('son').checked=j.son;$('stime').value=j.stime;$('srun').value=j.srun;$('sdly').value=j.sdly;
filled=true;T.sync();
}
lastThr=j.thr;
hist.push({t:Date.now(),v:j.pct});
const cut=Date.now()-600000;while(hist.length&&hist[0].t<cut)hist.shift();
drawChart();
}
function saveSet(){act(T.saveSet({thr:$('thr').value,run:$('run').value,dly:$('dly').value}),'Settings saved');}
function saveSched(){act(T.saveSched({srun:$('srun').value,sdly:$('sdly').value,stime:$('stime').value||'06:00'}),'Schedule saved');}
async function saveWifi(){
const s=$('ssid').value.trim();if(!s){toast('Enter the Wi-Fi name first');return;}
const t=await T.wifi(s,$('pass').value);$('wmsg').textContent=t||'No response from the pump';
}
async function forget(){
if(confirm('Erase saved Wi-Fi and restart in hotspot mode?')){await T.forget();$('wmsg').textContent='Restarting in hotspot mode. Join ESP32_WaterPump.';}
}
/* ---------- chart ---------- */
function drawChart(){
const cv=$('cv'),dpr=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;
if(!w||!h)return;
cv.width=w*dpr;cv.height=h*dpr;
const cs=getComputedStyle(document.documentElement),line=cs.getPropertyValue('--line').trim(),mute=cs.getPropertyValue('--mute').trim();
const c=cv.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);
const pad=28;c.strokeStyle=line;c.fillStyle=mute;c.font='11px sans-serif';c.lineWidth=1;
[0,50,100].forEach(v=>{const y=h-8-(v/100)*(h-16);c.beginPath();c.moveTo(pad,y);c.lineTo(w,y);c.stroke();c.fillText(v+'%',0,y+4);});
const ty=h-8-(lastThr/100)*(h-16);
c.strokeStyle='#ffb020aa';c.setLineDash([5,5]);c.beginPath();c.moveTo(pad,ty);c.lineTo(w,ty);c.stroke();c.setLineDash([]);
if(hist.length<2)return;
const t0=Date.now()-600000,X=t=>pad+((t-t0)/600000)*(w-pad),Y=v=>h-8-(v/100)*(h-16);
const g=c.createLinearGradient(0,0,0,h);g.addColorStop(0,'#2bb6ff55');g.addColorStop(1,'#2bb6ff00');
c.beginPath();c.moveTo(X(hist[0].t),h-8);hist.forEach(p=>c.lineTo(X(p.t),Y(p.v)));c.lineTo(X(hist[hist.length-1].t),h-8);c.closePath();c.fillStyle=g;c.fill();
c.beginPath();hist.forEach((p,i)=>i?c.lineTo(X(p.t),Y(p.v)):c.moveTo(X(p.t),Y(p.v)));
c.strokeStyle='#3ddc97';c.lineWidth=2.5;c.lineJoin='round';c.stroke();
}
window.addEventListener('resize',drawChart);
/* ---------- weather ---------- */
const WX={0:['Clear sky','\u2600\uFE0F'],1:['Mostly clear','\u{1F324}\uFE0F'],2:['Partly cloudy','\u26C5'],3:['Overcast','\u2601\uFE0F'],45:['Fog','\u{1F32B}\uFE0F'],48:['Fog','\u{1F32B}\uFE0F'],
51:['Light drizzle','\u{1F326}\uFE0F'],53:['Drizzle','\u{1F326}\uFE0F'],55:['Heavy drizzle','\u{1F327}\uFE0F'],56:['Freezing drizzle','\u{1F327}\uFE0F'],57:['Freezing drizzle','\u{1F327}\uFE0F'],
61:['Light rain','\u{1F326}\uFE0F'],63:['Rain','\u{1F327}\uFE0F'],65:['Heavy rain','\u{1F327}\uFE0F'],66:['Freezing rain','\u{1F327}\uFE0F'],67:['Freezing rain','\u{1F327}\uFE0F'],
71:['Light snow','\u{1F328}\uFE0F'],73:['Snow','\u{1F328}\uFE0F'],75:['Heavy snow','\u2744\uFE0F'],77:['Snow grains','\u{1F328}\uFE0F'],
80:['Rain showers','\u{1F326}\uFE0F'],81:['Rain showers','\u{1F327}\uFE0F'],82:['Violent showers','\u{1F327}\uFE0F'],85:['Snow showers','\u{1F328}\uFE0F'],86:['Snow showers','\u{1F328}\uFE0F'],
95:['Thunderstorm','\u26C8\uFE0F'],96:['Thunderstorm, hail','\u26C8\uFE0F'],99:['Thunderstorm, hail','\u26C8\uFE0F']};
const RAINY=[51,53,55,56,57,61,63,65,66,67,80,81,82,95,96,99];
const wx=c=>WX[c]||['Unknown','\u{1F321}\uFE0F'];
const COUNTRIES=[['BD','Bangladesh'],['IN','India'],['PK','Pakistan'],['NP','Nepal'],['LK','Sri Lanka'],['BT','Bhutan'],['MM','Myanmar'],['MV','Maldives'],['AE','United Arab Emirates'],['SA','Saudi Arabia'],['QA','Qatar'],['KW','Kuwait'],['OM','Oman'],['MY','Malaysia'],['SG','Singapore'],['ID','Indonesia'],['TH','Thailand'],['VN','Vietnam'],['PH','Philippines'],['CN','China'],['JP','Japan'],['KR','South Korea'],['AU','Australia'],['NZ','New Zealand'],['GB','United Kingdom'],['IE','Ireland'],['DE','Germany'],['FR','France'],['IT','Italy'],['ES','Spain'],['NL','Netherlands'],['TR','Turkey'],['EG','Egypt'],['NG','Nigeria'],['KE','Kenya'],['ZA','South Africa'],['US','United States'],['CA','Canada'],['MX','Mexico'],['BR','Brazil'],['AR','Argentina'],['','Any country (search everywhere)']];
const DEFLOC={name:'Dhaka',admin1:'Dhaka Division',country:'Bangladesh',cc:'BD',lat:23.8103,lon:90.4125};
function loc(){try{const l=JSON.parse(store.get('aq_loc','null'));if(l&&l.lat!=null)return l;}catch(e){}return DEFLOC;}
function initCountries(){
const s=$('country');COUNTRIES.forEach(c=>{const o=document.createElement('option');o.value=c[0];o.textContent=c[1];s.appendChild(o);});
s.value=loc().cc||'BD';
}
function placeLabel(l){return [l.name,l.admin1&&l.admin1!==l.name?l.admin1:'',l.country].filter(Boolean).join(', ');}
async function searchPlace(){
const q=$('place').value.trim();if(!q){toast('Type a district or city name');return;}
const cc=$('country').value,box=$('results');box.textContent='Searching...';
try{
const r=await fetch('https://geocoding-api.open-meteo.com/v1/search?name='+encodeURIComponent(q)+'&count=8&language=en&format=json'+(cc?'&countryCode='+cc:''));
if(!r.ok)throw 0;const j=await r.json();found=j.results||[];box.textContent='';
if(!found.length){box.textContent='No place found. Try another spelling or choose "Any country".';return;}
found.forEach((p,i)=>{const b=document.createElement('button');b.textContent=[p.name,p.admin1,p.country].filter(Boolean).join(', ');b.onclick=()=>pickPlace(i);box.appendChild(b);});
}catch(e){box.textContent='Search needs internet on this device.';}
}
function pickPlace(i){
const p=found[i];if(!p)return;
store.set('aq_loc',JSON.stringify({name:p.name,admin1:p.admin1||'',country:p.country||'',cc:p.country_code||'',lat:p.latitude,lon:p.longitude}));
$('results').textContent='';$('place').value='';loadWx();toast('Location set: '+p.name);
}
function useGps(){
if(!navigator.geolocation){toast('GPS is not available here. Search by name instead.');return;}
navigator.geolocation.getCurrentPosition(p=>{
store.set('aq_loc',JSON.stringify({name:'My location',admin1:'GPS',country:'',cc:'',lat:+p.coords.latitude.toFixed(4),lon:+p.coords.longitude.toFixed(4)}));
loadWx();toast('Using your GPS location');
},()=>toast('GPS blocked or unavailable. Search by name instead.'),{timeout:10000});
}
async function loadWx(){
const L=loc();$('wxPlace').textContent=placeLabel(L);$('wxStatus').textContent='Loading weather...';
try{
const u='https://api.open-meteo.com/v1/forecast?latitude='+L.lat+'&longitude='+L.lon+'&current=temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset&timezone=auto&forecast_days=5';
const r=await fetch(u);if(!r.ok)throw 0;W=await r.json();renderWx();
}catch(e){
W=null;$('wxStatus').textContent='Weather needs internet on the phone or computer you are using. It will retry automatically.';
$('tTemp').textContent='--';$('tRain').textContent='--';advice();
}
}
function renderWx(){
const c=W.current,d=W.daily,w=wx(c.weather_code);
$('wxIco').textContent=w[1];$('wxTemp').textContent=Math.round(c.temperature_2m)+'\u00B0C';$('wxDesc').textContent=w[0];
$('wxFeel').textContent=Math.round(c.apparent_temperature)+'\u00B0C';$('wxHum').textContent=c.relative_humidity_2m+'%';
$('wxWind').textContent=Math.round(c.wind_speed_10m)+' km/h';$('wxPrec').textContent=c.precipitation+' mm';
$('wxRise').textContent=(d.sunrise[0]||'').split('T')[1]||'--';$('wxSet').textContent=(d.sunset[0]||'').split('T')[1]||'--';
$('wxStatus').textContent='Updated '+new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})+' \u00B7 data by Open-Meteo';
$('tTemp').textContent=Math.round(c.temperature_2m)+'\u00B0C';
$('tRain').textContent=(d.precipitation_probability_max[0]!=null?d.precipitation_probability_max[0]:'--')+'%';
const fc=$('fc');fc.textContent='';
d.time.forEach((t,i)=>{
const x=wx(d.weather_code[i]),el=document.createElement('div');
const day=i===0?'Today':new Date(t+'T12:00').toLocaleDateString([],{weekday:'short'});
el.innerHTML='<b></b><div class="e"></div><b></b><span></span><div class="sub"></div>';
el.children[0].textContent=day;el.children[1].textContent=x[1];
el.children[2].textContent=Math.round(d.temperature_2m_max[i])+'\u00B0 / '+Math.round(d.temperature_2m_min[i])+'\u00B0';
el.children[3].textContent=x[0];
el.children[4].textContent='Rain '+(d.precipitation_probability_max[i]!=null?d.precipitation_probability_max[i]:'--')+'%';
fc.appendChild(el);
});
advice();
}
function advice(){
const h=$('advH'),w=$('advW');
if(!W){h.hidden=true;w.hidden=true;return;}
const c=W.current,p=W.daily.precipitation_probability_max[0]||0;let cls='ok',txt;
if(c.precipitation>0.2||RAINY.indexOf(c.weather_code)>=0){cls='info';txt='It is raining now, so the soil is getting water. You can skip pumping.';}
else if(p>=60){cls='info';txt='Rain is likely today ('+p+'%). Consider waiting before you water.';}
else if(c.temperature_2m>=35){cls='warn';txt='Very hot today ('+Math.round(c.temperature_2m)+'\u00B0C). Water early morning or evening so less evaporates.';}
else{txt='Good conditions for watering today. Rain chance is '+p+'%.';}
[h,w].forEach(e=>{e.className='advice '+cls;e.textContent=txt;e.hidden=false;});
}
/* ---------- start ---------- */
document.querySelectorAll('[data-local]').forEach(e=>e.hidden=!T.local);
document.querySelectorAll('[data-remote]').forEach(e=>e.hidden=T.local);
initCountries();applyTheme();
tab(store.get('aq_tab','home'));
loadWx();setInterval(loadWx,900000);
T.start(onStatus,setLink);
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

// Saves the current sensor reading as the DRY or WET calibration point
void doCal(bool dry) {
  int raw = readRaw();
  if (dry) { dryVal = raw; prefs.putInt("dry", raw); }
  else     { wetVal = raw; prefs.putInt("wet", raw); }
  lastRaw = raw; lastPct = rawToPct(raw);
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
  wifiSsid      = prefs.getString("ssid", "");
  wifiPass      = prefs.getString("pass", "");
  thresholdPct  = prefs.getInt("thr", 30);
  runSec        = prefs.getInt("run", 10);
  delaySec      = prefs.getInt("dly", 5);
  autoMode      = prefs.getBool("auto", true);
  dryVal        = prefs.getInt("dry", 3500);
  wetVal        = prefs.getInt("wet", 1500);
  schedOn       = prefs.getBool("son", false);
  schedMin      = prefs.getInt("smin", 360);
  schedRunSec   = prefs.getInt("srun", runSec);     // first boot of V4: copy normal value
  schedDelaySec = prefs.getInt("sdly", delaySec);
  tzMin         = prefs.getInt("tz", 0);
  lastSchedKey  = prefs.getInt("skey", -1);
  String base = String("waterpump/") + DEVICE_ID;
  tStatus = base + "/status"; tCmd = base + "/cmd";
  tOnline = base + "/online"; tAck = base + "/ack";
}

// ---------- Job memory (power-cut resume) ----------
// Saves what the pump is doing + the REMAINING seconds, so after a reboot it continues.
void saveJob() {
  if (pumpState == IDLE) { prefs.putUChar("jst", 0); return; }
  long el  = (long)((millis() - stateStart) / 1000);
  long rem = (pumpState == COUNTDOWN) ? curDelay - el : curRun - el;
  if (rem < 1) rem = 1;
  prefs.putInt("jrem", (int)rem);
  prefs.putInt("jrun", curRun);
  prefs.putUChar("jst", (uint8_t)pumpState);
  lastJobSave = millis();
}

void resumeJob() {
  uint8_t js = prefs.getUChar("jst", 0);
  int rem = prefs.getInt("jrem", 0);
  int run = prefs.getInt("jrun", runSec);
  if (js == COUNTDOWN && rem > 0) {
    curDelay = rem; curRun = max(1, run);
    pumpState = COUNTDOWN; stateStart = millis();
    Serial.printf("Power restored: resuming COUNTDOWN, %d s left, then run %d s\n", rem, curRun);
  } else if (js == RUNNING && rem > 0) {
    curRun = rem;
    relayWrite(true);
    pumpState = RUNNING; stateStart = millis();
    Serial.printf("Power restored: resuming RUN, %d s left\n", rem);
  }
}

// ---------- Pump control ----------
void pumpStopNow() {
  relayWrite(false);
  if (pumpState == RUNNING) { lastRunEnd = millis(); hasRun = true; }
  pumpState = IDLE;
  digitalWrite(LED_PIN, LOW);
  saveJob();                                   // clears the saved job
  Serial.println("Pump OFF");
}

void pumpStartCountdown(int dly, int run) {
  if (pumpState != IDLE) return;
  curDelay = dly; curRun = run;
  pumpState = COUNTDOWN;
  stateStart = millis();
  saveJob();
  Serial.printf("Countdown %d s, then run %d s\n", dly, run);
}

void pumpRun() {
  flashLed(2);
  relayWrite(true);
  pumpState = RUNNING;
  stateStart = millis();
  saveJob();
  Serial.println("Pump ON");
}

void updatePump() {
  unsigned long now = millis();
  if (pumpState == COUNTDOWN) {
    digitalWrite(LED_PIN, (now / 100) % 2);
    if (now - stateStart >= (unsigned long)curDelay * 1000UL) pumpRun();
  } else if (pumpState == RUNNING) {
    digitalWrite(LED_PIN, HIGH);
    if (now - stateStart >= (unsigned long)curRun * 1000UL) pumpStopNow();
  }
  if (pumpState != IDLE && now - lastJobSave >= JOB_SAVE_EVERY) saveJob();
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
  if (lastPct < thresholdPct && soaked) pumpStartCountdown(delaySec, runSec);
}

// Daily schedule: uses its OWN delay + run time
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
    pumpStartCountdown(schedDelaySec, schedRunSec);
  }
}

// ---------- Status JSON ----------
String buildStatus() {
  const char* st = pumpState == IDLE ? "OFF" : (pumpState == COUNTDOWN ? "Countdown" : "RUNNING");
  long rem = 0;
  long el = (long)((millis() - stateStart) / 1000);
  if (pumpState == COUNTDOWN) rem = curDelay - el;
  if (pumpState == RUNNING)   rem = curRun - el;
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
  s += ",\"srun\":" + String(schedRunSec);
  s += ",\"sdly\":" + String(schedDelaySec);
  s += ",\"clk\":\"" + clockStr() + "\"";
  s += "}";
  return s;
}

// ---------- Remote commands ----------
bool applyCommand(String msg) {
  msg.trim();
  msg.toLowerCase();
  Serial.println("Command: " + msg);
  if (msg == "on")            { pumpStartCountdown(delaySec, runSec); return true; }
  if (msg == "off")           { pumpStopNow(); return true; }
  if (msg == "cal_dry")       { doCal(true);  return true; }
  if (msg == "cal_wet")       { doCal(false); return true; }
  if (msg == "auto_on")       { autoMode = true;  prefs.putBool("auto", true);  return true; }
  if (msg == "auto_off")      { autoMode = false; prefs.putBool("auto", false); return true; }
  if (msg == "sched_on")      { schedOn = true;   prefs.putBool("son", true);   return true; }
  if (msg == "sched_off")     { schedOn = false;  prefs.putBool("son", false);  return true; }
  if (msg.startsWith("thr=")) { thresholdPct = constrain(msg.substring(4).toInt(), 1, 100); prefs.putInt("thr", thresholdPct); return true; }
  if (msg.startsWith("run=")) { runSec   = constrain(msg.substring(4).toInt(), 1, 3600);    prefs.putInt("run", runSec);        return true; }
  if (msg.startsWith("dly=")) { delaySec = constrain(msg.substring(4).toInt(), 0, 600);     prefs.putInt("dly", delaySec);      return true; }
  if (msg.startsWith("srun=")) { schedRunSec   = constrain(msg.substring(5).toInt(), 1, 3600); prefs.putInt("srun", schedRunSec);   return true; }
  if (msg.startsWith("sdly=")) { schedDelaySec = constrain(msg.substring(5).toInt(), 0, 600);  prefs.putInt("sdly", schedDelaySec); return true; }
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

// Keeps the resumed pump timer running while Wi-Fi is still connecting at boot
bool waitSta(unsigned long ms) {
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < ms) {
    digitalWrite(LED_PIN, (millis() / 250) % 2);
    updatePump();
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
  if (pumpState == IDLE) digitalWrite(LED_PIN, LOW);
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
void handleOn()     { pumpStartCountdown(delaySec, runSec); ackBlink(); server.send(200, "text/plain", "OK"); }
void handleOff()    { pumpStopNow();                        ackBlink(); server.send(200, "text/plain", "OK"); }

void handleCal(bool dry) {
  doCal(dry);
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
  if (server.hasArg("srun") && server.arg("srun").length()) { schedRunSec   = constrain(server.arg("srun").toInt(), 1, 3600); prefs.putInt("srun", schedRunSec); }
  if (server.hasArg("sdly") && server.arg("sdly").length()) { schedDelaySec = constrain(server.arg("sdly").toInt(), 0, 600);  prefs.putInt("sdly", schedDelaySec); }
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
    saveJob();                                   // keep a running job across this restart too
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
  saveJob();
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
  relayWrite(false);               // pump OFF at boot (unless a saved job is resumed below)
  pinMode(LED_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(SENSOR_PIN, ADC_11db);

  loadSettings();
  resumeJob();                     // continue the interrupted run / countdown, if any
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
