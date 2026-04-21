#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <mbedtls/md.h>
#include <Update.h>

// ─── USER CONFIGURATION ──────────────────────────────────────────
// Change these before flashing!

#define WEBOS_USERNAME      "admin"
#define WEBOS_PASSWORD      "change-me-now"
#define WEBOS_HOSTNAME      "webos-32"
#define AP_MDNS_HOSTNAME    "connect-webos-32"

// Preferred LAN boot mode (STA): set your home/office Wi-Fi here.
// If NVS already has credentials, those override these defaults.
#define STA_DEFAULT_SSID        ""
#define STA_DEFAULT_PASSWORD    ""
#define STA_CONNECT_TIMEOUT_MS  12000
// Boot behavior: 0 = start AP only and wait for manual connect from dashboard,
// 1 = also try STA on boot when credentials exist.
#define STA_AUTOCONNECT_ON_BOOT 0

// AP fallback (if Wi-Fi connect fails, ESP-32 opens its own hotspot)
#define AP_SSID             "ESP32-WebOS-32"
#define AP_PASSWORD         "webos123"
#define AP_CHANNEL          6
#define AP_HIDDEN           0
#define AP_MAX_CLIENTS      4
#define AP_FALLBACK_ENABLED 1

// Session token secret (change to any random string)
#define TOKEN_SECRET        "replace-with-long-random-token-secret"

// Session timeout in seconds (0 = never)
#define SESSION_TIMEOUT_SEC 3600

// GPIO pins available for UI control
// Remove any pins you don't want exposed
#define GPIO_ALLOWED_PINS   {2, 4, 5, 12, 13, 14, 15, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}
#define GPIO_INPUT_ONLY     {34, 35, 36, 39}  // read-only on ESP32

// PWM config
#define PWM_FREQ_HZ         5000
#define PWM_RESOLUTION_BITS 8

// NVS namespace
#define NVS_NAMESPACE       "micros"
// ─── GLOBALS ─────────────────────────────────────────────────────
AsyncWebServer server(80);
AsyncWebSocket wsTerminal("/ws/terminal");
AsyncWebSocket wsGpio("/ws/gpio");
AsyncWebSocket wsStatus("/ws/status");
Preferences prefs;
DNSServer dnsServer;

// Stored Wi-Fi credentials
String savedSSID = "";
String savedPass = "";
bool   apMode    = false;

// Settings flags (persisted in NVS)
bool nightLight  = false;
bool airplaneMode= false;
bool mdnsStarted = false;
bool dnsCaptiveRunning = false;
String activeMdnsHostname = "";
bool staBootConnectPending = false;
bool apStartedForBootFallback = false;
unsigned long staBootConnectStartMs = 0;

// Uptime
unsigned long bootMillis = 0;

// GPIO state tracking
struct PinState {
  int     pin;
  bool    isOutput;
  bool    value;
  int     pwmDuty;   // 0 = not PWM
  int     pwmChannel;
};

const int OUTPUT_PINS[] = {2,4,5,12,13,14,15,18,19,21,22,23,25,26,27,32,33};
const int INPUT_PINS[]  = {34,35,36,39};
const int OUT_COUNT = sizeof(OUTPUT_PINS)/sizeof(int);
const int IN_COUNT  = sizeof(INPUT_PINS)/sizeof(int);
const int MAX_LEDC_CHANNELS = 16;

PinState pinStates[sizeof(OUTPUT_PINS)/sizeof(int) + sizeof(INPUT_PINS)/sizeof(int)];
int      totalPins = 0;

// Active session token (simple single-session)
String   sessionToken = "";
unsigned long sessionLastSeenMs = 0;

const char INDEX_HTML[] PROGMEM = R"WEBOS(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>WebOS-32</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}

:root{
  --bg0:#0d1117;
  --bg1:#161b22;
  --bg2:#1f2630;
  --bg3:#30363d;
  --tx0:#e6edf3;
  --tx1:#9aa5b1;
  --tx2:#7d8690;
  --green:#3fb950;
  --blue:#58a6ff;
  --warn:#d29922;
  --red:#f85149;
  --radius:10px;
  --font:'Segoe UI',Tahoma,Arial,sans-serif;
  --mono:'Consolas','Courier New',monospace;
}

html,body{height:100%;width:100%}

body{
  font-family:var(--font);
  background:var(--bg0);
  color:var(--tx0);
  font-size:14px;
  line-height:1.45;
  min-height:100vh;
  overflow:hidden;
}

body.nightlight::after{
  content:'';
  position:fixed;
  inset:0;
  background:rgba(255,140,0,0.08);
  pointer-events:none;
  z-index:9999;
}

/* auth */
#auth{
  display:flex;
  align-items:center;
  justify-content:center;
  min-height:100vh;
  padding:20px;
}

.auth-box{
  background:var(--bg1);
  border:1px solid var(--bg3);
  border-radius:14px;
  padding:34px 30px;
  width:min(380px,100%);
  box-shadow:0 14px 40px rgba(0,0,0,0.3);
}

.auth-logo{
  color:var(--blue);
  font-size:22px;
  font-weight:700;
  text-align:center;
  margin-bottom:4px;
  letter-spacing:-0.02em;
}

.auth-sub{
  color:var(--tx1);
  font-size:12px;
  text-align:center;
  margin-bottom:24px;
}

.form-group{margin-bottom:14px}

.form-label{
  display:block;
  color:var(--tx1);
  font-size:12px;
  margin-bottom:6px;
  font-family:var(--mono);
}

.input{
  width:100%;
  background:var(--bg2);
  border:1px solid var(--bg3);
  border-radius:8px;
  padding:10px 12px;
  color:var(--tx0);
  font-size:13px;
  font-family:var(--mono);
  outline:none;
  transition:border-color .12s, box-shadow .12s;
}

.input:focus{
  border-color:var(--blue);
  box-shadow:0 0 0 3px rgba(88,166,255,0.15);
}

.btn{
  background:var(--bg2);
  border:1px solid var(--bg3);
  color:var(--tx0);
  font-size:13px;
  padding:8px 14px;
  border-radius:8px;
  cursor:pointer;
  font-family:var(--font);
  line-height:1.2;
  transition:all .12s;
}

.btn:hover{background:#2a3441;border-color:#4b5563}
.btn:disabled{opacity:.55;cursor:not-allowed}

.btn-primary{background:#1f6feb;border-color:#1f6feb;color:#f7fbff}
.btn-primary:hover{background:#3c8bff;border-color:#3c8bff}

.btn-danger{background:var(--bg2);border-color:#7e2323;color:#ff9f9f}
.btn-danger:hover{background:#30131a;border-color:#9d2b2b}

.btn-sm{font-size:12px;padding:7px 12px}
.btn-full{width:100%}

.auth-err{
  color:var(--red);
  font-size:12px;
  text-align:center;
  margin-top:10px;
  display:none;
}

/* shell */
#shell{
  display:none;
  grid-template-rows:auto auto 1fr;
  width:100%;
  height:100vh;
}

.topbar{
  background:var(--bg1);
  height:52px;
  display:flex;
  align-items:center;
  justify-content:space-between;
  padding:0 18px;
  border-bottom:1px solid var(--bg3);
  flex-shrink:0;
}

.tb-left{display:flex;align-items:center;gap:14px}

.tb-logo{
  color:var(--blue);
  font-size:14px;
  font-weight:700;
  letter-spacing:0.04em;
  font-family:var(--mono);
}

.tb-time{color:var(--tx1);font-size:12px;font-family:var(--mono)}

.tb-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap;justify-content:flex-end}

.chip{
  display:flex;
  align-items:center;
  gap:6px;
  font-size:12px;
  padding:5px 10px;
  border-radius:999px;
  border:1px solid var(--bg3);
  cursor:pointer;
  transition:all .12s;
  font-family:var(--mono);
  background:rgba(255,255,255,0.01);
}

.chip:hover{background:var(--bg2)}

.dot{width:7px;height:7px;border-radius:50%}
.dot-g{background:var(--green)}
.dot-b{background:var(--blue)}
.dot-w{background:var(--warn)}
.dot-r{background:var(--red)}
.chip-green{color:var(--green)}
.chip-blue{color:var(--blue)}
.chip-warn{color:var(--warn)}
.chip-red{color:var(--red)}

.nav{
  background:var(--bg1);
  border-bottom:1px solid var(--bg3);
  display:flex;
  align-items:flex-end;
  gap:4px;
  padding:0 16px;
  min-height:44px;
}

.nav-item{
  font-size:13px;
  color:var(--tx1);
  padding:11px 14px 10px;
  border-radius:8px 8px 0 0;
  cursor:pointer;
  border-bottom:2px solid transparent;
  transition:all .12s;
  text-transform:capitalize;
}

.nav-item:hover{color:var(--tx0);background:var(--bg2)}
.nav-item.active{color:var(--blue);border-bottom-color:var(--blue);background:#1c2532}

.content{
  min-height:0;
  overflow:hidden;
  padding:16px 18px 18px;
}

.panel{
  display:none;
  height:100%;
  overflow:auto;
  align-content:start;
  gap:14px;
}

.panel.active{display:grid;grid-auto-rows:min-content}

.row{
  display:grid;
  grid-template-columns:repeat(12,minmax(0,1fr));
  gap:14px;
  align-items:stretch;
}

.span-12{grid-column:span 12}
.span-8{grid-column:span 8}
.span-6{grid-column:span 6}
.span-4{grid-column:span 4}
.span-3{grid-column:span 3}

.card,
.metric{
  background:var(--bg1);
  border:1px solid var(--bg3);
  border-radius:var(--radius);
  padding:16px;
  min-height:220px;
  display:flex;
  flex-direction:column;
  gap:10px;
}

.metric{min-height:170px}

.card-title{
  color:var(--tx1);
  font-size:11px;
  text-transform:uppercase;
  letter-spacing:.08em;
  margin-bottom:2px;
  font-family:var(--mono);
}

.metric-label{
  color:var(--tx1);
  font-size:11px;
  font-family:var(--mono);
}

.metric-val{font-size:34px;font-weight:650;font-family:var(--mono);line-height:1.15}
.metric-sub{color:var(--tx1);font-size:12px;font-family:var(--mono)}
.metric-unit{font-size:14px;color:var(--tx1)}

.bar-wrap{
  background:var(--bg2);
  border-radius:999px;
  height:6px;
  margin-top:auto;
  overflow:hidden;
}

.bar{height:100%;border-radius:999px;transition:width .35s}
.bar-blue{background:var(--blue);width:0%}
.bar-warn{background:var(--warn);width:0%}
.bar-green{background:var(--green);width:0%}
.is-hidden{display:none}

.card-head{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}
.card-head.wrap{flex-wrap:wrap}
.card-head .card-title{margin:0}

.hint{color:var(--tx1);font-size:12px;font-family:var(--mono)}
.hint-error{color:var(--red)}

.action-row{display:flex;gap:8px;margin-top:4px;flex-wrap:wrap}
.msg-line{margin-top:10px;font-size:12px;font-family:var(--mono)}
.msg-error{color:var(--red)}

.term-head{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
.term-head-actions{display:flex;align-items:center;gap:8px;flex-wrap:wrap}

.terminal-chip{
  appearance:none;
  background:#1a2533;
  border:1px solid #2f3f55;
  color:#9cc8ff;
  border-radius:999px;
  font-size:11px;
  padding:5px 10px;
  font-family:var(--mono);
  cursor:pointer;
  transition:all .12s;
}

.terminal-chip:hover{background:#233349;border-color:#4a6787;color:#d6eaff}

.terminal-actions{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.terminal-status-live{box-shadow:0 0 0 1px rgba(63,185,80,.35),0 0 14px rgba(63,185,80,.2)}
.terminal-status-offline{box-shadow:0 0 0 1px rgba(248,81,73,.35),0 0 14px rgba(248,81,73,.2)}

.controls-row{grid-template-columns:repeat(auto-fit,minmax(12rem,1fr));align-items:end}
.controls-row .input{width:100%}
.controls-actions{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.input-compact{max-width:8rem;text-align:center}

.srow{
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:12px;
  padding:10px 0;
  border-bottom:1px solid var(--bg2);
}

.srow:last-child{border-bottom:none}
.srow-info{display:flex;flex-direction:column;gap:3px}
.srow-main{font-size:14px;color:var(--tx0)}
.srow-sub{font-size:11px;color:var(--tx1);font-family:var(--mono)}

.toggle-wrap{
  width:40px;
  height:22px;
  border-radius:999px;
  cursor:pointer;
  position:relative;
  transition:background .18s;
  flex-shrink:0;
}

.toggle-wrap.on{background:#238636}
.toggle-wrap.off{background:#4b5563}

.toggle-wrap::after{
  content:'';
  position:absolute;
  top:2px;
  left:2px;
  width:18px;
  height:18px;
  border-radius:50%;
  background:#e6edf3;
  transition:transform .18s;
}

.toggle-wrap.on::after{transform:translateX(18px)}

.badge{
  font-size:11px;
  padding:3px 8px;
  border-radius:999px;
  font-family:var(--mono);
  font-weight:600;
}

.badge-g{background:#0d2418;color:var(--green);border:1px solid #238636}
.badge-b{background:#0d1f38;color:var(--blue);border:1px solid #1f6feb}
.badge-w{background:#1c1207;color:var(--warn);border:1px solid #9e6a03}
.badge-r{background:#2d1117;color:var(--red);border:1px solid #6e2020}
.badge-gray{background:var(--bg2);color:var(--tx1);border:1px solid var(--bg3)}

.sig{display:flex;gap:2px;align-items:flex-end}
.sig span{width:3px;border-radius:2px;background:var(--bg3)}
.sig span.lit{background:var(--green)}
.sig .h1{height:5px}
.sig .h2{height:8px}
.sig .h3{height:11px}
.sig .h4{height:14px}

.net-name{font-size:13px}
.net-meta{color:var(--tx1);font-size:11px;font-family:var(--mono);margin-top:2px}

.net-item{
  display:flex;
  align-items:center;
  justify-content:space-between;
  gap:10px;
  padding:10px 12px;
  border-radius:8px;
  cursor:pointer;
  border:1px solid transparent;
  transition:all .12s;
  margin-bottom:6px;
}

.net-item:hover{background:var(--bg2)}
.net-item.connected{border-color:var(--green);background:#0d2418}
.net-item.paired{border-color:var(--blue);background:#0d1f38}

.gpio-grid{
  display:grid;
  grid-template-columns:repeat(auto-fill,minmax(88px,1fr));
  gap:8px;
}

.pin{
  background:var(--bg2);
  border:1px solid var(--bg3);
  border-radius:8px;
  padding:10px 6px;
  text-align:center;
  cursor:pointer;
  transition:all .12s;
  user-select:none;
}

.pin:hover{border-color:var(--blue)}
.pin.sel{box-shadow:0 0 0 2px rgba(88,166,255,.45) inset;border-color:#6cb6ff}
.pin.high{background:#0d2418;border-color:var(--green)}
.pin.pwm{background:#0d1f38;border-color:var(--blue)}
.pin.inp{background:#1c1207;border-color:var(--warn)}
.pin-num{font-size:13px;font-weight:700;font-family:var(--mono)}
.pin-mode{font-size:10px;color:var(--tx1);margin-top:2px;font-family:var(--mono)}
.pin-state{font-size:10px;margin-top:2px;font-family:var(--mono)}
.pin-state-high{color:var(--green)}
.pin-state-pwm{color:var(--blue)}
.pin-state-inp{color:var(--warn)}
.pin-state-low{color:var(--tx1)}

.term{
  background:linear-gradient(180deg,#0a0f18 0%,#0d1117 70%);
  border:1px solid #314057;
  border-radius:var(--radius);
  overflow:hidden;
  font-family:var(--mono);
  font-size:13px;
  display:flex;
  flex-direction:column;
  min-height:320px;
  height:100%;
  box-shadow:0 14px 40px rgba(0,0,0,.28), inset 0 1px 0 rgba(255,255,255,.03);
}

.term-bar{
  background:linear-gradient(90deg,#141d2b 0%,#101824 60%,#0e1620 100%);
  padding:10px 14px;
  display:flex;
  align-items:center;
  gap:8px;
  border-bottom:1px solid #2c3a4f;
}

.term-dot{width:10px;height:10px;border-radius:50%}
.term-dot-red{background:var(--red)}
.term-dot-warn{background:var(--warn)}
.term-dot-green{background:var(--green)}
.term-caption{color:var(--tx1);font-size:12px;margin-left:6px}
.ml-auto{margin-left:auto}

.term-body{
  padding:12px 14px;
  flex:1;
  min-height:220px;
  overflow-y:auto;
  line-height:1.75;
  position:relative;
  background:
    linear-gradient(180deg,rgba(16,22,34,.65) 0%,rgba(12,17,27,.88) 100%),
    radial-gradient(circle at 20% -20%,rgba(88,166,255,.15),rgba(88,166,255,0) 40%);
}

.term-body::before{
  content:'';
  position:absolute;
  inset:0;
  pointer-events:none;
  background:repeating-linear-gradient(
    180deg,
    rgba(255,255,255,.02) 0,
    rgba(255,255,255,.02) 1px,
    transparent 1px,
    transparent 3px
  );
  opacity:.25;
}

.term-body > *{position:relative;z-index:1}

.term-compact{border:none;border-radius:6px;min-height:0}
.term-body-compact{min-height:180px}
.term-body-tall{min-height:360px}

.t-line{color:var(--tx1)}
.t-ok{color:var(--green)}
.t-err{color:var(--red)}
.t-info{color:var(--blue)}
.t-warn{color:var(--warn)}
.t-cmd{color:var(--tx0)}

.term-in-row{
  display:flex;
  align-items:center;
  padding:10px 14px;
  border-top:1px solid var(--bg2);
  gap:8px;
}

.term-prompt{color:var(--green);flex-shrink:0}

.term-input{
  background:transparent;
  border:none;
  outline:none;
  color:var(--tx0);
  font-size:13px;
  font-family:var(--mono);
  flex:1;
}

.modal-bg{
  display:none;
  position:fixed;
  inset:0;
  background:rgba(0,0,0,.65);
  z-index:200;
  align-items:center;
  justify-content:center;
  padding:18px;
}

.modal-bg.open{display:flex}

.modal{
  background:var(--bg1);
  border:1px solid var(--bg3);
  border-radius:12px;
  padding:22px 20px;
  width:min(420px,100%);
}

.modal-title{font-size:16px;font-weight:600;margin-bottom:14px}
.input-spaced{margin-bottom:12px}
.modal-actions{display:flex;gap:8px;justify-content:flex-end}

/* panel-specific fill rules */
#panel-dashboard{grid-template-rows:auto minmax(320px,1fr)}
#panel-dashboard .row:last-child{min-height:320px}
#panel-dashboard .row:last-child .card{min-height:320px}

#panel-wifi,#panel-bt{grid-template-rows:minmax(360px,1fr)}
#panel-wifi .card,#panel-bt .card{min-height:360px}
#panel-wifi #wifi-list,#panel-bt #bt-list{flex:1;overflow:auto;padding-right:4px}

#panel-gpio{grid-template-rows:minmax(320px,1fr) auto}
#panel-gpio > .card:first-child{min-height:320px}

#panel-terminal{grid-template-rows:1fr}
#panel-terminal .card{min-height:0;height:100%}
#panel-terminal .term{min-height:0;height:100%}

#panel-settings{grid-template-rows:auto auto}
#panel-settings .row .card{min-height:300px}

@media(max-width:1200px){
  .span-8,.span-6,.span-4,.span-3{grid-column:span 6}
  .content{padding:14px}
}

@media(max-width:900px){
  body{overflow:auto}
  #shell{height:auto;min-height:100vh}
  .topbar{height:auto;min-height:52px;padding:10px 12px;align-items:flex-start;gap:8px}
  .tb-right{gap:6px}
  .nav{padding:0 10px;overflow:auto}
  .content{overflow:visible;padding:10px}
  .panel{height:auto;overflow:visible}
  .row{grid-template-columns:repeat(1,minmax(0,1fr));gap:10px}
  .span-12,.span-8,.span-6,.span-4,.span-3{grid-column:span 1}
  .metric-val{font-size:30px}
  .card,.metric{min-height:0}
  #panel-dashboard,#panel-wifi,#panel-bt,#panel-gpio,#panel-terminal,#panel-settings{grid-template-rows:auto}
}
</style>
</head>
<body>

<!-- ── AUTH SCREEN ── -->
<div id="auth">
  <div class="auth-box">
    <div class="auth-logo">WebOS-32</div>
    <div class="auth-sub">v1.0.0 · secure access</div>
    <div class="form-group">
      <label class="form-label">username</label>
      <input class="input" id="u" type="text" autocomplete="username" value="">
    </div>
    <div class="form-group">
      <label class="form-label">password</label>
      <input class="input" id="p" type="password" autocomplete="current-password">
    </div>
    <button class="btn btn-primary btn-full" onclick="doLogin()">sign in</button>
    <div class="auth-err" id="auth-err">invalid credentials</div>
  </div>
</div>

<!-- ── SHELL ── -->
<div id="shell">
  <!-- topbar -->
  <div class="topbar">
    <div class="tb-left">
      <span class="tb-logo">WebOS-32</span>
      <span class="tb-time" id="clock">--:--</span>
    </div>
    <div class="tb-right">
      <div class="chip chip-green" id="chip-wifi" onclick="nav('wifi')">
        <span class="dot dot-g"></span><span id="chip-wifi-lbl">Wi-Fi</span>
      </div>
      <div class="chip chip-blue" id="chip-bt" onclick="nav('bt')">
        <span class="dot dot-b"></span>BT
      </div>
      <div class="chip chip-warn" id="chip-flash">
        <span class="dot dot-w"></span><span id="chip-flash-lbl">Flash</span>
      </div>
      <div class="chip chip-blue" id="chip-ram">
        <span class="dot dot-b"></span><span id="chip-ram-lbl">RAM</span>
      </div>
      <div class="chip" onclick="nav('settings')">&#9881;</div>
    </div>
  </div>

  <!-- nav -->
  <div class="nav">
    <div class="nav-item active" onclick="nav('dashboard')">dashboard</div>
    <div class="nav-item" onclick="nav('wifi')">wi-fi</div>
    <div class="nav-item" onclick="nav('bt')">bluetooth</div>
    <div class="nav-item" onclick="nav('gpio')">gpio</div>
    <div class="nav-item" onclick="nav('terminal')">terminal</div>
    <div class="nav-item" onclick="nav('settings')">settings</div>
  </div>

  <div class="content">

    <!-- ── DASHBOARD ── -->
    <div class="panel active" id="panel-dashboard">
      <div class="row kpi-grid">
        <div class="metric span-3">
          <div class="metric-label">RAM usage</div>
          <div class="metric-val" id="d-ram">--<span class="metric-unit">%</span></div>
          <div class="metric-sub" id="d-ram-sub">-- KB free</div>
          <div class="bar-wrap"><div class="bar bar-blue" id="d-ram-bar"></div></div>
        </div>
        <div class="metric span-3">
          <div class="metric-label">Flash usage</div>
          <div class="metric-val" id="d-flash">--<span class="metric-unit">%</span></div>
          <div class="metric-sub" id="d-flash-sub">-- KB used</div>
          <div class="bar-wrap"><div class="bar bar-warn" id="d-flash-bar"></div></div>
        </div>
        <div class="metric span-3">
          <div class="metric-label">CPU temperature</div>
          <div class="metric-val" id="d-temp">--<span class="metric-unit">°C</span></div>
          <div class="metric-sub" id="d-temp-sub">nominal</div>
          <div class="bar-wrap"><div class="bar bar-green" id="d-temp-bar"></div></div>
        </div>
        <div class="metric span-3">
          <div class="metric-label">Uptime</div>
          <div class="metric-val" id="d-uptime">--:--:--</div>
          <div class="metric-sub" id="d-mode">loading...</div>
          <div class="bar-wrap is-hidden"></div>
        </div>
      </div>
      <div class="row">
        <div class="card span-4">
          <div class="card-title">system</div>
          <div class="srow"><span class="srow-main">Wi-Fi</span><span class="badge badge-g" id="d-wifi-badge">--</span></div>
          <div class="srow"><span class="srow-main">IP address</span><span class="badge badge-b" id="d-ip">--</span></div>
          <div class="srow"><span class="srow-main">RSSI</span><span class="badge badge-gray" id="d-rssi">-- dBm</span></div>
          <div class="srow"><span class="srow-main">Night light</span><span class="badge" id="d-nl">off</span></div>
          <div class="srow"><span class="srow-main">Airplane mode</span><span class="badge" id="d-ap">off</span></div>
        </div>
        <div class="card span-8">
          <div class="term-head">
            <div class="card-title">dashboard terminal</div>
            <div class="term-head-actions">
              <span class="badge badge-gray" id="qt-status">standby</span>
              <button class="terminal-chip" onclick="openTerminalPanel()">open full terminal</button>
              <button class="terminal-chip" onclick="clearTerminals()">clear</button>
            </div>
          </div>
          <div class="term term-compact">
            <div class="term-body term-body-compact" id="qt-body">
              <div class="t-line t-info">[boot] WebOS-32 connecting...</div>
            </div>
            <div class="term-in-row">
              <span class="term-prompt">dash$</span>
              <input class="term-input" id="qt-in" placeholder="type a command..." onkeydown="qtKey(event)">
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- ── WIFI ── -->
    <div class="panel" id="panel-wifi">
      <div class="row">
        <div class="card span-6">
          <div class="card-head">
            <div class="card-title">available networks</div>
            <button class="btn btn-sm" onclick="scanWifi()">scan</button>
          </div>
          <div id="wifi-list"><div class="hint">tap scan to discover networks</div></div>
        </div>
        <div class="card span-6">
          <div class="card-title">join network</div>
          <div class="form-group">
            <label class="form-label">SSID</label>
            <input class="input" id="wifi-ssid" type="text" placeholder="network name">
          </div>
          <div class="form-group">
            <label class="form-label">password</label>
            <input class="input" id="wifi-pass" type="password" placeholder="leave empty if open">
          </div>
          <div class="action-row">
            <button class="btn btn-primary btn-sm" onclick="connectWifi()">connect</button>
            <button class="btn btn-danger btn-sm" onclick="disconnectWifi()">disconnect</button>
          </div>
          <div class="msg-line" id="wifi-msg"></div>
        </div>
      </div>
    </div>

    <!-- ── BLUETOOTH ── -->
    <div class="panel" id="panel-bt">
      <div class="row">
        <div class="card span-6">
          <div class="card-head">
            <div class="card-title">devices</div>
            <button class="btn btn-sm" onclick="scanBT()">scan</button>
          </div>
          <div id="bt-list"><div class="hint">tap scan to find nearby demo devices</div></div>
        </div>
        <div class="card span-6">
          <div class="card-title">bluetooth settings</div>
          <div class="srow">
            <div class="srow-info"><span class="srow-main">Device name</span><span class="srow-sub" id="bt-name">ESP32-WebOS-32</span></div>
          </div>
          <div class="srow">
            <div class="srow-info"><span class="srow-main">Status</span><span class="srow-sub">scan list is UI demo (no hardware pairing yet)</span></div>
            <span class="badge badge-w">demo</span>
          </div>
        </div>
      </div>
    </div>

    <!-- ── GPIO ── -->
    <div class="panel" id="panel-gpio">
      <div class="card">
        <div class="card-head wrap">
          <div class="card-title">pin map — click OUTPUT pins to toggle</div>
          <div class="action-row">
            <span class="badge badge-g">HIGH</span>
            <span class="badge badge-b">PWM</span>
            <span class="badge badge-w">INPUT</span>
            <span class="badge badge-gray">LOW</span>
          </div>
        </div>
        <div class="gpio-grid" id="gpio-grid"></div>
      </div>
      <div class="card">
        <div class="card-title">pin control</div>
        <div class="row controls-row">
          <div>
            <label class="form-label">pin number</label>
            <input class="input" id="ctrl-pin" type="number" min="2" max="39" placeholder="e.g. 2">
          </div>
          <div>
            <label class="form-label">mode</label>
            <select class="input" id="ctrl-mode">
              <option>OUTPUT</option><option>INPUT</option>
            </select>
          </div>
          <div>
            <label class="form-label">PWM duty (0-255)</label>
            <input class="input" id="ctrl-pwm" type="number" min="0" max="255" placeholder="0 = digital">
          </div>
          <div class="controls-actions">
            <button class="btn btn-primary btn-sm" onclick="pinWrite(1)">set HIGH</button>
            <button class="btn btn-sm" onclick="pinWrite(0)">set LOW</button>
            <button class="btn btn-sm" onclick="pinPWM()">set PWM</button>
            <button class="btn btn-sm" onclick="pinMode()">set mode</button>
          </div>
        </div>
        <div class="msg-line" id="gpio-msg"></div>
      </div>
    </div>

    <!-- ── TERMINAL ── -->
    <div class="panel" id="panel-terminal">
      <div class="card">
        <div class="term-head">
          <div class="card-title">gpio terminal — full command interface</div>
          <div class="term-head-actions">
            <span class="badge badge-gray" id="term-route">route: full</span>
            <button class="terminal-chip" onclick="clearTerminals()">clear logs</button>
          </div>
        </div>
        <div class="terminal-actions">
          <button class="terminal-chip" onclick="runTerminalShortcut('help')">help</button>
          <button class="terminal-chip" onclick="runTerminalShortcut('gpio list')">gpio list</button>
          <button class="terminal-chip" onclick="runTerminalShortcut('sys temp')">sys temp</button>
          <button class="terminal-chip" onclick="runTerminalShortcut('wifi status')">wifi status</button>
        </div>
        <div class="term">
          <div class="term-bar">
            <div class="term-dot term-dot-red"></div>
            <div class="term-dot term-dot-warn"></div>
            <div class="term-dot term-dot-green"></div>
            <span class="term-caption">ESP-32 shell</span>
            <span class="badge badge-g ml-auto" id="ws-status">connecting...</span>
          </div>
          <div class="term-body term-body-tall" id="term-body">
            <div class="t-line t-info">[info] Connecting to ESP-32...</div>
            <div class="t-line t-info">[info] Type 'help' for available commands</div>
          </div>
          <div class="term-in-row">
            <span class="term-prompt">gpio$</span>
            <input class="term-input" id="term-in" placeholder="gpio write 2 high" onkeydown="termKey(event)" autocomplete="off" autocorrect="off" spellcheck="false">
          </div>
        </div>
      </div>
    </div>

    <!-- ── SETTINGS ── -->
    <div class="panel" id="panel-settings">
      <div class="row">
        <div class="card span-6">
          <div class="card-title">display</div>
          <div class="srow">
            <div class="srow-info">
              <span class="srow-main">Night light</span>
              <span class="srow-sub">warm overlay, reduces eye strain</span>
            </div>
            <div class="toggle-wrap off" id="tog-nl" onclick="togNightlight()"></div>
          </div>
          <div class="srow">
            <div class="srow-info">
              <span class="srow-main">Airplane mode</span>
              <span class="srow-sub">disables Wi-Fi and Bluetooth</span>
            </div>
            <div class="toggle-wrap off" id="tog-ap" onclick="togAirplane()"></div>
          </div>
        </div>
        <div class="card span-6">
          <div class="card-title">system</div>
          <div class="srow">
            <div class="srow-info"><span class="srow-main">OTA update</span><span class="srow-sub">flash new firmware over Wi-Fi</span></div>
            <button class="btn btn-primary btn-sm" onclick="openOTA()">update</button>
          </div>
          <div class="srow">
            <div class="srow-info"><span class="srow-main">Reboot</span><span class="srow-sub">soft restart ESP-32</span></div>
            <button class="btn btn-sm" onclick="doReboot()">reboot</button>
          </div>
          <div class="srow">
            <div class="srow-info"><span class="srow-main">Sign out</span><span class="srow-sub">clear session</span></div>
            <button class="btn btn-danger btn-sm" onclick="doLogout()">logout</button>
          </div>
        </div>
      </div>
      <div class="card span-12">
        <div class="card-title">about</div>
        <div class="srow"><span class="srow-main">Firmware</span><span class="badge badge-b">WebOS-32 v1.0.0</span></div>
        <div class="srow"><span class="srow-main">Framework</span><span class="badge badge-gray">ESP-IDF v5.x / Arduino</span></div>
        <div class="srow"><span class="srow-main">Chip</span><span class="badge badge-gray">ESP32</span></div>
        <div class="srow"><span class="srow-main">Flash size</span><span class="badge badge-gray">4 MB</span></div>
        <div class="srow"><span class="srow-main">Build</span><span class="badge badge-gray">2026-04-20</span></div>
      </div>
    </div>

  </div><!-- /content -->
</div><!-- /shell -->

<!-- ── OTA MODAL ── -->
<div class="modal-bg" id="modal-ota">
  <div class="modal">
    <div class="modal-title">OTA firmware update</div>
    <input type="file" id="ota-file" accept=".bin" class="input input-spaced">
    <div class="modal-actions">
      <button class="btn btn-sm" onclick="closeModal('modal-ota')">cancel</button>
      <button class="btn btn-primary btn-sm" onclick="doOTA()">flash now</button>
    </div>
    <div class="msg-line" id="ota-msg"></div>
  </div>
</div>

<script>
// ── STATE ────────────────────────────────────────────────────────
let token = '';
let ws = null;
let wsGpio = null;
let wsStatus = null;
let statusInterval = null;
let clockInterval = null;
let shellStarted = false;
let cmdHistory = [];
let histIdx = -1;
let activePanel = 'dashboard';
let selectedPin = null;
let nightlightOn = false;
let airplaneOn = false;
const FULL_TERMINAL_MAX_LINES = 550;
const QUICK_TERMINAL_MAX_LINES = 140;
const TOKEN_STORAGE_KEY = 'webos-32.token';
const ENABLE_DEMO_MODE = false;
const DEFAULT_API_TIMEOUT_MS = 8000;

async function fetchWithTimeout(path, options = {}, timeoutMs = DEFAULT_API_TIMEOUT_MS) {
  const ctrl = new AbortController();
  const timer = setTimeout(() => ctrl.abort(), timeoutMs);
  try {
    return await fetch(path, {...options, signal: ctrl.signal});
  } finally {
    clearTimeout(timer);
  }
}

function saveToken(nextToken) {
  token = nextToken || '';
  if (token) localStorage.setItem(TOKEN_STORAGE_KEY, token);
  else localStorage.removeItem(TOKEN_STORAGE_KEY);
}

function loadToken() {
  const cached = localStorage.getItem(TOKEN_STORAGE_KEY);
  token = cached || '';
  return token;
}

function showAuth(message = '') {
  document.getElementById('shell').style.display = 'none';
  document.getElementById('auth').style.display = 'flex';
  const err = document.getElementById('auth-err');
  if (message) {
    err.textContent = message;
    err.style.display = 'block';
  } else {
    err.style.display = 'none';
  }
}

function showShell() {
  document.getElementById('auth').style.display = 'none';
  document.getElementById('shell').style.display = 'grid';
  document.getElementById('auth-err').style.display = 'none';
}

function stopShellRuntime() {
  if (clockInterval) {
    clearInterval(clockInterval);
    clockInterval = null;
  }
  if (statusInterval) {
    clearInterval(statusInterval);
    statusInterval = null;
  }
  if (ws) {
    try { ws.close(); } catch (e) {}
    ws = null;
  }
  if (wsGpio) {
    try { wsGpio.close(); } catch (e) {}
    wsGpio = null;
  }
  if (wsStatus) {
    try { wsStatus.close(); } catch (e) {}
    wsStatus = null;
  }
  shellStarted = false;
}

function handleUnauthorized(message = 'Session expired. Please login again.') {
  saveToken('');
  stopShellRuntime();
  showAuth(message);
}

async function apiFetch(path, options = {}) {
  const headers = new Headers(options.headers || {});
  if (token) headers.set('X-Token', token);
  const response = await fetchWithTimeout(path, {...options, headers}, options.timeoutMs || DEFAULT_API_TIMEOUT_MS);

  if (response.status === 401) {
    handleUnauthorized();
    throw new Error('unauthorized');
  }
  return response;
}

async function restoreSession() {
  if (!loadToken()) {
    showAuth();
    return;
  }

  try {
    const r = await apiFetch('/api/status');
    if (!r.ok) throw new Error('status_check_failed');
    const d = await r.json();
    showShell();
    startShell();
    updateDashboard(d);
  } catch (e) {
    if (e.message === 'unauthorized') return;
    showAuth('Unable to restore session. Please sign in.');
  }
}

// ── AUTH ─────────────────────────────────────────────────────────
async function doLogin() {
  const loginBtn = document.querySelector('#auth .btn-primary');
  const u = document.getElementById('u').value.trim();
  const p = document.getElementById('p').value;
  if (loginBtn) loginBtn.disabled = true;
  try {
    const r = await fetchWithTimeout('/api/auth', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({username:u, password:p})
    }, 10000);
    if (!r.ok) {
      showAuth('Invalid credentials');
      return;
    }
    const d = await r.json();
    if (d.ok) {
      saveToken(d.token || '');
      showShell();
      startShell();
    } else {
      showAuth('Invalid credentials');
    }
  } catch(e) {
    if (ENABLE_DEMO_MODE) {
      saveToken('demo');
      showShell();
      startShell();
      return;
    }
    showAuth('Unable to reach device. Check connection and try again.');
  } finally {
    if (loginBtn) loginBtn.disabled = false;
  }
}
document.getElementById('p').addEventListener('keydown', e => { if(e.key==='Enter') doLogin(); });

function doLogout() {
  fetch('/api/logout', {method:'POST', headers:{'X-Token':token}}).catch(()=>{});
  saveToken('');
  stopShellRuntime();
  showAuth();
}

// ── SHELL START ──────────────────────────────────────────────────
function startShell() {
  if (shellStarted) return;
  shellStarted = true;
  setTerminalStatus(false);
  setRouteLabel('terminal');
  startClock();
  renderGPIOGrid();
  fetchStatus();
  // Fallback polling only; realtime updates come from ws/status.
  statusInterval = setInterval(fetchStatus, 15000);
  connectStatusWS();
  connectTerminalWS();
  connectGpioWS();
}

// ── CLOCK ────────────────────────────────────────────────────────
function startClock() {
  if (clockInterval) {
    clearInterval(clockInterval);
    clockInterval = null;
  }
  function tick() {
    const n = new Date();
    let h = n.getHours(), m = n.getMinutes().toString().padStart(2,'0');
    const am = h < 12 ? 'AM' : 'PM';
    h = h % 12 || 12;
    document.getElementById('clock').textContent = h + ':' + m + ' ' + am;
  }
  tick();
  clockInterval = setInterval(tick, 15000);
}

// ── STATUS POLLING ───────────────────────────────────────────────
async function fetchStatus() {
  try {
    const r = await apiFetch('/api/status');
    if (!r.ok) return;
    const d = await r.json();
    updateDashboard(d);
  } catch(e) {
    if (ENABLE_DEMO_MODE && token === 'demo') {
      updateDashboard({
        free_heap_kb: 136, total_heap_kb: 320,
        flash_used_kb: 2764, flash_total_kb: 4096,
        cpu_temp_c: 47, uptime_sec: Math.floor((Date.now()/1000)%86400),
        wifi_connected: true, wifi_ssid: 'HomeNet_5G',
        wifi_ip: '192.168.1.42', wifi_rssi: -42,
        nightlight: nightlightOn, airplane_mode: airplaneOn,
        ap_mode: false
      });
    }
  }
}

function updateDashboard(d) {
  // RAM
  const ramPct = Math.round((1 - d.free_heap_kb / d.total_heap_kb) * 100);
  document.getElementById('d-ram').innerHTML = ramPct + '<span class="metric-unit">%</span>';
  document.getElementById('d-ram-sub').textContent = d.free_heap_kb + ' KB free / ' + d.total_heap_kb + ' KB total';
  document.getElementById('d-ram-bar').style.width = ramPct + '%';
  // topbar RAM chip
  document.getElementById('chip-ram-lbl').textContent = 'RAM ' + ramPct + '%';

  // Flash
  const flashPct = Math.round(d.flash_used_kb / d.flash_total_kb * 100);
  document.getElementById('d-flash').innerHTML = flashPct + '<span class="metric-unit">%</span>';
  document.getElementById('d-flash-sub').textContent = d.flash_used_kb + ' KB / ' + d.flash_total_kb + ' KB';
  document.getElementById('d-flash-bar').style.width = flashPct + '%';
  document.getElementById('chip-flash-lbl').textContent = 'Flash ' + flashPct + '%';

  // Temp
  const t = d.cpu_temp_c;
  document.getElementById('d-temp').innerHTML = t + '<span class="metric-unit">°C</span>';
  document.getElementById('d-temp-sub').textContent = t < 60 ? 'nominal' : t < 80 ? 'warm' : 'hot!';
  document.getElementById('d-temp-bar').style.width = Math.min(t * 1.2, 100) + '%';
  document.getElementById('d-temp-bar').style.background = t < 60 ? 'var(--green)' : t < 80 ? 'var(--warn)' : 'var(--red)';

  // Uptime
  const s = d.uptime_sec;
  const h = String(Math.floor(s/3600)).padStart(2,'0');
  const m = String(Math.floor((s%3600)/60)).padStart(2,'0');
  const sec = String(s%60).padStart(2,'0');
  document.getElementById('d-uptime').textContent = h+':'+m+':'+sec;
  document.getElementById('d-mode').textContent = d.ap_mode ? 'AP mode' : 'STA mode';

  // Wi-Fi
  const wc = d.wifi_connected;
  const wBadge = document.getElementById('d-wifi-badge');
  wBadge.textContent = wc ? d.wifi_ssid : 'disconnected';
  wBadge.className = 'badge ' + (wc ? 'badge-g' : 'badge-r');
  document.getElementById('d-ip').textContent = wc ? d.wifi_ip : '--';
  document.getElementById('d-rssi').textContent = wc ? d.wifi_rssi + ' dBm' : '--';
  const wChip = document.getElementById('chip-wifi');
  wChip.className = 'chip ' + (wc ? 'chip-green' : 'chip-red');
  wChip.querySelector('.dot').className = 'dot ' + (wc ? 'dot-g' : 'dot-r');
  document.getElementById('chip-wifi-lbl').textContent = wc ? d.wifi_ssid : 'offline';

  // Badges
  const nl = d.nightlight;
  const nlBadge = document.getElementById('d-nl');
  nlBadge.textContent = nl ? 'on' : 'off';
  nlBadge.className = 'badge ' + (nl ? 'badge-w' : 'badge-gray');
  const ap = d.airplane_mode;
  const apBadge = document.getElementById('d-ap');
  apBadge.textContent = ap ? 'on' : 'off';
  apBadge.className = 'badge ' + (ap ? 'badge-b' : 'badge-gray');
}

// ── NAV ──────────────────────────────────────────────────────────
const panels = ['dashboard','wifi','bt','gpio','terminal','settings'];
function nav(id) {
  activePanel = id;
  panels.forEach(p => {
    document.getElementById('panel-'+p).classList.remove('active');
  });
  document.querySelectorAll('.nav-item').forEach((el,i) => {
    el.classList.toggle('active', panels[i] === id);
  });
  document.getElementById('panel-'+id).classList.add('active');
  if (id === 'terminal') setTimeout(focusTermInput, 0);
}

function focusTermInput() {
  const input = document.getElementById('term-in');
  if (input) input.focus();
}

function openTerminalPanel() {
  nav('terminal');
}

function clearTerminals() {
  const full = document.getElementById('term-body');
  const quick = document.getElementById('qt-body');
  if (full) full.innerHTML = '';
  if (quick) quick.innerHTML = '';
}

function setTerminalStatus(isConnected) {
  const wsBadge = document.getElementById('ws-status');
  const qtBadge = document.getElementById('qt-status');
  if (wsBadge) {
    wsBadge.textContent = isConnected ? 'connected' : 'disconnected';
    wsBadge.className = 'badge ' + (isConnected ? 'badge-g terminal-status-live' : 'badge-r terminal-status-offline') + ' ml-auto';
  }
  if (qtBadge) {
    qtBadge.textContent = isConnected ? 'live route' : 'offline route';
    qtBadge.className = 'badge ' + (isConnected ? 'badge-g terminal-status-live' : 'badge-r terminal-status-offline');
  }
}

function setRouteLabel(source) {
  const route = document.getElementById('term-route');
  if (!route) return;
  route.textContent = 'route: ' + (source === 'quick' ? 'dashboard' : 'full');
}

function trimLog(targetId, maxLines) {
  const body = document.getElementById(targetId);
  if (!body) return;
  while (body.children.length > maxLines) {
    body.removeChild(body.firstChild);
  }
}

function runTerminalShortcut(cmd) {
  setRouteLabel('terminal');
  tLog('gpio$ ' + cmd, 't-cmd');
  qtLog('gpio$ ' + cmd, 't-cmd');
  sendCmd(cmd, 'terminal');
}

// ── WEBSOCKET — TERMINAL ─────────────────────────────────────────
function connectTerminalWS() {
  if (!shellStarted) return;
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  try {
    ws = new WebSocket(proto + '//' + location.host + '/ws/terminal?token=' + encodeURIComponent(token));
    ws.onopen = () => {
      setTerminalStatus(true);
      tLog('[ok] Connected to ESP-32 terminal', 't-ok');
      qtLog('[ok] Dashboard terminal route is live', 't-ok');
    };
    ws.onmessage = e => {
      const lines = e.data.split('\n');
      lines.forEach(l => {
        if (!l) return;
        if (l === '__CLEAR__') {
          clearTerminals();
          return;
        }
        const cls = l.startsWith('[ok]') ? 't-ok' :
                    l.startsWith('[err]') ? 't-err' :
                    l.startsWith('[info]') ? 't-info' :
                    l.startsWith('[warn]') ? 't-warn' : 't-line';
        tLog(l, cls);
        qtLog(l, cls);
      });
    };
    ws.onclose = () => {
      setTerminalStatus(false);
      if (!shellStarted) return;
      setTimeout(connectTerminalWS, 3000);
    };
    ws.onerror = () => {};
  } catch(e) {}
}

// ── WEBSOCKET — LIVE STATUS ─────────────────────────────────────
function connectStatusWS() {
  if (!shellStarted) return;
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  try {
    wsStatus = new WebSocket(proto + '//' + location.host + '/ws/status?token=' + encodeURIComponent(token));
    wsStatus.onmessage = e => {
      try {
        const d = JSON.parse(e.data);
        if (d && d.ok) updateDashboard(d);
      } catch (ex) {}
    };
    wsStatus.onclose = () => {
      if (!shellStarted) return;
      setTimeout(connectStatusWS, 3000);
    };
    wsStatus.onerror = () => {};
  } catch(e) {}
}

// ── WEBSOCKET — GPIO LIVE ────────────────────────────────────────
function connectGpioWS() {
  if (!shellStarted) return;
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  try {
    wsGpio = new WebSocket(proto + '//' + location.host + '/ws/gpio?token=' + encodeURIComponent(token));
    wsGpio.onmessage = e => {
      try {
        const d = JSON.parse(e.data);
        if (d.pins) updateGpioGrid(d.pins);
      } catch(ex){}
    };
    wsGpio.onclose = () => {
      if (!shellStarted) return;
      setTimeout(connectGpioWS, 3000);
    };
    wsGpio.onerror = () => {};
  } catch(e) {}
}

// ── TERMINAL ─────────────────────────────────────────────────────
function tLog(txt, cls='t-line', targetId='term-body') {
  const body = document.getElementById(targetId);
  if (!body) return;
  const d = document.createElement('div');
  d.className = cls;
  d.textContent = txt;
  body.appendChild(d);
  trimLog(targetId, targetId === 'qt-body' ? QUICK_TERMINAL_MAX_LINES : FULL_TERMINAL_MAX_LINES);
  body.scrollTop = body.scrollHeight;
}
function qtLog(txt, cls='t-line') { tLog(txt, cls, 'qt-body'); }

function sendCmd(cmd, source='terminal') {
  if (ws && ws.readyState === WebSocket.OPEN) {
    setRouteLabel(source);
    ws.send(cmd);
  } else {
    if (ENABLE_DEMO_MODE && token === 'demo') {
      const resp = demoCmd(cmd);
      resp.split('\n').forEach(l => {
        if (!l) return;
        const cls = l.startsWith('[ok]') ? 't-ok' :
                    l.startsWith('[err]') ? 't-err' :
                    l.startsWith('[info]') ? 't-info' :
                    l.startsWith('[warn]') ? 't-warn' : 't-line';
        tLog(l, cls);
        qtLog(l, cls);
      });
      return;
    }
    tLog('[err] Terminal disconnected. Reopening terminal route...', 't-err');
    qtLog('[err] Terminal disconnected. Reopening terminal route...', 't-err');
    if (source === 'quick') openTerminalPanel();
  }
}

function routeTerminalCommand(cmd, source='terminal') {
  const prompt = source === 'quick' ? 'dash$ ' : 'gpio$ ';
  setRouteLabel(source);
  tLog(prompt + cmd, 't-cmd');
  qtLog(prompt + cmd, 't-cmd');
  sendCmd(cmd, source);
}

function termKey(e) {
  const inp = document.getElementById('term-in');
  if (e.key === 'Enter') {
    const cmd = inp.value.trim();
    if (!cmd) return;
    cmdHistory.unshift(cmd);
    histIdx = -1;
    inp.value = '';
    if (cmd === 'clear') { clearTerminals(); return; }
    routeTerminalCommand(cmd, 'terminal');
  }
  if (e.key === 'ArrowUp') {
    histIdx = Math.min(histIdx+1, cmdHistory.length-1);
    inp.value = cmdHistory[histIdx] || '';
    e.preventDefault();
  }
  if (e.key === 'ArrowDown') {
    histIdx = Math.max(histIdx-1, -1);
    inp.value = histIdx < 0 ? '' : cmdHistory[histIdx];
    e.preventDefault();
  }
}
function qtKey(e) {
  const inp = document.getElementById('qt-in');
  if (e.key === 'ArrowUp') {
    histIdx = Math.min(histIdx+1, cmdHistory.length-1);
    inp.value = cmdHistory[histIdx] || '';
    e.preventDefault();
    return;
  }
  if (e.key === 'ArrowDown') {
    histIdx = Math.max(histIdx-1, -1);
    inp.value = histIdx < 0 ? '' : cmdHistory[histIdx];
    e.preventDefault();
    return;
  }
  if (e.key !== 'Enter') return;
  const cmd = inp.value.trim(); inp.value = '';
  if (!cmd) return;
  cmdHistory.unshift(cmd);
  histIdx = -1;
  if (cmd === 'clear') { clearTerminals(); return; }
  routeTerminalCommand(cmd, 'quick');
}

// ── DEMO CMD PARSER (when no real ESP32) ─────────────────────────
const demoPins = {};
[2,4,5,12,13,14,15,18,19,21,22,23,25,26,27,32,33].forEach(p => demoPins[p]={mode:'OUTPUT',val:0,pwm:0});
[34,35,36,39].forEach(p => demoPins[p]={mode:'INPUT',val:0,pwm:0});

function demoCmd(cmd) {
  cmd = cmd.trim();
  if (cmd === 'help') return '[info] gpio read/write/mode/pwm/list\n[info] sys uptime/heap/temp\n[info] wifi status\n[info] settings nightlight/airplane on/off\n[info] clear';
  if (cmd === 'gpio list') {
    return Object.entries(demoPins).map(([p,s]) =>
      `  GPIO${p} [${s.mode}] = ${s.pwm>0?'PWM:'+s.pwm:s.val?'HIGH':'LOW'}`).join('\n');
  }
  if (cmd.startsWith('gpio write ')) {
    const parts = cmd.split(' '); const pin=parseInt(parts[2]); const v=parts[3]?.toLowerCase();
    if (!demoPins[pin]) return '[err] pin not found';
    if (demoPins[pin].mode==='INPUT') return '[err] pin is INPUT';
    demoPins[pin].val = v==='high'?1:0; demoPins[pin].pwm=0;
    renderGPIOGrid();
    return '[ok] GPIO'+pin+' = '+(v==='high'?'HIGH':'LOW');
  }
  if (cmd.startsWith('gpio pwm ')) {
    const parts=cmd.split(' ');const pin=parseInt(parts[2]);const duty=parseInt(parts[3]);
    if (!demoPins[pin]) return '[err] pin not found';
    if (isNaN(duty)||duty<0||duty>255) return '[err] duty 0-255';
    demoPins[pin].pwm=duty; demoPins[pin].val=duty>0?1:0;
    renderGPIOGrid(); return '[ok] GPIO'+pin+' PWM='+duty;
  }
  if (cmd.startsWith('gpio read ')) {
    const pin=parseInt(cmd.split(' ')[2]);
    if (!demoPins[pin]) return '[err] pin not found';
    return '[ok] GPIO'+pin+' = '+(demoPins[pin].val?'HIGH':'LOW');
  }
  if (cmd.startsWith('gpio mode ')) {
    const parts=cmd.split(' ');const pin=parseInt(parts[2]);const m=parts[3]?.toLowerCase();
    if (!demoPins[pin]) return '[err] pin not found';
    if ([34,35,36,39].includes(pin)) return '[err] hardware input-only pin';
    demoPins[pin].mode=m==='input'?'INPUT':'OUTPUT';
    renderGPIOGrid(); return '[ok] GPIO'+pin+' mode='+m.toUpperCase();
  }
  if (cmd==='sys uptime') return '[ok] Uptime: 00:01:23 (demo)';
  if (cmd==='sys heap') return '[ok] Free heap: 136 KB / 320 KB';
  if (cmd==='sys temp') return '[ok] CPU temp: 47°C';
  if (cmd==='wifi status') return '[ok] Connected · HomeNet_5G · -42 dBm · 192.168.1.42';
  if (cmd==='bt status') return '[warn] Bluetooth controls are unavailable in this build';
  if (cmd.startsWith('settings nightlight ')) {
    nightlightOn = cmd.endsWith('on');
    document.body.classList.toggle('nightlight', nightlightOn);
    document.getElementById('tog-nl').className = 'toggle-wrap ' + (nightlightOn?'on':'off');
    return '[ok] Night light: ' + (nightlightOn?'on':'off');
  }
  if (cmd.startsWith('settings airplane ')) {
    airplaneOn = cmd.endsWith('on');
    document.getElementById('tog-ap').className = 'toggle-wrap ' + (airplaneOn?'on':'off');
    return '[ok] Airplane mode: ' + (airplaneOn?'on':'off');
  }
  return '[err] Unknown command. Type help';
}

// ── GPIO GRID ────────────────────────────────────────────────────
function renderGPIOGrid() {
  const grid = document.getElementById('gpio-grid');
  grid.innerHTML = '';
  Object.entries(demoPins).forEach(([pin, s]) => {
    const p = parseInt(pin);
    let cls = 'pin', stateStr = 'LOW', stateClass = 'pin-state-low';
    if (selectedPin === p) cls += ' sel';
    if (s.mode==='INPUT') { cls+=' inp'; stateStr='INPUT'; stateClass='pin-state-inp'; }
    else if (s.pwm>0) { cls+=' pwm'; stateStr='PWM '+s.pwm; stateClass='pin-state-pwm'; }
    else if (s.val) { cls+=' high'; stateStr='HIGH'; stateClass='pin-state-high'; }
    const el = document.createElement('div');
    el.className = cls;
    el.innerHTML = `<div class="pin-num">GPIO${p}</div><div class="pin-mode">${s.mode}</div><div class="pin-state ${stateClass}">${stateStr}</div>`;
    el.onclick = async () => {
      selectedPin = p;
      syncControlsFromPin(p);

      if (s.mode !== 'OUTPUT') {
        gpioMsg('GPIO' + p + ' is INPUT mode. Change mode to OUTPUT first.', false);
        renderGPIOGrid();
        return;
      }

      const nextVal = s.val ? 0 : 1;
      await writePinFromGrid(p, nextVal);
    };
    grid.appendChild(el);
  });
}

function syncControlsFromPin(pin) {
  const st = demoPins[pin];
  if (!st) return;
  const pinInput = document.getElementById('ctrl-pin');
  const modeInput = document.getElementById('ctrl-mode');
  const pwmInput = document.getElementById('ctrl-pwm');
  pinInput.value = pin;
  modeInput.value = st.mode === 'INPUT' ? 'INPUT' : 'OUTPUT';
  pwmInput.value = st.pwm || 0;
}

async function writePinFromGrid(pin, value) {
  if (ENABLE_DEMO_MODE && token === 'demo') {
    demoPins[pin].val = value;
    demoPins[pin].pwm = 0;
    renderGPIOGrid();
    tLog('[ok] GPIO'+pin+' = '+(value ? 'HIGH' : 'LOW'), 't-ok');
    gpioMsg('GPIO' + pin + ' set to ' + (value ? 'HIGH' : 'LOW'));
    return;
  }

  try {
    const r = await apiFetch('/api/gpio/write', {
      method:'POST',
      headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({pin, value})
    });
    if (!r.ok) throw new Error('gpio_write_failed');
    demoPins[pin].val = value;
    demoPins[pin].pwm = 0;
    renderGPIOGrid();
    gpioMsg('GPIO' + pin + ' set to ' + (value ? 'HIGH' : 'LOW'));
    tLog('[ok] GPIO'+pin+' = '+(value ? 'HIGH' : 'LOW'), 't-ok');
  } catch (e) {
    gpioMsg('failed to write GPIO' + pin, false);
    tLog('[err] GPIO'+pin+' write failed', 't-err');
  }
}

function updateGpioGrid(pins) {
  pins.forEach(p => {
    if (demoPins[p.pin] !== undefined) {
      demoPins[p.pin] = {mode:p.mode, val:p.value, pwm:p.pwm};
    }
  });
  if (selectedPin !== null) syncControlsFromPin(selectedPin);
  renderGPIOGrid();
}

// ── GPIO CONTROLS ────────────────────────────────────────────────
function getPin() { return parseInt(document.getElementById('ctrl-pin').value); }
function gpioMsg(msg, ok=true) {
  const el = document.getElementById('gpio-msg');
  el.textContent = msg;
  el.style.color = ok ? 'var(--green)' : 'var(--red)';
}

async function pinWrite(val) {
  const pin = getPin();
  if (isNaN(pin)) { gpioMsg('enter a pin number', false); return; }
  try {
    await apiFetch('/api/gpio/write', {
      method:'POST', headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({pin, value:val})
    });
  } catch(e) {}
  sendCmd('gpio write ' + pin + ' ' + (val ? 'high' : 'low'));
  gpioMsg('GPIO' + pin + ' set to ' + (val ? 'HIGH' : 'LOW'));
}

async function pinPWM() {
  const pin = getPin(), duty = parseInt(document.getElementById('ctrl-pwm').value);
  if (isNaN(pin)||isNaN(duty)) { gpioMsg('fill in pin and duty', false); return; }
  try {
    await apiFetch('/api/gpio/pwm', {
      method:'POST', headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({pin, duty})
    });
  } catch(e) {}
  sendCmd('gpio pwm ' + pin + ' ' + duty);
  gpioMsg('GPIO' + pin + ' PWM = ' + duty);
}

async function pinMode() {
  const pin = getPin(), mode = document.getElementById('ctrl-mode').value.toLowerCase();
  if (isNaN(pin)) { gpioMsg('enter a pin number', false); return; }
  try {
    await apiFetch('/api/gpio/mode', {
      method:'POST', headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({pin, mode})
    });
  } catch(e) {}
  sendCmd('gpio mode ' + pin + ' ' + mode);
  gpioMsg('GPIO' + pin + ' mode = ' + mode.toUpperCase());
}

// ── WIFI ─────────────────────────────────────────────────────────
async function scanWifi() {
  document.getElementById('wifi-list').innerHTML = '<div class="hint">scanning...</div>';
  try {
    const r = await apiFetch('/api/wifi/scan');
    if (!r.ok) throw new Error('scan_failed');
    const d = await r.json();
    renderWifiList(Array.isArray(d.networks) ? d.networks : []);
  } catch(e) {
    if (ENABLE_DEMO_MODE && token === 'demo') {
      renderWifiList([
        {ssid:'HomeNet_5G', rssi:-42, open:false},
        {ssid:'Neighbor_2G', rssi:-71, open:false},
        {ssid:'Office_Guest', rssi:-84, open:true},
      ]);
      return;
    }
    document.getElementById('wifi-list').innerHTML = '<div class="hint hint-error">scan failed or unauthorized</div>';
  }
}

function sigBars(rssi) {
  const str = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  let html = '<div class="sig">';
  for (let i=1; i<=4; i++) html += `<span class="h${i} ${i<=str?'lit':''}"></span>`;
  return html + '</div>';
}

function renderWifiList(nets) {
  const el = document.getElementById('wifi-list');
  el.innerHTML = '';
  if (!Array.isArray(nets) || nets.length === 0) {
    el.innerHTML = '<div class="hint">no networks found</div>';
    return;
  }
  nets.forEach(n => {
    const d = document.createElement('div');
    d.className = 'net-item';
    d.innerHTML = `<div><div class="net-name">${n.ssid} ${n.open?'<span class="badge badge-gray">open</span>':''}</div><div class="net-meta">${n.rssi} dBm</div></div>${sigBars(n.rssi)}`;
    d.onclick = () => { document.getElementById('wifi-ssid').value = n.ssid; };
    el.appendChild(d);
  });
}

async function connectWifi() {
  const ssid = document.getElementById('wifi-ssid').value.trim();
  const pass = document.getElementById('wifi-pass').value;
  const msg = document.getElementById('wifi-msg');
  if (!ssid) { msg.style.color='var(--red)'; msg.textContent='enter an SSID'; return; }
  msg.style.color='var(--warn)'; msg.textContent='connecting to ' + ssid + '...';
  try {
    const r = await apiFetch('/api/wifi/connect', {
      method:'POST', headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({ssid, password:pass})
    });
    if (!r.ok) throw new Error('connect_failed');
    msg.style.color='var(--green)'; msg.textContent='connecting... check dashboard for status';
  } catch(e) {
    msg.style.color='var(--red)'; msg.textContent='connect request failed';
  }
}

async function disconnectWifi() {
  try { await apiFetch('/api/wifi/disconnect', {method:'POST'}); } catch(e){}
  document.getElementById('wifi-msg').style.color='var(--red)';
  document.getElementById('wifi-msg').textContent='disconnected';
}

// ── BLUETOOTH ────────────────────────────────────────────────────
function scanBT() {
  const list = document.getElementById('bt-list');
  list.innerHTML = '<div class="hint">scanning nearby devices...</div>';
  setTimeout(() => {
    renderBTList([
      {name:'My Laptop', mac:'XX:XX:XX:4A:2F', paired:true},
      {name:'Phone Hotspot', mac:'XX:XX:XX:98:12', paired:false},
      {name:'ESP32 Sensor', mac:'XX:XX:XX:AB:CD', paired:false}
    ]);
  }, 900);
}

function renderBTList(devs) {
  const el = document.getElementById('bt-list');
  el.innerHTML = '';
  devs.forEach(d => {
    const item = document.createElement('div');
    item.className = 'net-item ' + (d.paired ? 'paired' : '');
    item.innerHTML = `<div><div class="net-name">${d.name} ${d.paired?'<span class="badge badge-b">paired</span>':''}</div><div class="net-meta">${d.mac}</div></div><button class="btn btn-sm ${d.paired?'btn-danger':''}" onclick="toggleBT(this,${d.paired})">${d.paired?'unpair':'pair'}</button>`;
    el.appendChild(item);
  });
}

function toggleBT(btn, paired) {
  btn.textContent = paired ? 'unpairing...' : 'pairing...';
  setTimeout(() => {
    const nowPaired = !paired;
    btn.textContent = nowPaired ? 'unpair' : 'pair';
    btn.className = 'btn btn-sm ' + (nowPaired ? 'btn-danger' : '');
    btn.closest('.net-item').classList.toggle('paired', nowPaired);
  }, 600);
}

// ── TOGGLES ──────────────────────────────────────────────────────
function togEl(id) {
  const el = document.getElementById(id);
  const isOn = el.classList.contains('on');
  el.className = 'toggle-wrap ' + (isOn ? 'off' : 'on');
}

async function togNightlight() {
  togEl('tog-nl');
  nightlightOn = document.getElementById('tog-nl').classList.contains('on');
  document.body.classList.toggle('nightlight', nightlightOn);
  try {
    await apiFetch('/api/settings', {
      method:'POST', headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({nightlight: nightlightOn})
    });
  } catch(e) {}
}

async function togAirplane() {
  togEl('tog-ap');
  airplaneOn = document.getElementById('tog-ap').classList.contains('on');
  document.getElementById('chip-wifi').style.opacity = airplaneOn ? '0.3' : '1';
  document.getElementById('chip-bt').style.opacity = airplaneOn ? '0.3' : '1';
  try {
    await apiFetch('/api/settings', {
      method:'POST', headers:{'Content-Type':'application/json','X-Token':token},
      body:JSON.stringify({airplane_mode: airplaneOn})
    });
  } catch(e) {}
}

// ── MODALS ───────────────────────────────────────────────────────
function openOTA() { document.getElementById('modal-ota').classList.add('open'); }
function closeModal(id) { document.getElementById(id).classList.remove('open'); }

async function doOTA() {
  const file = document.getElementById('ota-file').files[0];
  const msg = document.getElementById('ota-msg');
  if (!file) { msg.style.color='var(--red)'; msg.textContent='select a .bin file'; return; }
  msg.style.color='var(--warn)'; msg.textContent='uploading ' + file.name + '...';
  const fd = new FormData(); fd.append('firmware', file);
  try {
    const r = await apiFetch('/api/ota', {method:'POST', body:fd});
    const d = await r.json();
    if (d.ok) { msg.style.color='var(--green)'; msg.textContent='flashed! rebooting...'; }
    else { msg.style.color='var(--red)'; msg.textContent='error: ' + d.error; }
  } catch(e) { msg.style.color='var(--red)'; msg.textContent='connection error'; }
}

async function doReboot() {
  if (!confirm('Reboot the ESP-32?')) return;
  try { await apiFetch('/api/reboot', {method:'POST'}); } catch(e) {}
  tLog('[warn] Rebooting...', 't-warn');
  setTimeout(() => location.reload(), 3000);
}

// ── INIT ─────────────────────────────────────────────────────────
restoreSession();
</script>
</body>
</html>
)WEBOS";

// ─── HMAC TOKEN ──────────────────────────────────────────────────
String generateToken(const String& user) {
    String payload = user + ":" + String(millis()) + ":" + String(esp_random());
    unsigned char hmac[32];
    const char* key = TOKEN_SECRET;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char*)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char*)payload.c_str(), payload.length());
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);
    // Base64-like hex encode
    String token = "";
    for (int i = 0; i < 16; i++) {
        char hex[3];
        sprintf(hex, "%02x", hmac[i]);
        token += hex;
    }
    return token;
}

bool isAuthenticated(AsyncWebServerRequest* req) {
    if (sessionToken.isEmpty()) return false;

    if (SESSION_TIMEOUT_SEC > 0 && sessionLastSeenMs > 0) {
        unsigned long idleMs = millis() - sessionLastSeenMs;
        if (idleMs > (unsigned long)SESSION_TIMEOUT_SEC * 1000UL) {
            sessionToken = "";
            sessionLastSeenMs = 0;
            return false;
        }
    }

    if (req->hasHeader("X-Token")) {
        bool ok = req->getHeader("X-Token")->value() == sessionToken;
        if (ok) sessionLastSeenMs = millis();
        return ok;
    }
    if (req->hasParam("token")) {
        bool ok = req->getParam("token")->value() == sessionToken;
        if (ok) sessionLastSeenMs = millis();
        return ok;
    }
    return false;
}

void rejectUnauth(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse(401, "application/json",
        "{\"ok\":false,\"error\":\"unauthorized\"}");
    req->send(res);
}

bool isApModeActive() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK) return apMode;
  return mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA;
}

bool isInputOnlyPin(int pin) {
  for (int i = 0; i < IN_COUNT; i++) {
    if (INPUT_PINS[i] == pin) return true;
  }
  return false;
}

bool collectRequestBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total, String& bodyOut) {
  if (index == 0) {
    String* acc = new String();
    if (!acc) {
      req->send(500, "application/json", "{\"ok\":false,\"error\":\"alloc failed\"}");
      return false;
    }
    acc->reserve(total);
    req->_tempObject = acc;
  }

  String* acc = reinterpret_cast<String*>(req->_tempObject);
  if (!acc) {
    req->send(500, "application/json", "{\"ok\":false,\"error\":\"body buffer missing\"}");
    return false;
  }

  acc->concat(reinterpret_cast<const char*>(data), len);
  if (index + len < total) return false;

  bodyOut = *acc;
  delete acc;
  req->_tempObject = nullptr;
  return true;
}

bool parseJsonBody(AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total, JsonDocument& doc) {
  String body;
  if (!collectRequestBody(req, data, len, index, total, body)) return false;

  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid json\"}");
    return false;
  }
  return true;
}

void startApDnsCaptive() {
  if (dnsCaptiveRunning) return;
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsCaptiveRunning = dnsServer.start(53, "*", WiFi.softAPIP());
  if (dnsCaptiveRunning) {
    Serial.printf("[dns] Captive DNS active at %s\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[dns] Captive DNS start failed");
  }
}

void stopApDnsCaptive() {
  if (!dnsCaptiveRunning) return;
  dnsServer.stop();
  dnsCaptiveRunning = false;
  Serial.println("[dns] Captive DNS stopped");
}

void stopMdnsIfRunning() {
    if (!mdnsStarted) return;
    MDNS.end();
    mdnsStarted = false;
  activeMdnsHostname = "";
}

const char* desiredMdnsHostname() {
  if (WiFi.status() == WL_CONNECTED) return WEBOS_HOSTNAME;
  if (isApModeActive()) return AP_MDNS_HOSTNAME;
  return nullptr;
}

void ensureMdnsForCurrentMode() {
  const char* desired = desiredMdnsHostname();

  if (!desired) {
    stopMdnsIfRunning();
    return;
  }

  if (mdnsStarted && activeMdnsHostname == desired) return;

  stopMdnsIfRunning();
  if (MDNS.begin(desired)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    activeMdnsHostname = desired;
    Serial.printf("[net] URL: http://%s.local\n", desired);
  } else {
    Serial.printf("[net] mDNS start failed for %s.local\n", desired);
  }
}

void startFallbackAp() {
    Serial.println("[wifi] Falling back to AP mode");
    stopMdnsIfRunning();
    // AP+STA keeps setup hotspot available while enabling Wi-Fi scans.
    WiFi.mode(WIFI_AP_STA);
    WiFi.setSleep(false);
    IPAddress apIP(192, 168, 4, 1);
    IPAddress apGW(192, 168, 4, 1);
    IPAddress apMask(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, apGW, apMask);

    bool apOk = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, AP_HIDDEN, AP_MAX_CLIENTS);
    if (!apOk) {
        Serial.println("[wifi] Secured AP start failed, retrying as open AP");
        apOk = WiFi.softAP(AP_SSID);
    }

    if (!apOk) {
        Serial.println("[wifi] AP start FAILED");
        apMode = false;
        stopApDnsCaptive();
    } else {
        esp_wifi_set_max_tx_power(84);
        Serial.printf("[wifi] AP: %s | IP: %s | CH: %d\n", AP_SSID,
            WiFi.softAPIP().toString().c_str(), AP_CHANNEL);
      Serial.printf("[net] AP URL: http://%s\n", WiFi.softAPIP().toString().c_str());
      Serial.printf("[net] AP mDNS target: http://%s.local\n", AP_MDNS_HOSTNAME);
        apMode = true;
      startApDnsCaptive();
      ensureMdnsForCurrentMode();
    }
}

void beginStaConnectAsync(const String& ssid, const String& pass, bool keepApEnabled) {
    stopMdnsIfRunning();
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);
    WiFi.mode(keepApEnabled ? WIFI_AP_STA : WIFI_STA);

    if (ssid.isEmpty()) {
        Serial.println("[wifi] STA connect skipped (empty SSID)");
        return;
    }

    WiFi.begin(ssid.c_str(), pass.c_str());

    staBootConnectPending = true;
    staBootConnectStartMs = millis();
    Serial.printf("[wifi] Boot STA connect started (%s): %s\n",
        keepApEnabled ? "AP+STA" : "STA", ssid.c_str());
}

void handleBootStaConnectProgress() {
    if (!staBootConnectPending) return;

    if (WiFi.status() == WL_CONNECTED) {
        staBootConnectPending = false;
        Serial.printf("[wifi] STA connected in %lu ms | IP: %s\n",
            millis() - staBootConnectStartMs,
            WiFi.localIP().toString().c_str());
      ensureMdnsForCurrentMode();

        // If AP was only started as a fast-boot fallback, disable it once STA is ready.
        if (apStartedForBootFallback) {
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            apMode = false;
            apStartedForBootFallback = false;
          stopApDnsCaptive();
            Serial.println("[wifi] Fallback AP disabled (STA ready)");
        }
        return;
    }

    if (millis() - staBootConnectStartMs >= STA_CONNECT_TIMEOUT_MS) {
        staBootConnectPending = false;
        Serial.printf("[wifi] Boot STA timeout after %lu ms\n", (unsigned long)STA_CONNECT_TIMEOUT_MS);

        // Keep AP available after timeout when fallback is enabled.
        if (apMode) {
            apStartedForBootFallback = false;
            Serial.println("[wifi] Keeping AP fallback active");
        } else if (AP_FALLBACK_ENABLED) {
            startFallbackAp();
        }
    }
}

// ─── GPIO INIT ───────────────────────────────────────────────────
void initGPIO() {
    int idx = 0;
    bool warnedPwmLimit = false;
    for (int i = 0; i < OUT_COUNT; i++) {
        pinStates[idx].pin       = OUTPUT_PINS[i];
        pinStates[idx].isOutput  = true;
        pinStates[idx].value     = false;
        pinStates[idx].pwmDuty   = 0;
        pinStates[idx].pwmChannel= (i < MAX_LEDC_CHANNELS) ? i : -1;
        pinMode(OUTPUT_PINS[i], OUTPUT);
        digitalWrite(OUTPUT_PINS[i], LOW);
        // Setup PWM only while channels are available; overflow pins remain digital-only.
        if (pinStates[idx].pwmChannel >= 0) {
          ledcAttachChannel(OUTPUT_PINS[i], PWM_FREQ_HZ, PWM_RESOLUTION_BITS, pinStates[idx].pwmChannel);
          ledcWriteChannel(pinStates[idx].pwmChannel, 0);
        } else if (!warnedPwmLimit) {
            warnedPwmLimit = true;
            Serial.println("[gpio] PWM channel limit reached; remaining output pins are digital-only");
        }
        idx++;
    }
    for (int i = 0; i < IN_COUNT; i++) {
        pinStates[idx].pin       = INPUT_PINS[i];
        pinStates[idx].isOutput  = false;
        pinStates[idx].value     = false;
        pinStates[idx].pwmDuty   = 0;
        pinStates[idx].pwmChannel= -1;
        pinMode(INPUT_PINS[i], INPUT);
        idx++;
    }
    totalPins = idx;
}

PinState* findPin(int pin) {
    for (int i = 0; i < totalPins; i++) {
        if (pinStates[i].pin == pin) return &pinStates[i];
    }
    return nullptr;
}

// ─── GPIO JSON ───────────────────────────────────────────────────
String buildGpioJson() {
    JsonDocument doc;
    JsonArray arr = doc["pins"].to<JsonArray>();
    for (int i = 0; i < totalPins; i++) {
        PinState& p = pinStates[i];
        // Update input pin values
        if (!p.isOutput) p.value = digitalRead(p.pin);
        JsonObject obj = arr.add<JsonObject>();
        obj["pin"]      = p.pin;
        obj["mode"]     = p.isOutput ? "OUTPUT" : "INPUT";
        obj["value"]    = p.value ? 1 : 0;
        obj["pwm"]      = p.pwmDuty;
    }
    String out;
    serializeJson(doc, out);
    return out;
}

String buildStatusJson() {
    unsigned long sec = (millis() - bootMillis) / 1000;
    float tempC = temperatureRead();
    size_t totalFlash = LittleFS.totalBytes();
    size_t usedFlash  = LittleFS.usedBytes();
  bool apActive = isApModeActive();
  apMode = apActive;
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  String wifiIp = "0.0.0.0";
  if (wifiConnected) wifiIp = WiFi.localIP().toString();
  else if (apActive) wifiIp = WiFi.softAPIP().toString();

    JsonDocument doc;
    doc["ok"]              = true;
    doc["uptime_sec"]      = sec;
    doc["free_heap_kb"]    = ESP.getFreeHeap() / 1024;
    doc["total_heap_kb"]   = ESP.getHeapSize() / 1024;
    doc["flash_used_kb"]   = usedFlash / 1024;
    doc["flash_total_kb"]  = totalFlash / 1024;
    doc["cpu_temp_c"]      = (int)tempC;
    doc["wifi_connected"]  = wifiConnected;
    doc["wifi_ssid"]       = WiFi.SSID();
    doc["wifi_rssi"]       = WiFi.RSSI();
    doc["wifi_ip"]         = wifiIp;
    doc["nightlight"]      = nightLight;
    doc["airplane_mode"]   = airplaneMode;
    doc["ap_mode"]         = apActive;

    String out;
    serializeJson(doc, out);
    return out;
}

// ─── TERMINAL COMMAND PARSER ─────────────────────────────────────
String handleTerminalCmd(const String& raw) {
    String cmd = raw;
    cmd.trim();

    if (cmd == "help") {
        return "[info] Commands:\n"
               "  gpio read <pin>\n"
               "  gpio write <pin> <high|low>\n"
               "  gpio mode <pin> <input|output>\n"
               "  gpio pwm <pin> <0-255>\n"
               "  gpio list\n"
               "  sys uptime\n"
               "  sys heap\n"
               "  sys temp\n"
               "  sys reset\n"
               "  wifi status\n"
               "  wifi connect <ssid> <password>\n"
               "  wifi disconnect\n"
               "  settings nightlight <on|off>\n"
               "  settings airplane <on|off>\n"
               "  clear";
    }

    if (cmd == "gpio list") {
        String res = "[info] GPIO pin states:\n";
        for (int i = 0; i < totalPins; i++) {
            PinState& p = pinStates[i];
            if (!p.isOutput) p.value = digitalRead(p.pin);
            res += "  GPIO" + String(p.pin) + " [" + (p.isOutput?"OUT":"IN") + "] = ";
            if (p.pwmDuty > 0) res += "PWM:" + String(p.pwmDuty);
            else res += p.value ? "HIGH" : "LOW";
            res += "\n";
        }
        return res;
    }

    if (cmd.startsWith("gpio ")) {
        String rest = cmd.substring(5);
        int sp1 = rest.indexOf(' ');
        String sub = (sp1 < 0) ? rest : rest.substring(0, sp1);
        String args = (sp1 < 0) ? "" : rest.substring(sp1 + 1);
        args.trim();

        if (sub == "read") {
            int pin = args.toInt();
            PinState* ps = findPin(pin);
            if (!ps) return "[err] GPIO" + String(pin) + " not found";
            if (!ps->isOutput) ps->value = digitalRead(pin);
            return "[ok] GPIO" + String(pin) + " = " + (ps->value ? "HIGH" : "LOW");
        }

        if (sub == "write") {
            int sp = args.indexOf(' ');
            if (sp < 0) return "[err] usage: gpio write <pin> <high|low>";
            int pin = args.substring(0, sp).toInt();
            String val = args.substring(sp + 1);
            val.trim(); val.toLowerCase();
            PinState* ps = findPin(pin);
            if (!ps) return "[err] GPIO" + String(pin) + " not found";
            if (!ps->isOutput) return "[err] GPIO" + String(pin) + " is INPUT only";
            bool high = (val == "high" || val == "1");
            ps->value = high;
            ps->pwmDuty = 0;
            if (ps->pwmChannel >= 0) ledcWriteChannel(ps->pwmChannel, high ? 255 : 0);
            else digitalWrite(pin, high ? HIGH : LOW);
            return "[ok] GPIO" + String(pin) + " set to " + (high ? "HIGH" : "LOW");
        }

        if (sub == "mode") {
            int sp = args.indexOf(' ');
            if (sp < 0) return "[err] usage: gpio mode <pin> <input|output>";
            int pin = args.substring(0, sp).toInt();
            String mode = args.substring(sp + 1);
            mode.trim(); mode.toLowerCase();
            PinState* ps = findPin(pin);
            if (!ps) return "[err] GPIO" + String(pin) + " not found";
            // Input-only pins cannot be changed
            for (int i = 0; i < IN_COUNT; i++) {
                if (INPUT_PINS[i] == pin) return "[err] GPIO" + String(pin) + " is hardware input-only";
            }
            if (mode == "input") {
                ps->isOutput = false;
                ps->pwmDuty = 0;
                pinMode(pin, INPUT);
            } else {
                ps->isOutput = true;
                pinMode(pin, OUTPUT);
                if (ps->pwmChannel >= 0) ledcAttachChannel(pin, PWM_FREQ_HZ, PWM_RESOLUTION_BITS, ps->pwmChannel);
            }
            String modeLabel = mode;
            modeLabel.toUpperCase();
            return "[ok] GPIO" + String(pin) + " mode = " + modeLabel;
        }

        if (sub == "pwm") {
            int sp = args.indexOf(' ');
            if (sp < 0) return "[err] usage: gpio pwm <pin> <0-255>";
            int pin = args.substring(0, sp).toInt();
            int duty = args.substring(sp + 1).toInt();
            if (duty < 0 || duty > 255) return "[err] PWM duty must be 0-255";
            PinState* ps = findPin(pin);
            if (!ps) return "[err] GPIO" + String(pin) + " not found";
            if (!ps->isOutput) return "[err] GPIO" + String(pin) + " is INPUT";
            if (ps->pwmChannel < 0) return "[err] PWM unavailable on GPIO" + String(pin);
            ps->pwmDuty = duty;
            ps->value = duty > 0;
            ledcWriteChannel(ps->pwmChannel, duty);
            return "[ok] GPIO" + String(pin) + " PWM = " + String(duty);
        }
    }

    if (cmd == "sys uptime") {
        unsigned long sec = (millis() - bootMillis) / 1000;
        unsigned long h = sec / 3600, m = (sec % 3600) / 60, s = sec % 60;
        char buf[32];
        sprintf(buf, "[ok] Uptime: %02lu:%02lu:%02lu", h, m, s);
        return String(buf);
    }
    if (cmd == "sys heap") {
        return "[ok] Free heap: " + String(ESP.getFreeHeap() / 1024) + " KB / " +
               String(ESP.getHeapSize() / 1024) + " KB";
    }
    if (cmd == "sys temp") {
        float c = temperatureRead();
        return "[ok] CPU temp: " + String(c, 1) + " C";
    }
    if (cmd == "sys reset") {
        return "[warn] Rebooting in 1s...";
        // Actual reset happens after response is sent
    }
    if (cmd == "wifi status") {
        if (WiFi.status() == WL_CONNECTED) {
            return "[ok] Wi-Fi connected: " + WiFi.SSID() +
                   " | IP: " + WiFi.localIP().toString() +
                   " | RSSI: " + String(WiFi.RSSI()) + " dBm";
        }
        return "[warn] Wi-Fi not connected";
    }
    if (cmd.startsWith("wifi connect ")) {
        String args = cmd.substring(13);
        int sp = args.indexOf(' ');
        if (sp < 0) return "[err] usage: wifi connect <ssid> <password>";
        String ssid = args.substring(0, sp);
        String pass = args.substring(sp + 1);
        stopMdnsIfRunning();
        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), pass.c_str());
        apMode = false;
        return "[info] Connecting to " + ssid + "...";
    }
    if (cmd == "wifi disconnect") {
        WiFi.disconnect();
        return "[ok] Wi-Fi disconnected";
    }
    if (cmd == "bt status") {
      return "[warn] Bluetooth controls are unavailable in this build";
    }
    if (cmd.startsWith("settings nightlight ")) {
        String val = cmd.substring(20);
        nightLight = (val == "on");
        prefs.putBool("nightlight", nightLight);
        return "[ok] Night light: " + String(nightLight ? "on" : "off");
    }
    if (cmd.startsWith("settings airplane ")) {
        String val = cmd.substring(18);
        airplaneMode = (val == "on");
        if (airplaneMode) {
            stopMdnsIfRunning();
            WiFi.disconnect(); WiFi.mode(WIFI_OFF);
        apMode = false;
        } else {
            String ssid = savedSSID.isEmpty() ? String(STA_DEFAULT_SSID) : savedSSID;
            String pass = savedSSID.isEmpty() ? String(STA_DEFAULT_PASSWORD) : savedPass;
            WiFi.mode(WIFI_STA);
            if (!ssid.isEmpty()) WiFi.begin(ssid.c_str(), pass.c_str());
        apMode = false;
        }
        prefs.putBool("airplane", airplaneMode);
        return "[ok] Airplane mode: " + String(airplaneMode ? "on" : "off");
    }
    if (cmd == "clear") return "__CLEAR__";

    return "[err] Unknown command. Type 'help' for command list.";
}

// ─── WEBSOCKET HANDLERS ──────────────────────────────────────────
void onTerminalEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                     AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    AsyncWebServerRequest* req = reinterpret_cast<AsyncWebServerRequest*>(arg);
    if (!isAuthenticated(req)) {
      client->close();
      return;
    }
    return;
  }

    if (type == WS_EVT_DATA) {
        String cmd = String((char*)data, len);
        String resp = handleTerminalCmd(cmd);
        client->text(resp);
        if (cmd == "sys reset") {
            delay(500);
            ESP.restart();
        }
    }
}

void onGpioEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    AsyncWebServerRequest* req = reinterpret_cast<AsyncWebServerRequest*>(arg);
    if (!isAuthenticated(req)) {
      client->close();
      return;
    }
    client->text(buildGpioJson());
    return;
  }

    if (type == WS_EVT_DATA) {
        // Client can request immediate GPIO state push
        client->text(buildGpioJson());
    }
}

void onStatusEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                   AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    AsyncWebServerRequest* req = reinterpret_cast<AsyncWebServerRequest*>(arg);
    if (!isAuthenticated(req)) {
      client->close();
      return;
    }
    client->text(buildStatusJson());
    return;
  }

  if (type == WS_EVT_DATA) {
        client->text(buildStatusJson());
    }
}

// ─── API ROUTES ───────────────────────────────────────────────────
void setupRoutes() {

    // Always serve a fresh UI document to avoid browser stale-cache blank pages.
    auto sendIndexHtml = [](AsyncWebServerRequest* req) {
    AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html", reinterpret_cast<const uint8_t*>(INDEX_HTML), strlen(INDEX_HTML));
        res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        res->addHeader("Pragma", "no-cache");
        res->addHeader("Expires", "0");
        req->send(res);
    };

    server.on("/", HTTP_GET, sendIndexHtml);
    server.on("/index.html", HTTP_GET, sendIndexHtml);



    // ── AUTH ────────────────────────────────────────────────────────
    server.on("/api/auth", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
        if (!parseJsonBody(req, data, len, index, total, doc)) return;
            String user = doc["username"] | "";
            String pass = doc["password"] | "";
            if (user == WEBOS_USERNAME && pass == WEBOS_PASSWORD) {
                sessionToken = generateToken(user);
                sessionLastSeenMs = millis();
                JsonDocument res;
                res["ok"]    = true;
                res["token"] = sessionToken;
                String out; serializeJson(res, out);
                req->send(200, "application/json", out);
            } else {
                req->send(401, "application/json", "{\"ok\":false,\"error\":\"invalid credentials\"}");
            }
        });

    server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        sessionToken = "";
        sessionLastSeenMs = 0;
        req->send(200, "application/json", "{\"ok\":true}");
    });

    // ── SYSTEM STATUS ───────────────────────────────────────────────
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        req->send(200, "application/json", buildStatusJson());
    });

    // ── WI-FI ───────────────────────────────────────────────────────
    server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        if (airplaneMode) {
            req->send(409, "application/json", "{\"ok\":false,\"error\":\"airplane mode\"}");
            return;
        }

        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);
        if (mode == WIFI_MODE_AP) {
            WiFi.mode(WIFI_AP_STA);
        }

        int n = WiFi.scanNetworks(false, true);
        if (n < 0) {
            req->send(500, "application/json", "{\"ok\":false,\"error\":\"scan failed\"}");
            return;
        }

        JsonDocument doc;
        doc["ok"] = true;
        doc["count"] = n;
        JsonArray nets = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i).isEmpty()) continue;
            JsonObject net = nets.add<JsonObject>();
            net["ssid"]     = WiFi.SSID(i);
            net["rssi"]     = WiFi.RSSI(i);
            net["open"]     = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        }
        WiFi.scanDelete();
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(req)) { rejectUnauth(req); return; }
            JsonDocument doc;
        if (!parseJsonBody(req, data, len, index, total, doc)) return;
            savedSSID = (const char*)(doc["ssid"] | "");
            savedPass = (const char*)(doc["password"] | "");
            prefs.putString("ssid", savedSSID);
            prefs.putString("pass", savedPass);
            stopMdnsIfRunning();
        bool keepApEnabled = isApModeActive();
        WiFi.mode(keepApEnabled ? WIFI_AP_STA : WIFI_STA);
            WiFi.begin(savedSSID.c_str(), savedPass.c_str());
        apMode = keepApEnabled;
            req->send(200, "application/json", "{\"ok\":true,\"msg\":\"connecting\"}");
        });

    server.on("/api/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        WiFi.disconnect();
        req->send(200, "application/json", "{\"ok\":true}");
    });

    server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        JsonDocument doc;
        doc["connected"] = (WiFi.status() == WL_CONNECTED);
        doc["ssid"]      = WiFi.SSID();
        doc["ip"]        = WiFi.localIP().toString();
        doc["rssi"]      = WiFi.RSSI();
        String out; serializeJson(doc, out);
        req->send(200, "application/json", out);
    });

    // ── GPIO ────────────────────────────────────────────────────────
    server.on("/api/gpio", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        req->send(200, "application/json", buildGpioJson());
    });

    server.on("/api/gpio/write", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(req)) { rejectUnauth(req); return; }
            JsonDocument doc;
        if (!parseJsonBody(req, data, len, index, total, doc)) return;
            int  pin = doc["pin"]   | -1;
            int  val = doc["value"] | 0;
            PinState* ps = findPin(pin);
            if (!ps || !ps->isOutput) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid pin\"}");
                return;
            }
            ps->value   = val;
            ps->pwmDuty = 0;
            if (ps->pwmChannel >= 0) ledcWriteChannel(ps->pwmChannel, val ? 255 : 0);
            else digitalWrite(pin, val ? HIGH : LOW);
            req->send(200, "application/json", "{\"ok\":true}");
        });

    server.on("/api/gpio/pwm", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(req)) { rejectUnauth(req); return; }
            JsonDocument doc;
        if (!parseJsonBody(req, data, len, index, total, doc)) return;
            int pin  = doc["pin"]  | -1;
            int duty = doc["duty"] | 0;
            PinState* ps = findPin(pin);
            if (!ps || !ps->isOutput) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid pin\"}");
                return;
            }
            if (ps->pwmChannel < 0) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"PWM unavailable on this pin\"}");
                return;
            }
            ps->pwmDuty = constrain(duty, 0, 255);
            ps->value   = duty > 0;
            ledcWriteChannel(ps->pwmChannel, ps->pwmDuty);
            req->send(200, "application/json", "{\"ok\":true}");
        });

    server.on("/api/gpio/mode", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(req)) { rejectUnauth(req); return; }
            JsonDocument doc;
        if (!parseJsonBody(req, data, len, index, total, doc)) return;
            int    pin  = doc["pin"]  | -1;
            String mode = doc["mode"] | "output";
            PinState* ps = findPin(pin);
            if (!ps) {
                req->send(400, "application/json", "{\"ok\":false,\"error\":\"pin not found\"}");
                return;
            }
        if (isInputOnlyPin(pin)) {
          req->send(400, "application/json", "{\"ok\":false,\"error\":\"hardware input-only pin\"}");
          return;
        }
            if (mode == "input") {
                ps->isOutput = false; pinMode(pin, INPUT);
            } else {
                ps->isOutput = true; pinMode(pin, OUTPUT);
                if (ps->pwmChannel >= 0) ledcAttachChannel(pin, PWM_FREQ_HZ, PWM_RESOLUTION_BITS, ps->pwmChannel);
            }
            req->send(200, "application/json", "{\"ok\":true}");
        });

    // ── SETTINGS ────────────────────────────────────────────────────
    server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (!isAuthenticated(req)) { rejectUnauth(req); return; }
            JsonDocument doc;
        if (!parseJsonBody(req, data, len, index, total, doc)) return;
            if (doc["nightlight"].is<bool>()) {
                nightLight = doc["nightlight"].as<bool>();
                prefs.putBool("nightlight", nightLight);
            }
            if (doc["airplane_mode"].is<bool>()) {
                airplaneMode = doc["airplane_mode"].as<bool>();
                if (airplaneMode) {
                    stopMdnsIfRunning();
                    WiFi.disconnect(); WiFi.mode(WIFI_OFF);
                  stopApDnsCaptive();
            apMode = false;
                } else {
                    String ssid = savedSSID.isEmpty() ? String(STA_DEFAULT_SSID) : savedSSID;
                    String pass = savedSSID.isEmpty() ? String(STA_DEFAULT_PASSWORD) : savedPass;
                    WiFi.mode(WIFI_STA);
                    if (!ssid.isEmpty()) WiFi.begin(ssid.c_str(), pass.c_str());
            apMode = false;
                }
                prefs.putBool("airplane", airplaneMode);
            }
            req->send(200, "application/json", "{\"ok\":true}");
        });

    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!isAuthenticated(req)) { rejectUnauth(req); return; }
        req->send(200, "application/json", "{\"ok\":true}");
        delay(500);
        ESP.restart();
    });

    // ── OTA UPDATE ──────────────────────────────────────────────────
    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!isAuthenticated(req)) { rejectUnauth(req); return; }
            bool ok = !Update.hasError();
            AsyncWebServerResponse* res = req->beginResponse(
                ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"OTA failed\"}"
            );
            req->send(res);
            if (ok) { delay(500); ESP.restart(); }
        },
        [](AsyncWebServerRequest* req, String filename, size_t index, uint8_t* data, size_t len, bool final) {
            if (!isAuthenticated(req)) return;
            if (!index) {
                Serial.printf("[ota] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
            }
            if (!Update.hasError()) Update.write(data, len);
            if (final) {
                if (Update.end(true)) Serial.printf("[ota] Done: %u bytes\n", index + len);
                else Update.printError(Serial);
            }
        });

    // 404
    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
    });
}

// ─── SETUP ───────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[boot] WebOS-32 starting...");
    bootMillis = millis();

    if (strcmp(WEBOS_PASSWORD, "change-me-now") == 0) {
      Serial.println("[sec] WARNING: WEBOS_PASSWORD is default placeholder. Change it before production use.");
    }
    if (strcmp(TOKEN_SECRET, "replace-with-long-random-token-secret") == 0) {
      Serial.println("[sec] WARNING: TOKEN_SECRET is placeholder. Set a long random secret before production use.");
    }

    // NVS
    prefs.begin(NVS_NAMESPACE, false);
    savedSSID  = prefs.isKey("ssid") ? prefs.getString("ssid", "") : "";
    savedPass  = prefs.isKey("pass") ? prefs.getString("pass", "") : "";
    nightLight = prefs.getBool("nightlight", false);
    airplaneMode = prefs.getBool("airplane", false);

    // GPIO
    initGPIO();
    Serial.println("[boot] GPIO initialized");

    // LittleFS
    if (!LittleFS.begin(true)) {
        Serial.println("[boot] LittleFS mount FAILED");
    } else {
        Serial.println("[boot] LittleFS mounted");
    }

    // Wi-Fi first (non-blocking), so TCP/IP stack is initialized before server.begin().
    if (!airplaneMode) {
        WiFi.setHostname(WEBOS_HOSTNAME);
        String bootSsid = savedSSID.isEmpty() ? String(STA_DEFAULT_SSID) : savedSSID;
        String bootPass = savedSSID.isEmpty() ? String(STA_DEFAULT_PASSWORD) : savedPass;

        if (AP_FALLBACK_ENABLED) {
            startFallbackAp();
        apStartedForBootFallback = false;
  #if STA_AUTOCONNECT_ON_BOOT
        if (!bootSsid.isEmpty()) {
          apStartedForBootFallback = apMode;
          beginStaConnectAsync(bootSsid, bootPass, true);
        } else {
          Serial.println("[wifi] AP-first mode: waiting for manual STA connect from dashboard");
        }
  #else
        Serial.println("[wifi] AP-first mode: waiting for manual STA connect from dashboard");
  #endif
        } else {
            apMode = false;
        if (!bootSsid.isEmpty()) {
          beginStaConnectAsync(bootSsid, bootPass, false);
        } else {
          Serial.println("[wifi] No STA credentials configured at boot");
        }
        }
    } else {
        Serial.println("[wifi] Airplane mode enabled at boot");
    }

    // Start mDNS for current mode (AP onboarding or STA mode).
    ensureMdnsForCurrentMode();

    // WebSocket
    wsTerminal.onEvent(onTerminalEvent);
    wsGpio.onEvent(onGpioEvent);
    wsStatus.onEvent(onStatusEvent);
    server.addHandler(&wsTerminal);
    server.addHandler(&wsGpio);
    server.addHandler(&wsStatus);

    // Routes
    setupRoutes();
    server.begin();
    Serial.println("[boot] HTTP server started on port 80");

    Serial.println("[boot] WebOS-32 ready!");
}

// ─── LOOP ────────────────────────────────────────────────────────
unsigned long lastGpioBroadcast = 0;
unsigned long lastStatusBroadcast = 0;
unsigned long lastCleanup       = 0;
unsigned long lastWifiHeartbeat = 0;

void loop() {
    handleBootStaConnectProgress();

    // Broadcast GPIO state to all connected ws clients every 200ms
    if (millis() - lastGpioBroadcast > 200) {
        lastGpioBroadcast = millis();
        if (wsGpio.count() > 0) {
            wsGpio.textAll(buildGpioJson());
        }
    }

    if (millis() - lastStatusBroadcast > 1000) {
        lastStatusBroadcast = millis();
        if (wsStatus.count() > 0) {
            wsStatus.textAll(buildStatusJson());
        }
    }

    // WebSocket cleanup
    if (millis() - lastCleanup > 5000) {
        lastCleanup = millis();
        wsTerminal.cleanupClients();
        wsGpio.cleanupClients();
        wsStatus.cleanupClients();
    }

    if (millis() - lastWifiHeartbeat > 10000) {
        lastWifiHeartbeat = millis();
        wifi_mode_t mode = WIFI_MODE_NULL;
        esp_wifi_get_mode(&mode);

        ensureMdnsForCurrentMode();

        if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
          if (!dnsCaptiveRunning) startApDnsCaptive();
          Serial.printf("[wifi] AP heartbeat | IP: %s | clients: %d | hostname: %s.local\n",
            WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(), AP_MDNS_HOSTNAME);
        } else if (mode == WIFI_MODE_STA) {
          stopApDnsCaptive();
            Serial.printf("[wifi] STA heartbeat | connected: %s | IP: %s\n",
                WiFi.status() == WL_CONNECTED ? "yes" : "no",
                WiFi.localIP().toString().c_str());
        } else {
          stopApDnsCaptive();
            Serial.println("[wifi] Wi-Fi heartbeat | mode: OFF");
        }
    }

      if (dnsCaptiveRunning) {
        dnsServer.processNextRequest();
      }
}