/*
 * IronSight FPV Scout
 * Board: ESP32-S3 N16R8
 * Libraries: ESPAsyncWebServer, AsyncTCP, ESP32Servo, esp_camera
 *
 * Architecture:
 * 1) ESP32-S3 runs as a SoftAP and serves the control UI on port 80.
 * 2) The UI talks to /ws for drive + pan/tilt control.
 * 3) MJPEG video streaming runs on a dedicated HTTP server on port 81.
 * 4) Track control retains deadzone, curve shaping and slew limiting.
 */

#include <AsyncTCP.h>
#include <ESP32Servo.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <stdlib.h>
#include <string.h>

// ========== Build-time options ==========
#define ENABLE_DEBUG_LOG 0
#define ENABLE_SERVO_SELF_TEST 1

#if ENABLE_DEBUG_LOG
#define LOG_PRINTLN(x) Serial.println(x)
#define LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define LOG_PRINTLN(x) ((void)0)
#define LOG_PRINTF(...) ((void)0)
#endif

// Wi-Fi AP config
const char *ap_ssid = "IronSight_FPV_Scout";
const char *ap_password = "12345678";
IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// HTTP + WebSocket server
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
httpd_handle_t stream_httpd = nullptr;

// Track motor pins (A=left, B=right)
#define PWMA 5
#define AIN2 6
#define AIN1 7
#define PWMB 18
#define BIN2 17
#define BIN1 16

// Pan / tilt servo pins
#define PAN_SERVO_PIN 13
#define TILT_SERVO_PIN 12

// OV2640 camera pin map for YD-ESP32-S3-COREBOARD V1.4
#define CAM_PIN_PWDN 41
#define CAM_PIN_RESET 42
#define CAM_PIN_XCLK 15
#define CAM_PIN_SIOD 9
#define CAM_PIN_SIOC 8

#define CAM_PIN_D7 40
#define CAM_PIN_D6 39
#define CAM_PIN_D5 38
#define CAM_PIN_D4 11
#define CAM_PIN_D3 10
#define CAM_PIN_D2 4
#define CAM_PIN_D1 2
#define CAM_PIN_D0 1
#define CAM_PIN_VSYNC 21
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 14

// PWM config
const uint16_t PWM_FREQ = 5000;
const uint8_t PWM_RESOLUTION = 8;
const int16_t PWM_MAX = 255;

// Track tuning
const int16_t JOYSTICK_DEADZONE = 8;
const int16_t MOTOR_MIN_EFFECTIVE_PWM = 85;
const uint8_t TRACK_SLEW_STEP = 12;
const uint16_t DRIVE_UPDATE_INTERVAL_MS = 15;
const uint16_t TRACK_SIGNAL_TIMEOUT_MS = 350;
const uint8_t LEFT_TRACK_GAIN_PERCENT = 100;
const uint8_t RIGHT_TRACK_GAIN_PERCENT = 100;

// Servo objects
Servo panServo;
Servo tiltServo;

// Pan / tilt state
int16_t panAngle = 90;
int16_t tiltAngle = 95;
const int16_t PAN_MIN = 10;
const int16_t PAN_MAX = 170;
const int16_t TILT_MIN = 60;
const int16_t TILT_MAX = 135;
const uint16_t SERVO_UPDATE_INTERVAL_MS = 20;
const int16_t PAN_STEP_PER_TICK = 2;
const int16_t TILT_STEP_PER_TICK = 1;

// Track state
int16_t leftTargetPwm = 0;
int16_t rightTargetPwm = 0;
int16_t leftCurrentPwm = 0;
int16_t rightCurrentPwm = 0;
uint32_t lastTrackCmdMs = 0;
uint32_t lastDriveUpdateMs = 0;
int8_t panMoveDir = 0;
int8_t tiltMoveDir = 0;
uint32_t lastServoUpdateMs = 0;

// Stream tuning
const framesize_t CAMERA_FRAME_SIZE = FRAMESIZE_QVGA;
const int CAMERA_JPEG_QUALITY = 12;
const int CAMERA_FB_COUNT = 2;

// UI
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0, viewport-fit=cover">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <title>IronSight FPV Scout</title>
  <style>
    :root {
      --safe-top: env(safe-area-inset-top, 10px);
      --safe-bottom: env(safe-area-inset-bottom, 10px);
      --safe-left: env(safe-area-inset-left, 10px);
      --safe-right: env(safe-area-inset-right, 10px);
      --hud-bg: rgba(5, 9, 14, 0.58);
      --hud-border: rgba(72, 187, 255, 0.26);
      --hud-text: #e8f6ff;
      --muted: #8da4b8;
      --accent: #38bdf8;
      --ok: #22c55e;
      --bad: #ef4444;
      --panel-shadow: 0 18px 50px rgba(0, 0, 0, 0.38);
      --stick-bg: rgba(8, 15, 25, 0.66);
      --stick-stroke: rgba(255, 255, 255, 0.1);
      --knob-fill: linear-gradient(180deg, #67e8f9, #2563eb);
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    html, body {
      width: 100%;
      height: 100%;
      overflow: hidden;
      font-family: "Segoe UI", "PingFang SC", sans-serif;
      background: #05070a;
      color: var(--hud-text);
      touch-action: none;
      -webkit-user-select: none;
      user-select: none;
      -webkit-tap-highlight-color: transparent;
    }

    body {
      position: fixed;
      inset: 0;
    }

    .video-shell {
      position: absolute;
      inset: 0;
      background:
        radial-gradient(circle at top, rgba(56, 189, 248, 0.2), transparent 30%),
        linear-gradient(180deg, rgba(0, 0, 0, 0.14), rgba(0, 0, 0, 0.55));
    }

    .video-shell::after {
      content: "";
      position: absolute;
      inset: 0;
      background-image:
        linear-gradient(rgba(255, 255, 255, 0.035) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255, 255, 255, 0.03) 1px, transparent 1px);
      background-size: 28px 28px;
      mix-blend-mode: screen;
      opacity: 0.18;
      pointer-events: none;
    }

    #stream {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      object-fit: cover;
      background: #020406;
    }

    .scanline {
      position: absolute;
      inset: 0;
      background: linear-gradient(180deg, transparent, rgba(56, 189, 248, 0.05), transparent);
      animation: sweep 5s linear infinite;
      pointer-events: none;
    }

    @keyframes sweep {
      0% { transform: translateY(-100%); }
      100% { transform: translateY(100%); }
    }

    .hud {
      position: absolute;
      inset: 0;
      padding: calc(var(--safe-top) + 10px) calc(var(--safe-right) + 10px) calc(var(--safe-bottom) + 10px) calc(var(--safe-left) + 10px);
      display: grid;
      grid-template-columns: 1fr auto 1fr;
      grid-template-rows: auto 1fr auto;
      pointer-events: none;
    }

    .topbar {
      grid-column: 1 / 4;
      display: flex;
      justify-content: space-between;
      align-items: flex-start;
      gap: 12px;
      pointer-events: none;
    }

    .hud-card {
      display: flex;
      align-items: center;
      gap: 10px;
      padding: 8px 12px;
      border-radius: 16px;
      background: var(--hud-bg);
      border: 1px solid var(--hud-border);
      backdrop-filter: blur(12px);
      box-shadow: var(--panel-shadow);
    }

    .title-stack {
      display: flex;
      flex-direction: column;
      gap: 2px;
    }

    .eyebrow {
      font-size: 0.62rem;
      color: var(--muted);
      letter-spacing: 0.24em;
      text-transform: uppercase;
    }

    .title {
      font-size: 0.9rem;
      font-weight: 700;
      letter-spacing: 0.08em;
      text-transform: uppercase;
    }

    .status-wrap {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 0.75rem;
      font-weight: 700;
      letter-spacing: 0.08em;
      text-transform: uppercase;
    }

    .dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--bad);
      box-shadow: 0 0 10px var(--bad);
      flex: none;
    }

    .reticle {
      grid-column: 2;
      grid-row: 2;
      place-self: center;
      width: min(32vw, 180px);
      height: min(32vw, 180px);
      border: 1px solid rgba(255, 255, 255, 0.12);
      border-radius: 50%;
      position: relative;
      pointer-events: none;
      background: radial-gradient(circle, rgba(56, 189, 248, 0.07), transparent 65%);
    }

    .reticle::before,
    .reticle::after {
      content: "";
      position: absolute;
      background: rgba(255, 255, 255, 0.18);
    }

    .reticle::before {
      left: 50%;
      top: 12%;
      bottom: 12%;
      width: 1px;
      transform: translateX(-50%);
    }

    .reticle::after {
      top: 50%;
      left: 12%;
      right: 12%;
      height: 1px;
      transform: translateY(-50%);
    }

    .reticle-center {
      position: absolute;
      left: 50%;
      top: 50%;
      width: 18px;
      height: 18px;
      border-radius: 50%;
      border: 2px solid rgba(56, 189, 248, 0.68);
      transform: translate(-50%, -50%);
      box-shadow: 0 0 14px rgba(56, 189, 248, 0.28);
    }

    .reticle-label {
      position: absolute;
      bottom: -28px;
      left: 50%;
      transform: translateX(-50%);
      font-size: 0.68rem;
      letter-spacing: 0.18em;
      text-transform: uppercase;
      color: rgba(232, 246, 255, 0.72);
      white-space: nowrap;
    }

    .control-side {
      grid-row: 3;
      align-self: end;
      display: flex;
      gap: 10px;
      pointer-events: auto;
    }

    .control-left { grid-column: 1; justify-self: start; }
    .control-right { grid-column: 3; justify-self: end; }

    .panel {
      background: var(--hud-bg);
      border: 1px solid var(--hud-border);
      backdrop-filter: blur(12px);
      box-shadow: var(--panel-shadow);
      border-radius: 18px;
      padding: 10px;
    }

    .panel-head {
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 8px;
      font-size: 0.65rem;
      color: var(--muted);
      letter-spacing: 0.14em;
      text-transform: uppercase;
    }

    .sticks {
      display: flex;
      gap: 10px;
    }

    .stick-wrap {
      width: clamp(92px, 18vw, 120px);
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 8px;
    }

    .stick {
      position: relative;
      width: 100%;
      height: clamp(150px, 27vh, 230px);
      background: var(--stick-bg);
      border: 1px solid var(--stick-stroke);
      border-radius: 999px;
      box-shadow: inset 0 6px 20px rgba(0, 0, 0, 0.48);
      overflow: hidden;
      touch-action: none;
    }

    .stick::before {
      content: "";
      position: absolute;
      left: 22%;
      right: 22%;
      top: 50%;
      height: 1px;
      transform: translateY(-50%);
      background: rgba(255, 255, 255, 0.12);
    }

    .stick-knob {
      position: absolute;
      left: 50%;
      top: 50%;
      width: clamp(56px, 11vw, 68px);
      height: clamp(56px, 11vw, 68px);
      transform: translate(-50%, -50%);
      border-radius: 50%;
      background: var(--knob-fill);
      border: 1px solid rgba(255, 255, 255, 0.22);
      box-shadow: 0 10px 20px rgba(0, 0, 0, 0.42), inset 0 3px 6px rgba(255, 255, 255, 0.24);
    }

    .stick-meta {
      width: 100%;
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 0.72rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
    }

    .stick-val {
      color: var(--accent);
      font-weight: 700;
      font-variant-numeric: tabular-nums;
    }

    .gimbal-grid {
      width: clamp(156px, 22vw, 220px);
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      grid-template-rows: repeat(4, auto);
      gap: 8px;
    }

    .btn {
      min-height: 58px;
      border: 1px solid rgba(255, 255, 255, 0.12);
      border-radius: 16px;
      background: rgba(10, 16, 24, 0.72);
      color: var(--hud-text);
      font-size: 0.72rem;
      font-weight: 700;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      display: flex;
      align-items: center;
      justify-content: center;
      touch-action: manipulation;
    }

    .btn:active { background: rgba(24, 35, 49, 0.92); }
    .btn-primary:active { color: var(--accent); border-color: rgba(56, 189, 248, 0.7); }
    .btn-stop {
      background: linear-gradient(180deg, rgba(249, 115, 22, 0.88), rgba(194, 65, 12, 0.96));
      border-color: rgba(253, 186, 116, 0.45);
      color: white;
    }
    .btn-center {
      background: linear-gradient(180deg, rgba(2, 132, 199, 0.84), rgba(3, 105, 161, 0.96));
      border-color: rgba(125, 211, 252, 0.4);
    }

    .metric-stack {
      display: flex;
      gap: 8px;
      align-items: stretch;
      pointer-events: none;
    }

    .metric {
      min-width: 98px;
      display: flex;
      flex-direction: column;
      gap: 3px;
      padding: 8px 10px;
      border-radius: 16px;
      background: rgba(5, 9, 14, 0.44);
      border: 1px solid rgba(255, 255, 255, 0.08);
    }

    .metric-k {
      font-size: 0.62rem;
      color: var(--muted);
      letter-spacing: 0.14em;
      text-transform: uppercase;
    }

    .metric-v {
      font-size: 0.95rem;
      font-weight: 700;
      color: var(--accent);
      font-variant-numeric: tabular-nums;
    }

    .offline {
      position: absolute;
      inset: 0;
      display: flex;
      align-items: center;
      justify-content: center;
      text-align: center;
      padding: 20px;
      color: rgba(232, 246, 255, 0.72);
      font-size: 0.85rem;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      background: rgba(0, 0, 0, 0.36);
      pointer-events: none;
    }

    .offline.hidden { display: none; }

    @media (orientation: portrait) {
      .hud {
        grid-template-columns: 1fr;
        grid-template-rows: auto 1fr auto auto;
      }

      .topbar { grid-column: 1; }
      .reticle { grid-column: 1; grid-row: 2; width: min(44vw, 180px); height: min(44vw, 180px); }
      .control-left,
      .control-right {
        grid-column: 1;
        justify-self: center;
      }
      .control-left { grid-row: 3; }
      .control-right { grid-row: 4; }
    }
  </style>
</head>
<body>
  <div class="video-shell">
    <img id="stream" alt="FPV stream">
    <div class="scanline"></div>
    <div id="streamOffline" class="offline">Video Link Pending</div>
  </div>

  <div class="hud">
    <div class="topbar">
      <div class="hud-card">
        <div class="title-stack">
          <div class="eyebrow">IronSight Vehicle</div>
          <div class="title">FPV Scout Deck</div>
        </div>
      </div>

      <div class="metric-stack">
        <div class="metric">
          <div class="metric-k">Left Track</div>
          <div id="leftMetric" class="metric-v">0%</div>
        </div>
        <div class="metric">
          <div class="metric-k">Right Track</div>
          <div id="rightMetric" class="metric-v">0%</div>
        </div>
      </div>

      <div class="hud-card">
        <div class="status-wrap"><span id="statusDot" class="dot"></span><span id="linkLabel">Link Lost</span></div>
      </div>
    </div>

    <div class="reticle">
      <div class="reticle-center"></div>
      <div class="reticle-label">Local Recon Feed</div>
    </div>

    <div class="control-side control-left">
      <div class="panel">
        <div class="panel-head"><span>Mobility</span><span>Tracks</span></div>
        <div class="sticks">
          <div class="stick-wrap">
            <div id="stickL" class="stick"><div id="knobL" class="stick-knob"></div></div>
            <div class="stick-meta"><span>L-Track</span><span id="valL" class="stick-val">0%</span></div>
          </div>
          <div class="stick-wrap">
            <div id="stickR" class="stick"><div id="knobR" class="stick-knob"></div></div>
            <div class="stick-meta"><span>R-Track</span><span id="valR" class="stick-val">0%</span></div>
          </div>
        </div>
      </div>
    </div>

    <div class="control-side control-right">
      <div class="panel">
        <div class="panel-head"><span>Camera Gimbal</span><span>Pan / Tilt</span></div>
        <div class="gimbal-grid">
          <button class="btn btn-primary" style="grid-column:2;grid-row:1" id="tilt_up">Tilt Up</button>
          <button class="btn btn-primary" style="grid-column:1;grid-row:2" id="pan_left">Pan Left</button>
          <button class="btn btn-center" style="grid-column:2;grid-row:2" id="center_view">Center</button>
          <button class="btn btn-primary" style="grid-column:3;grid-row:2" id="pan_right">Pan Right</button>
          <button class="btn btn-primary" style="grid-column:2;grid-row:3" id="tilt_down">Tilt Down</button>
          <button class="btn btn-stop" style="grid-column:1 / 4;grid-row:4" id="stop">E-Stop</button>
        </div>
      </div>
    </div>
  </div>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());
    document.addEventListener('touchmove', e => {
      if (e.touches && e.touches.length > 1) e.preventDefault();
    }, { passive: false });

    const wsUrl = 'ws://' + location.hostname + '/ws';
    const streamUrl = 'http://' + location.hostname + ':81/stream';
    const supportsPointerEvents = 'PointerEvent' in window;
    let ws = null;
    let reconnectTimer = null;
    let reconnectAttempt = 0;
    let streamRetryTimer = null;
    let L = 0;
    let R = 0;
    let lastSent = '';

    const statusDot = document.getElementById('statusDot');
    const linkLabel = document.getElementById('linkLabel');
    const streamImg = document.getElementById('stream');
    const streamOffline = document.getElementById('streamOffline');
    const leftMetric = document.getElementById('leftMetric');
    const rightMetric = document.getElementById('rightMetric');

    function setLinkState(connected) {
      if (linkLabel) linkLabel.textContent = connected ? 'Link Active' : 'Link Lost';
      if (statusDot) {
        statusDot.style.backgroundColor = connected ? 'var(--ok)' : 'var(--bad)';
        statusDot.style.boxShadow = connected ? '0 0 10px var(--ok)' : '0 0 10px var(--bad)';
      }
    }

    function refreshVideo() {
      if (streamRetryTimer) {
        clearTimeout(streamRetryTimer);
        streamRetryTimer = null;
      }
      if (streamOffline) streamOffline.classList.remove('hidden');
      if (streamImg) {
        const nonce = Date.now();
        streamImg.src = streamUrl + '?t=' + nonce;
      }
    }

    function scheduleVideoRetry() {
      if (streamRetryTimer) clearTimeout(streamRetryTimer);
      streamRetryTimer = setTimeout(refreshVideo, 1200);
    }

    function connect() {
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
      ws = new WebSocket(wsUrl);
      ws.onopen = () => {
        if (reconnectTimer) {
          clearTimeout(reconnectTimer);
          reconnectTimer = null;
        }
        reconnectAttempt = 0;
        lastSent = '';
        setLinkState(true);
        sendTracks(true);
      };
      ws.onclose = () => {
        setLinkState(false);
        scheduleReconnect();
      };
      ws.onerror = () => {
        setLinkState(false);
        try { ws.close(); } catch (_) {}
      };
    }

    function scheduleReconnect() {
      if (reconnectTimer) clearTimeout(reconnectTimer);
      reconnectAttempt = Math.min(reconnectAttempt + 1, 6);
      const delay = Math.min(1000 * reconnectAttempt, 4000);
      reconnectTimer = setTimeout(connect, delay);
    }

    function send(msg) {
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(msg);
    }

    function sendTracks(force) {
      const msg = 'tracks:' + L + ':' + R;
      if (force || msg !== lastSent) {
        send(msg);
        lastSent = msg;
      }
    }

    function clamp(v, a, b) { return v < a ? a : (v > b ? b : v); }

    function draw(side, val) {
      const stick = document.getElementById(side === 'L' ? 'stickL' : 'stickR');
      const knob = document.getElementById(side === 'L' ? 'knobL' : 'knobR');
      const txt = document.getElementById(side === 'L' ? 'valL' : 'valR');
      if (!stick || !knob || !txt) return;

      const h = stick.clientHeight;
      const kh = knob.clientHeight;
      const mid = (h - kh) / 2;
      const yOffset = -(val / 100) * mid;
      knob.style.top = 'calc(50% + ' + yOffset + 'px)';
      txt.textContent = val + '%';

      if (side === 'L' && leftMetric) leftMetric.textContent = val + '%';
      if (side === 'R' && rightMetric) rightMetric.textContent = val + '%';
    }

    function bindStick(id, side) {
      const stick = document.getElementById(id);
      if (!stick) return;

      let pointerId = null;
      let touchId = null;

      function setByY(clientY) {
        const r = stick.getBoundingClientRect();
        const knob = document.getElementById(side === 'L' ? 'knobL' : 'knobR');
        const maxDist = (r.height - (knob ? knob.clientHeight : 0)) / 2;
        if (maxDist <= 0) return;

        const center = r.top + r.height / 2;
        const dist = center - clientY;
        let p = Math.round((dist / maxDist) * 100);
        p = clamp(p, -100, 100);

        if (side === 'L') L = p; else R = p;
        draw(side, p);
        sendTracks(false);
      }

      function resetStick() {
        if (side === 'L') L = 0; else R = 0;
        draw(side, 0);
        sendTracks(true);
      }

      function findTrackedTouch(touchList) {
        if (touchId === null || !touchList) return null;
        for (const touch of touchList) {
          if (touch.identifier === touchId) return touch;
        }
        return null;
      }

      if (supportsPointerEvents) {
        stick.addEventListener('pointerdown', e => {
          e.preventDefault();
          pointerId = e.pointerId;
          if (stick.setPointerCapture) {
            try { stick.setPointerCapture(e.pointerId); } catch (_) {}
          }
          setByY(e.clientY);
        });
        stick.addEventListener('pointermove', e => {
          if (pointerId !== e.pointerId) return;
          setByY(e.clientY);
        });
        stick.addEventListener('pointerup', e => {
          if (pointerId !== e.pointerId) return;
          pointerId = null;
          resetStick();
        });
        stick.addEventListener('pointercancel', e => {
          if (pointerId !== e.pointerId) return;
          pointerId = null;
          resetStick();
        });
        stick.addEventListener('lostpointercapture', () => {
          pointerId = null;
          resetStick();
        });
        return;
      }

      stick.addEventListener('touchstart', e => {
        if (touchId !== null || !e.changedTouches || !e.changedTouches[0]) return;
        e.preventDefault();
        touchId = e.changedTouches[0].identifier;
        setByY(e.changedTouches[0].clientY);
      }, { passive: false });
      stick.addEventListener('touchmove', e => {
        const touch = findTrackedTouch(e.touches);
        if (!touch) return;
        e.preventDefault();
        setByY(touch.clientY);
      }, { passive: false });
      stick.addEventListener('touchend', e => {
        const touch = findTrackedTouch(e.changedTouches);
        if (!touch) return;
        e.preventDefault();
        touchId = null;
        resetStick();
      }, { passive: false });
      stick.addEventListener('touchcancel', e => {
        const touch = findTrackedTouch(e.changedTouches);
        if (!touch) return;
        e.preventDefault();
        touchId = null;
        resetStick();
      }, { passive: false });
    }

    function bindHoldButton(id, startCmd, stopCmd) {
      const el = document.getElementById(id);
      if (!el) return;

      const start = e => { if (e) e.preventDefault(); send(startCmd); };
      const stop = e => { if (e) e.preventDefault(); send(stopCmd); };

      if (supportsPointerEvents) {
        el.addEventListener('pointerdown', start);
        el.addEventListener('pointerup', stop);
        el.addEventListener('pointercancel', stop);
        el.addEventListener('lostpointercapture', stop);
        el.addEventListener('pointerleave', e => { if (e.buttons === 1) stop(e); });
        return;
      }

      el.addEventListener('touchstart', start, { passive: false });
      el.addEventListener('touchend', stop, { passive: false });
      el.addEventListener('touchcancel', stop, { passive: false });
      el.addEventListener('mousedown', start);
      el.addEventListener('mouseup', stop);
      el.addEventListener('mouseleave', e => { if (e.buttons === 1) stop(e); });
    }

    function bindSinglePress(id, command) {
      const el = document.getElementById(id);
      if (!el) return;
      const fire = e => { if (e) e.preventDefault(); send(command); };
      if (supportsPointerEvents) {
        el.addEventListener('pointerdown', fire);
        return;
      }
      el.addEventListener('touchstart', fire, { passive: false });
      el.addEventListener('mousedown', fire);
    }

    function resetAll() {
      L = 0;
      R = 0;
      draw('L', 0);
      draw('R', 0);
      sendTracks(true);
      send('stop');
      send('pan_stop');
      send('tilt_stop');
    }

    document.addEventListener('DOMContentLoaded', () => {
      setLinkState(false);
      draw('L', 0);
      draw('R', 0);
      bindStick('stickL', 'L');
      bindStick('stickR', 'R');

      bindHoldButton('pan_left', 'pan_left_start', 'pan_stop');
      bindHoldButton('pan_right', 'pan_right_start', 'pan_stop');
      bindHoldButton('tilt_up', 'tilt_up_start', 'tilt_stop');
      bindHoldButton('tilt_down', 'tilt_down_start', 'tilt_stop');
      bindSinglePress('center_view', 'center_view');
      bindSinglePress('stop', 'stop');

      connect();
      refreshVideo();

      setInterval(() => sendTracks(true), 120);
      window.addEventListener('blur', resetAll);
      document.addEventListener('visibilitychange', () => { if (document.hidden) resetAll(); });
      window.addEventListener('pagehide', resetAll);

      if (streamImg) {
        streamImg.addEventListener('load', () => {
          if (streamOffline) streamOffline.classList.add('hidden');
        });
        streamImg.addEventListener('error', () => {
          if (streamOffline) streamOffline.classList.remove('hidden');
          scheduleVideoRetry();
        });
      }
    });
  </script>
</body>
</html>
)rawliteral";

static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=frame";
static const char *STREAM_BOUNDARY = "\r\n--frame\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

void motorA_Move(int16_t speed) {
  speed = constrain(speed, -PWM_MAX, PWM_MAX);
  if (speed > 0) {
    digitalWrite(AIN1, HIGH);
    digitalWrite(AIN2, LOW);
  } else if (speed < 0) {
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, HIGH);
  } else {
    digitalWrite(AIN1, LOW);
    digitalWrite(AIN2, LOW);
  }
  ledcWrite(PWMA, abs(speed));
}

void motorB_Move(int16_t speed) {
  speed = constrain(speed, -PWM_MAX, PWM_MAX);
  if (speed > 0) {
    digitalWrite(BIN1, HIGH);
    digitalWrite(BIN2, LOW);
  } else if (speed < 0) {
    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, HIGH);
  } else {
    digitalWrite(BIN1, LOW);
    digitalWrite(BIN2, LOW);
  }
  ledcWrite(PWMB, abs(speed));
}

void motorA_Brake() {
  digitalWrite(AIN1, HIGH);
  digitalWrite(AIN2, HIGH);
  ledcWrite(PWMA, 0);
}

void motorB_Brake() {
  digitalWrite(BIN1, HIGH);
  digitalWrite(BIN2, HIGH);
  ledcWrite(PWMB, 0);
}

void stopAllMotion() {
  motorA_Brake();
  motorB_Brake();
  leftTargetPwm = 0;
  rightTargetPwm = 0;
  leftCurrentPwm = 0;
  rightCurrentPwm = 0;
  panMoveDir = 0;
  tiltMoveDir = 0;
}

void centerView() {
  panMoveDir = 0;
  tiltMoveDir = 0;
  panAngle = 90;
  tiltAngle = 95;
  panServo.write(panAngle);
  tiltServo.write(tiltAngle);
}

int16_t mapTrackPercentToPwm(int16_t pct) {
  pct = constrain(pct, -100, 100);
  int16_t sign = (pct < 0) ? -1 : 1;
  int16_t ap = abs(pct);

  if (ap <= JOYSTICK_DEADZONE) {
    return 0;
  }

  int16_t linear = (int16_t)(((int32_t)(ap - JOYSTICK_DEADZONE) * 100) /
                             (100 - JOYSTICK_DEADZONE));
  int16_t curved = (int16_t)(((int32_t)linear * linear) / 100);
  int16_t pwm = MOTOR_MIN_EFFECTIVE_PWM +
                (int16_t)(((int32_t)(PWM_MAX - MOTOR_MIN_EFFECTIVE_PWM) * curved) / 100);
  pwm = constrain(pwm, 0, PWM_MAX);
  return sign * pwm;
}

int16_t approachWithStep(int16_t current, int16_t target, uint8_t step) {
  if (current < target) {
    int16_t next = current + step;
    return (next > target) ? target : next;
  }
  if (current > target) {
    int16_t next = current - step;
    return (next < target) ? target : next;
  }
  return current;
}

int16_t applyTrackGain(int16_t pwm, uint8_t gainPercent) {
  int16_t sign = (pwm < 0) ? -1 : 1;
  int32_t scaled = ((int32_t)abs(pwm) * gainPercent) / 100;
  scaled = constrain(scaled, 0, PWM_MAX);
  return sign * (int16_t)scaled;
}

void applyTrackOutput() {
  const uint32_t now = millis();
  if (now - lastDriveUpdateMs < DRIVE_UPDATE_INTERVAL_MS) {
    return;
  }
  lastDriveUpdateMs = now;

  if (now - lastTrackCmdMs > TRACK_SIGNAL_TIMEOUT_MS) {
    leftTargetPwm = 0;
    rightTargetPwm = 0;
  }

  leftCurrentPwm = approachWithStep(leftCurrentPwm, leftTargetPwm, TRACK_SLEW_STEP);
  rightCurrentPwm = approachWithStep(rightCurrentPwm, rightTargetPwm, TRACK_SLEW_STEP);

  motorA_Move(leftCurrentPwm);
  motorB_Move(rightCurrentPwm);
}

void applyServoMotion() {
  const uint32_t now = millis();
  if (now - lastServoUpdateMs < SERVO_UPDATE_INTERVAL_MS) {
    return;
  }
  lastServoUpdateMs = now;

  if (panMoveDir != 0) {
    panAngle = constrain(panAngle + panMoveDir * PAN_STEP_PER_TICK, PAN_MIN, PAN_MAX);
    panServo.write(panAngle);
  }

  if (tiltMoveDir != 0) {
    tiltAngle = constrain(tiltAngle + tiltMoveDir * TILT_STEP_PER_TICK, TILT_MIN, TILT_MAX);
    tiltServo.write(tiltAngle);
  }
}

bool parseTrackCommand(char *payload) {
  char *mid = strchr(payload, ':');
  if (mid == nullptr) {
    return false;
  }

  *mid = '\0';
  char *end1 = nullptr;
  char *end2 = nullptr;
  long l = strtol(payload, &end1, 10);
  long r = strtol(mid + 1, &end2, 10);

  if (end1 == nullptr || *end1 != '\0' || end2 == nullptr || *end2 != '\0') {
    return false;
  }

  int16_t lPct = (int16_t)constrain((int)l, -100, 100);
  int16_t rPct = (int16_t)constrain((int)r, -100, 100);

  leftTargetPwm = applyTrackGain(mapTrackPercentToPwm(lPct), LEFT_TRACK_GAIN_PERCENT);
  rightTargetPwm = applyTrackGain(mapTrackPercentToPwm(rPct), RIGHT_TRACK_GAIN_PERCENT);
  lastTrackCmdMs = millis();
  return true;
}

esp_err_t jpg_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = nullptr;
  esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  char part_buf[64];

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      LOG_PRINTLN(F("Camera frame capture failed"));
      res = ESP_FAIL;
    } else {
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) {
        size_t hlen = (size_t)snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      }
      esp_camera_fb_return(fb);
      fb = nullptr;
    }

    if (res != ESP_OK) {
      break;
    }
  }

  if (fb) {
    esp_camera_fb_return(fb);
  }
  return res;
}

void startCameraStreamServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;
  config.ctrl_port = 32768;
  config.max_open_sockets = 2;
  config.stack_size = 8192;

  httpd_uri_t stream_uri = {
      .uri = "/stream",
      .method = HTTP_GET,
      .handler = stream_handler,
      .user_ctx = nullptr,
  };

  httpd_uri_t jpg_uri = {
      .uri = "/capture.jpg",
      .method = HTTP_GET,
      .handler = jpg_handler,
      .user_ctx = nullptr,
  };

  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &jpg_uri);
    LOG_PRINTLN(F("Camera stream server started on :81"));
  } else {
    LOG_PRINTLN(F("Failed to start camera stream server"));
  }
}

bool initCamera() {
  camera_config_t config;
  memset(&config, 0, sizeof(config));
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.frame_size = CAMERA_FRAME_SIZE;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = CAMERA_JPEG_QUALITY;
  config.fb_count = psramFound() ? CAMERA_FB_COUNT : 1;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    LOG_PRINTF("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *sensor = esp_camera_sensor_get();
  if (sensor != nullptr) {
    sensor->set_vflip(sensor, 0);
    sensor->set_hmirror(sensor, 0);
    sensor->set_brightness(sensor, 0);
    sensor->set_saturation(sensor, 0);
  }

  LOG_PRINTLN(F("Camera initialized"));
  return true;
}

void onEvent(AsyncWebSocket *serverRef, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
  (void)serverRef;

  switch (type) {
  case WS_EVT_CONNECT:
    LOG_PRINTF("WebSocket client #%u connected from %s\n", client->id(),
               client->remoteIP().toString().c_str());
    break;

  case WS_EVT_DISCONNECT:
    LOG_PRINTF("WebSocket client #%u disconnected\n", client->id());
    stopAllMotion();
    break;

  case WS_EVT_DATA: {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (!(info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT)) {
      return;
    }

    if (len == 0 || len >= 48) {
      return;
    }

    char msg[48];
    memcpy(msg, data, len);
    msg[len] = '\0';

    if (strncmp(msg, "tracks:", 7) == 0) {
      parseTrackCommand(msg + 7);
      break;
    }

    if (strcmp(msg, "stop") == 0) {
      stopAllMotion();
    } else if (strcmp(msg, "center_view") == 0) {
      centerView();
    } else if (strcmp(msg, "pan_left_start") == 0 || strcmp(msg, "turret_left_start") == 0) {
      panMoveDir = +1;
    } else if (strcmp(msg, "pan_right_start") == 0 || strcmp(msg, "turret_right_start") == 0) {
      panMoveDir = -1;
    } else if (strcmp(msg, "pan_stop") == 0 || strcmp(msg, "turret_stop") == 0) {
      panMoveDir = 0;
    } else if (strcmp(msg, "tilt_up_start") == 0 || strcmp(msg, "barrel_up_start") == 0) {
      tiltMoveDir = -1;
    } else if (strcmp(msg, "tilt_down_start") == 0 || strcmp(msg, "barrel_down_start") == 0) {
      tiltMoveDir = +1;
    } else if (strcmp(msg, "tilt_stop") == 0 || strcmp(msg, "barrel_stop") == 0) {
      tiltMoveDir = 0;
    }
    break;
  }

  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

void initWiFiAP() {
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);

  if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
    LOG_PRINTLN(F("Failed to set AP IP address"));
    return;
  }

  if (!WiFi.softAP(ap_ssid, ap_password)) {
    LOG_PRINTLN(F("AP failed to start"));
    return;
  }

  LOG_PRINTLN(F("Access Point started"));
  IPAddress ip = WiFi.softAPIP();
  LOG_PRINTF("IP Address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
}

void setup() {
  Serial.begin(115200);

  panServo.attach(PAN_SERVO_PIN, 500, 2400);
  tiltServo.attach(TILT_SERVO_PIN, 500, 2400);
  centerView();

  if (ENABLE_SERVO_SELF_TEST) {
    delay(700);
    panServo.write(PAN_MIN);
    tiltServo.write(TILT_MIN);
    delay(450);
    panServo.write(PAN_MAX);
    tiltServo.write(TILT_MAX);
    delay(450);
    centerView();
  }

  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);

  ledcAttach(PWMA, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(PWMB, PWM_FREQ, PWM_RESOLUTION);
  stopAllMotion();

  initWiFiAP();
  initCamera();
  startCameraStreamServer();

  ws.onEvent(onEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });
  server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
  LOG_PRINTLN(F("HTTP + WebSocket server started"));
}

void loop() {
  ws.cleanupClients();
  applyTrackOutput();
  applyServoMotion();
}
