/*
  ESP32 Smart Water Pump & Soil Monitor  -  VERSION 5
  ---------------------------------------------------
  NEW in V5:
    - Modern dashboard (gauge, countdown ring, live graph, switches) now also
      runs on the ESP32 itself: http://waterpump.local or the IP address.
      It needs NO internet (works in hotspot mode too).

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
const char* MQTT_HOST = "qf66cf6f.ala.eu-central-1.emqxsl.com";        // ("" = remote access off)
const int   MQTT_PORT = 8883;      // TLS port
const char* MQTT_USER = "pumpuser";
const char* MQTT_PASS = "11111111";
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
<meta name="theme-color" content="#0b1220">
<title>AquaPump Control</title>
<style>
:root{--bg:#0b1220;--bg2:#0f1a2e;--card:#121d33;--card2:#172542;--line:#22335a;--txt:#e8eefc;--mute:#8da0c8;--acc:#3ddc97;--acc2:#2bb6ff;--warn:#ffb020;--bad:#ff5d6c;--r:18px}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{margin:0;font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif;color:var(--txt);background:radial-gradient(1200px 600px at 80% -10%,#16315f55,transparent),radial-gradient(900px 500px at -10% 110%,#0f5a4a40,transparent),var(--bg);min-height:100vh;padding-bottom:40px}
.wrap{max-width:980px;margin:auto;padding:16px}
header{display:flex;align-items:center;justify-content:space-between;gap:12px;margin:6px 0 16px;flex-wrap:wrap}
.brand{display:flex;align-items:center;gap:12px}
.logo{width:42px;height:42px;border-radius:13px;background:linear-gradient(135deg,var(--acc2),var(--acc));display:grid;place-items:center;font-size:22px;box-shadow:0 6px 20px #2bb6ff44}
h1{font-size:1.15rem;margin:0}.sub{font-size:.75rem;color:var(--mute)}
.chips{display:flex;gap:8px;flex-wrap:wrap}
.chip{display:flex;align-items:center;gap:7px;background:var(--card);border:1px solid var(--line);padding:7px 12px;border-radius:99px;font-size:.78rem;color:var(--mute)}
.dot{width:9px;height:9px;border-radius:50%;background:#556}
.dot.ok{background:var(--acc);animation:pulse 2s infinite}.dot.bad{background:var(--bad)}.dot.wait{background:var(--warn)}
@keyframes pulse{0%{box-shadow:0 0 0 0 #3ddc9788}70%{box-shadow:0 0 0 8px #3ddc9700}100%{box-shadow:0 0 0 0 #3ddc9700}}
.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:14px}
.card{background:linear-gradient(180deg,var(--card),var(--bg2));border:1px solid var(--line);border-radius:var(--r);padding:18px}
.card h3{margin:0 0 14px;font-size:.78rem;text-transform:uppercase;letter-spacing:1.4px;color:var(--mute);font-weight:600;display:flex;justify-content:space-between}
.c5{grid-column:span 5}.c7{grid-column:span 7}.c6{grid-column:span 6}.c12{grid-column:span 12}
@media(max-width:760px){.c5,.c7,.c6{grid-column:span 12}}
.gauge{position:relative;width:210px;height:210px;margin:auto}.gauge svg,.ring svg{transform:rotate(-90deg)}
.mid{position:absolute;inset:0;display:grid;place-items:center;text-align:center}
.gauge .v{font-size:3rem;font-weight:700;line-height:1}.gauge .l{font-size:.75rem;color:var(--mute);margin-top:4px}
.soil{margin-top:14px;text-align:center;font-size:.9rem;font-weight:600}
.meta{display:flex;justify-content:space-between;margin-top:12px;font-size:.75rem;color:var(--mute)}
.pstate{display:flex;align-items:center;gap:18px;flex-wrap:wrap}
.ring{position:relative;width:130px;height:130px;flex:none}
.ring .t{font-size:1.9rem;font-weight:700}.ring .u{font-size:.7rem;color:var(--mute)}
.pinfo{flex:1;min-width:150px}
.badge{display:inline-flex;padding:7px 14px;border-radius:99px;font-weight:700;font-size:.85rem;background:#26365f;color:var(--mute)}
.badge.run{background:#0e3d2c;color:var(--acc)}.badge.cd{background:#47340a;color:var(--warn)}
.btns{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:16px}
button{font:inherit;border:0;border-radius:14px;padding:14px;font-weight:700;color:#06231a;cursor:pointer;transition:transform .08s,filter .2s;width:100%}
button:active{transform:scale(.97)}button:hover{filter:brightness(1.1)}
.b-on{background:linear-gradient(135deg,var(--acc),#20b97a)}
.b-off{background:linear-gradient(135deg,#ff7a86,var(--bad));color:#2a050a}
.b-sec{background:var(--card2);color:var(--txt);border:1px solid var(--line);margin-top:8px}
.b-warn{background:#47340a;color:var(--warn);margin-top:8px}
.b-save{margin-top:8px;background:linear-gradient(135deg,var(--acc2),#4a7dff);color:#031326}
.row{display:flex;justify-content:space-between;align-items:center;gap:12px;padding:10px 0;border-bottom:1px solid #1c2c4f}
.row:last-of-type{border-bottom:0}.row .n{font-size:.92rem}.row .d{font-size:.72rem;color:var(--mute)}
.sw{position:relative;width:48px;height:28px;flex:none}.sw input{opacity:0;width:0;height:0}
.sw span{position:absolute;inset:0;background:#2a3a63;border-radius:99px;transition:.25s;cursor:pointer}
.sw span:before{content:"";position:absolute;width:22px;height:22px;left:3px;top:3px;border-radius:50%;background:#fff;transition:.25s}
.sw input:checked+span{background:var(--acc)}.sw input:checked+span:before{transform:translateX(20px)}
.num{width:96px;background:var(--bg);border:1px solid var(--line);color:var(--txt);border-radius:10px;padding:9px 10px;font:inherit;text-align:right}
.txt{width:100%;background:var(--bg);border:1px solid var(--line);color:var(--txt);border-radius:10px;padding:10px;font:inherit;margin-top:8px}
.num:focus,.txt:focus{outline:2px solid var(--acc2);border-color:transparent}
.chartbox{height:150px}canvas{width:100%;height:100%;display:block}
.kv{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.kv div{background:var(--bg);border:1px solid var(--line);border-radius:12px;padding:10px 12px}
.kv small{display:block;color:var(--mute);font-size:.68rem;text-transform:uppercase;letter-spacing:1px}.kv b{font-size:.92rem;word-break:break-all}
#toast{position:fixed;left:50%;bottom:24px;transform:translate(-50%,80px);background:#1b2b52;border:1px solid var(--line);padding:11px 18px;border-radius:12px;font-size:.85rem;transition:.3s;opacity:0;z-index:9;max-width:90%;text-align:center}
#toast.show{transform:translate(-50%,0);opacity:1}
.offline .card{filter:saturate(.4)}
</style></head><body>
<div class="wrap">
<header>
 <div class="brand"><div class="logo">&#128167;</div><div><h1>AquaPump Control</h1><div class="sub">Smart irrigation &middot; local dashboard</div></div></div>
 <div class="chips">
  <div class="chip"><span class="dot wait" id="dEsp"></span><span id="tEsp">Connecting...</span></div>
  <div class="chip"><span class="dot" id="dCloud"></span><span id="tCloud">Cloud</span></div>
 </div>
</header>
<div class="grid" id="main">
 <section class="card c5"><h3>Soil moisture <span id="upd">--</span></h3>
  <div class="gauge"><svg width="210" height="210" viewBox="0 0 210 210">
   <defs><linearGradient id="gg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2bb6ff"/><stop offset="1" stop-color="#3ddc97"/></linearGradient></defs>
   <circle cx="105" cy="105" r="88" fill="none" stroke="#1d2d52" stroke-width="16"/>
   <circle id="garc" cx="105" cy="105" r="88" fill="none" stroke="url(#gg)" stroke-width="16" stroke-linecap="round" stroke-dasharray="552.9" stroke-dashoffset="552.9" style="transition:stroke-dashoffset .8s ease"/></svg>
   <div class="mid"><div><div class="v" id="pct">--%</div><div class="l">moisture</div></div></div></div>
  <div class="soil" id="soil">Waiting for data...</div>
  <div class="meta"><span>Raw sensor: <b id="raw">-</b></span><span>Threshold: <b id="thrShow">-</b>%</span></div>
 </section>

 <section class="card c7"><h3>Pump</h3>
  <div class="pstate">
   <div class="ring"><svg width="130" height="130" viewBox="0 0 130 130">
    <circle cx="65" cy="65" r="56" fill="none" stroke="#1d2d52" stroke-width="10"/>
    <circle id="rarc" cx="65" cy="65" r="56" fill="none" stroke="#3ddc97" stroke-width="10" stroke-linecap="round" stroke-dasharray="351.9" stroke-dashoffset="351.9" style="transition:stroke-dashoffset .9s linear,stroke .3s"/></svg>
    <div class="mid"><div><div class="t" id="remT">0</div><div class="u">sec left</div></div></div></div>
   <div class="pinfo"><div class="badge" id="badge">OFF</div><div class="sub" style="margin-top:10px" id="pnote">Pump is idle.</div></div>
  </div>
  <div class="btns">
   <button class="b-on" onclick="cmd('/on','Start command sent')">&#9654; Turn ON</button>
   <button class="b-off" onclick="cmd('/off','Pump stopped')">&#9632; Stop now</button>
  </div>
 </section>

 <section class="card c12"><h3>Moisture history <span>live &middot; last 10 min</span></h3>
  <div class="chartbox"><canvas id="cv"></canvas></div></section>

 <section class="card c6"><h3>Daily schedule</h3>
  <div class="row"><div><div class="n">Run every day</div><div class="d">Uses its own timings below</div></div>
   <label class="sw"><input type="checkbox" id="son" onchange="post({son:this.checked?1:0},'Schedule '+(this.checked?'enabled':'disabled'))"><span></span></label></div>
  <div class="row"><div class="n">Start time</div><input class="num" type="time" id="stime" style="width:120px"></div>
  <div class="row"><div class="n">Run duration (sec)</div><input class="num" type="number" id="srun" min="1" max="3600"></div>
  <div class="row"><div class="n">Countdown delay (sec)</div><input class="num" type="number" id="sdly" min="0" max="600"></div>
  <div class="sub" style="margin-top:6px">Device clock: <b id="clk">-</b></div>
  <button class="b-save" onclick="saveSched()">Save schedule</button>
 </section>

 <section class="card c6"><h3>Automatic irrigation</h3>
  <div class="row"><div><div class="n">Auto mode</div><div class="d">Water when soil is too dry</div></div>
   <label class="sw"><input type="checkbox" id="auto" onchange="post({auto:this.checked?1:0},'Auto mode '+(this.checked?'on':'off'))"><span></span></label></div>
  <div class="row"><div class="n">Moisture threshold (%)</div><input class="num" type="number" id="thr" min="1" max="100"></div>
  <div class="row"><div class="n">Run duration (sec)</div><input class="num" type="number" id="run" min="1" max="3600"></div>
  <div class="row"><div class="n">Countdown delay (sec)</div><input class="num" type="number" id="dly" min="0" max="600"></div>
  <button class="b-save" onclick="saveSet()">Save settings</button>
 </section>

 <section class="card c6"><h3>Home Wi-Fi</h3>
  <div class="sub">Leave blank to keep the current network.</div>
  <input class="txt" id="ssid" placeholder="Wi-Fi name (SSID)">
  <input class="txt" id="pass" type="password" placeholder="Wi-Fi password">
  <button class="b-save" onclick="saveWifi()">Save Wi-Fi &amp; restart</button>
  <div class="sub" id="wmsg" style="margin-top:8px"></div>
 </section>

 <section class="card c6"><h3>Sensor calibration</h3>
  <div class="sub">Sensor in dry air, press DRY. Sensor in water, press WET.</div>
  <button class="b-sec" onclick="cmd('/calDry','DRY value saved')">Set current as DRY</button>
  <button class="b-sec" onclick="cmd('/calWet','WET value saved')">Set current as WET</button>
  <button class="b-warn" onclick="forget()">Forget Wi-Fi &amp; reset hotspot</button>
 </section>

 <section class="card c12"><h3>Network</h3>
  <div class="kv">
   <div><small>Mode</small><b id="iMode">-</b></div>
   <div><small>Address</small><b id="iIp">-</b></div>
   <div><small>Also at</small><b>waterpump.local</b></div>
   <div><small>Internet</small><b id="iNet">-</b></div>
  </div></section>
</div></div>
<div id="toast"></div>
<script>
const $=id=>document.getElementById(id);
const G=552.9,R=351.9;
let filled=false,maxRem=1,prev='',hist=[],fails=0,tt;
function toast(t){const e=$('toast');e.textContent=t;e.classList.add('show');clearTimeout(tt);tt=setTimeout(()=>e.classList.remove('show'),2200);}
async function api(u,o,m){try{const r=await fetch(u,o);if(!r.ok)throw 0;if(m)toast(m);return r;}catch(e){toast('No response from ESP32');}}
async function cmd(u,m){await api(u,null,m);refresh();}
async function post(o,m){return api('/save',{method:'POST',body:new URLSearchParams(o)},m);}
function sync(){fetch('/settime?epoch='+Math.floor(Date.now()/1000)+'&tz='+(-new Date().getTimezoneOffset()));}
function saveSet(){post({thr:$('thr').value,run:$('run').value,dly:$('dly').value},'Settings saved');}
function saveSched(){post({srun:$('srun').value,sdly:$('sdly').value,stime:$('stime').value||'06:00'},'Schedule saved');}
async function saveWifi(){
 if(!$('ssid').value.trim()){toast('Enter the Wi-Fi name first');return;}
 const r=await post({ssid:$('ssid').value,pass:$('pass').value});
 if(r)$('wmsg').textContent=await r.text();
}
async function forget(){
 if(confirm('Erase saved Wi-Fi and restart in hotspot mode?')){await fetch('/forget');$('wmsg').textContent='Restarting in hotspot mode. Join ESP32_WaterPump.';}
}
function setEsp(ok){
 $('dEsp').className='dot '+(ok?'ok':'bad');$('tEsp').textContent=ok?'Pump device online':'No response';
 $('main').classList.toggle('offline',!ok);
}
async function refresh(){
 try{const j=await (await fetch('/status')).json();fails=0;render(j);}
 catch(e){fails++;if(fails>2)setEsp(false);}
}
function render(j){
 setEsp(true);
 $('upd').textContent='updated '+new Date().toLocaleTimeString([],{hour:'2-digit',minute:'2-digit',second:'2-digit'});
 $('pct').textContent=j.pct+'%';$('garc').style.strokeDashoffset=G*(1-j.pct/100);
 $('raw').textContent=j.raw;$('thrShow').textContent=j.thr;
 $('soil').textContent=j.raw<50?'Sensor not detected - check wiring':(j.pct<j.thr?'Soil is dry':(j.pct<j.thr+25?'Soil is okay':'Soil is well watered'));
 $('soil').style.color=j.raw<50?'#ff5d6c':(j.pct<j.thr?'#ffb020':'#3ddc97');
 const st=j.state;
 if(st!==prev){maxRem=Math.max(j.rem,1);prev=st;}
 if(j.rem>maxRem)maxRem=j.rem;
 const b=$('badge');
 b.textContent=st==='RUNNING'?'RUNNING':(st==='Countdown'?'STARTING SOON':'OFF');
 b.className='badge'+(st==='RUNNING'?' run':(st==='Countdown'?' cd':''));
 $('pnote').textContent=st==='RUNNING'?'Pump is watering now.':(st==='Countdown'?'Countdown before the pump starts.':'Pump is idle.');
 $('remT').textContent=st==='OFF'?'0':j.rem;
 const ring=$('rarc');ring.style.stroke=st==='Countdown'?'#ffb020':'#3ddc97';
 ring.style.strokeDashoffset=st==='OFF'?R:R*(1-(j.rem/maxRem));
 $('iMode').textContent=j.mode;$('iIp').textContent=j.ip;$('clk').textContent=j.clk||'not set';
 $('iNet').textContent=j.mode==='Hotspot'?'n/a':(j.net?'Yes':'No (local only)');
 const c=$('dCloud');
 if(j.mq===1){c.className='dot ok';$('tCloud').textContent='Cloud connected';}
 else if(j.mq===2){c.className='dot bad';$('tCloud').textContent='Cloud not connected';}
 else{c.className='dot';$('tCloud').textContent='Cloud off';}
 if(!filled){
  $('thr').value=j.thr;$('run').value=j.run;$('dly').value=j.dly;$('auto').checked=j.auto;
  $('son').checked=j.son;$('stime').value=j.stime;$('srun').value=j.srun;$('sdly').value=j.sdly;
  filled=true;sync();
 }
 hist.push({t:Date.now(),v:j.pct});
 const cut=Date.now()-600000;while(hist.length&&hist[0].t<cut)hist.shift();
 drawChart(j.thr);
}
function drawChart(thr){
 const cv=$('cv'),dpr=window.devicePixelRatio||1,w=cv.clientWidth,h=cv.clientHeight;
 cv.width=w*dpr;cv.height=h*dpr;
 const c=cv.getContext('2d');c.scale(dpr,dpr);c.clearRect(0,0,w,h);
 const pad=26;c.strokeStyle='#1d2d52';c.fillStyle='#8da0c8';c.font='11px sans-serif';c.lineWidth=1;
 [0,50,100].forEach(v=>{const y=h-8-(v/100)*(h-16);c.beginPath();c.moveTo(pad,y);c.lineTo(w,y);c.stroke();c.fillText(v+'%',0,y+4);});
 const ty=h-8-(thr/100)*(h-16);
 c.strokeStyle='#ffb02088';c.setLineDash([5,5]);c.beginPath();c.moveTo(pad,ty);c.lineTo(w,ty);c.stroke();c.setLineDash([]);
 if(hist.length<2)return;
 const t0=Date.now()-600000,X=t=>pad+((t-t0)/600000)*(w-pad),Y=v=>h-8-(v/100)*(h-16);
 const g=c.createLinearGradient(0,0,0,h);g.addColorStop(0,'#2bb6ff55');g.addColorStop(1,'#2bb6ff00');
 c.beginPath();c.moveTo(X(hist[0].t),h-8);hist.forEach(p=>c.lineTo(X(p.t),Y(p.v)));c.lineTo(X(hist[hist.length-1].t),h-8);c.closePath();c.fillStyle=g;c.fill();
 c.beginPath();hist.forEach((p,i)=>i?c.lineTo(X(p.t),Y(p.v)):c.moveTo(X(p.t),Y(p.v)));
 c.strokeStyle='#3ddc97';c.lineWidth=2.5;c.lineJoin='round';c.stroke();
}
window.addEventListener('resize',()=>drawChart(+$('thr').value||30));
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
