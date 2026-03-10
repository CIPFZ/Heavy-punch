/*
 * WiFi Motion Core (ESP32 Tank Control)
 * Board: ESP32-S3
 * Libraries: ESPAsyncWebServer, AsyncTCP, ESP32Servo
 *
 * 架构说明:
 * 1) ESP32 以 AP 模式提供热点 + HTTP 页面。
 * 2) 页面通过 WebSocket 发送文本命令。
 * 3) 采用双摇杆控制左右履带：tracks:<left_pct>:<right_pct>，范围 -100..100。
 * 4) 固件做死区/曲线/起步补偿/斜坡处理后输出 PWM。
 * 5) 开火动作使用 loop() 中非阻塞状态机复位。
 */

#include <AsyncTCP.h>
#include <ESP32Servo.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <stdlib.h>
#include <string.h>

// ========== Build-time options ==========
// 设为 0 可关闭串口调试日志（默认关闭，减少输出和干扰）
#define ENABLE_DEBUG_LOG 0
// 设为 0 可关闭上电舵机自检，缩短启动时间
#define ENABLE_SERVO_SELF_TEST 1

#if ENABLE_DEBUG_LOG
#define LOG_PRINTLN(x) Serial.println(x)
#define LOG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define LOG_PRINTLN(x) ((void)0)
#define LOG_PRINTF(...) ((void)0)
#endif

// Wi-Fi AP 配置
const char *ap_ssid = "ESP32_WiFi_Motion_Control";
const char *ap_password = "12345678";
IPAddress local_IP(192, 168, 1, 4);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// HTTP 服务 + WebSocket 长连接
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// 电机驱动引脚（A=左履带，B=右履带）
#define PWMA 5
#define AIN2 6
#define AIN1 7
#define PWMB 18
#define BIN2 17
#define BIN1 16

// 舵机引脚（炮塔旋转 / 炮管俯仰 / 开火舵机）
#define TURRET_SERVO_PIN 13
#define BARREL_SERVO_PIN 12
#define FIRE_SERVO_PIN 11

// PWM 配置
const uint16_t PWM_FREQ = 5000;
const uint8_t PWM_RESOLUTION = 8;
const int16_t PWM_MAX = 255;

// 摇杆到速度映射参数（你后续主要调这组）
const int16_t JOYSTICK_DEADZONE = 8;        // 摇杆死区：防抖
const int16_t MOTOR_MIN_EFFECTIVE_PWM = 85; // 电机起转补偿
const uint8_t TRACK_SLEW_STEP = 12;         // 每次更新最大 PWM 变化，越小越平滑
const uint16_t DRIVE_UPDATE_INTERVAL_MS = 15;
const uint16_t TRACK_SIGNAL_TIMEOUT_MS = 350; // 摇杆信号超时自动停车

// 舵机对象
Servo turretServo;
Servo barrelServo;
Servo fireServo;

// 舵机状态和角度限制
int16_t turretAngle = 90;
int16_t barrelAngle = 90;
const int16_t TURRET_MIN = 10;
const int16_t TURRET_MAX = 170;
const int16_t BARREL_MIN = 70;
const int16_t BARREL_MAX = 130;
const int16_t FIRE_READY_POS = 90;
const int16_t FIRE_TRIGGER_POS = 130;
const uint16_t SERVO_UPDATE_INTERVAL_MS = 20;
const int16_t TURRET_STEP_PER_TICK = 2;
const int16_t BARREL_STEP_PER_TICK = 1;

// 非阻塞开火状态
bool isFiring = false;
uint32_t fireStartTime = 0;
const uint32_t fireDuration = 500;

// 履带目标/当前 PWM（带斜坡）
int16_t leftTargetPwm = 0;
int16_t rightTargetPwm = 0;
int16_t leftCurrentPwm = 0;
int16_t rightCurrentPwm = 0;
uint32_t lastTrackCmdMs = 0;
uint32_t lastDriveUpdateMs = 0;
int8_t turretMoveDir = 0; // -1: right, +1: left
int8_t barrelMoveDir = 0; // -1: up, +1: down
uint32_t lastServoUpdateMs = 0;

// 控制页面（存储在 Flash，减少 RAM 占用）
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0, viewport-fit=cover">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <title>Tank Command</title>
  <style>
    :root {
      --bg-dark: #050505;
      --panel-bg: rgba(15, 23, 42, 0.65);
      --panel-border: rgba(56, 189, 248, 0.2);
      --panel-shadow: 0 4px 16px rgba(0, 0, 0, 0.4);
      --text-main: #f8fafc;
      --text-muted: #94a3b8;
      --accent-cyan: #22d3ee;
      --accent-red: #ef4444;
      --accent-orange: #f97316;
      --accent-green: #10b981;
      --stick-bg: #0f172a;
      --stick-border: #1e293b;
      --knob-gradient: linear-gradient(135deg, #38bdf8, #2563eb);
      --safe-top: env(safe-area-inset-top, 10px);
      --safe-bottom: env(safe-area-inset-bottom, 10px);
      --safe-left: env(safe-area-inset-left, 10px);
      --safe-right: env(safe-area-inset-right, 10px);
    }

    * { box-sizing: border-box; margin: 0; padding: 0; }

    html, body {
      width: 100vw;
      height: 100vh;
      overflow: hidden;
      position: fixed;
      font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
      background: var(--bg-dark);
      background-image:
        radial-gradient(circle at 10% 50%, rgba(56, 189, 248, 0.08), transparent 30%),
        radial-gradient(circle at 90% 50%, rgba(59, 130, 246, 0.08), transparent 30%);
      color: var(--text-main);
      -webkit-user-select: none;
      user-select: none;
      -webkit-touch-callout: none;
      -webkit-tap-highlight-color: transparent;
      touch-action: none;
    }

    .glass {
      background: var(--panel-bg);
      backdrop-filter: blur(8px);
      -webkit-backdrop-filter: blur(8px);
      border: 1px solid var(--panel-border);
      box-shadow: var(--panel-shadow);
      border-radius: 12px;
    }

    .hud {
      position: absolute;
      top: var(--safe-top);
      left: var(--safe-left);
      right: var(--safe-right);
      z-index: 10;
      display: flex;
      justify-content: space-between;
      align-items: center;
      padding: 8px 12px;
      pointer-events: none;
    }

    .hud-pill {
      background: rgba(0, 0, 0, 0.5);
      border: 1px solid rgba(56, 189, 248, 0.3);
      padding: 4px 10px;
      border-radius: 20px;
      font-size: 0.65rem;
      font-weight: 700;
      letter-spacing: 0.05em;
      color: var(--accent-cyan);
      display: flex;
      align-items: center;
      gap: 6px;
      backdrop-filter: blur(4px);
    }

    .status-dot {
      width: 6px;
      height: 6px;
      border-radius: 50%;
      background-color: var(--accent-red);
      box-shadow: 0 0 6px var(--accent-red);
    }

    .main-container {
      display: flex;
      width: 100%;
      height: 100%;
      padding: calc(var(--safe-top) + 36px) var(--safe-right) var(--safe-bottom) var(--safe-left);
      gap: 12px;
    }

    .panel {
      flex: 1;
      display: flex;
      flex-direction: column;
      padding: 12px;
      position: relative;
      overflow: hidden;
    }

    .panel::before {
      content: '';
      position: absolute;
      inset: 0;
      background-image:
        linear-gradient(rgba(255, 255, 255, 0.02) 1px, transparent 1px),
        linear-gradient(90deg, rgba(255, 255, 255, 0.02) 1px, transparent 1px);
      background-size: 15px 15px;
      pointer-events: none;
      z-index: 0;
    }

    .panel-header {
      position: relative;
      z-index: 1;
      margin-bottom: 8px;
      display: flex;
      align-items: center;
      gap: 6px;
    }

    .panel-header::before {
      content: '';
      width: 3px;
      height: 12px;
      background: var(--accent-cyan);
      border-radius: 2px;
    }

    .panel-title {
      font-size: 0.85rem;
      font-weight: 700;
      color: var(--accent-cyan);
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }

    .drive-section { flex: 1.1; }
    .weapon-section { flex: 0.9; }

    .sticks-container {
      display: flex;
      justify-content: space-around;
      align-items: center;
      flex: 1;
      z-index: 1;
      position: relative;
    }

    .stick-wrapper {
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 8px;
      width: 45%;
      height: 100%;
      justify-content: center;
    }

    .stick-label {
      font-size: 0.65rem;
      color: var(--text-muted);
      font-weight: 700;
      letter-spacing: 0.05em;
    }

    .stick-body {
      position: relative;
      width: clamp(50px, 12vw, 70px);
      height: clamp(140px, 50vh, 200px);
      background: var(--stick-bg);
      border: 1px solid var(--stick-border);
      border-radius: 35px;
      box-shadow: inset 0 6px 15px rgba(0, 0, 0, 0.6);
      touch-action: none;
    }

    .stick-center-line {
      position: absolute;
      left: 20%;
      right: 20%;
      top: 50%;
      transform: translateY(-50%);
      height: 2px;
      background: rgba(255, 255, 255, 0.1);
      border-radius: 1px;
    }

    .stick-knob {
      position: absolute;
      left: 50%;
      top: 50%;
      transform: translate(-50%, -50%);
      width: clamp(40px, 10vw, 56px);
      height: clamp(40px, 10vw, 56px);
      background: var(--knob-gradient);
      border-radius: 50%;
      box-shadow: 0 4px 10px rgba(0, 0, 0, 0.5), inset 0 2px 4px rgba(255, 255, 255, 0.2);
      border: 1px solid rgba(255, 255, 255, 0.1);
    }

    .stick-readout {
      display: flex;
      flex-direction: column;
      align-items: center;
      width: 80%;
      gap: 4px;
    }

    .stick-value {
      font-size: 0.9rem;
      font-weight: 700;
      font-variant-numeric: tabular-nums;
      color: var(--accent-cyan);
    }

    .meter-bg {
      width: 100%;
      height: 4px;
      background: rgba(0, 0, 0, 0.6);
      border-radius: 2px;
      position: relative;
    }

    .meter-fill {
      position: absolute;
      left: 50%;
      top: 0;
      bottom: 0;
      width: 0%;
      background: var(--accent-green);
      transition: width 0.08s ease, left 0.08s ease, background-color 0.08s ease;
    }

    .controls-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      grid-template-rows: repeat(2, 1fr);
      gap: 8px;
      height: 100%;
      flex: 1;
      z-index: 1;
      position: relative;
    }

    .btn {
      background: rgba(30, 41, 59, 0.7);
      border: 1px solid rgba(255, 255, 255, 0.1);
      border-radius: 10px;
      color: var(--text-main);
      font-size: 0.7rem;
      font-weight: 600;
      display: flex;
      flex-direction: column;
      align-items: center;
      justify-content: center;
      gap: 4px;
      touch-action: manipulation;
      position: relative;
      overflow: hidden;
    }

    .btn:active { background: rgba(51, 65, 85, 0.9); }
    .btn-turret:active, .btn-barrel:active { color: var(--accent-cyan); border-color: var(--accent-cyan); }

    .btn-icon { font-size: 1.1rem; }

    .btn-fire {
      background: linear-gradient(135deg, rgba(220, 38, 38, 0.8), rgba(153, 27, 27, 0.9));
      border-color: rgba(248, 113, 113, 0.4);
      grid-row: span 2;
    }

    .btn-fire .btn-icon { font-size: 1.8rem; color: #fca5a5; }

    .btn-stop {
      grid-column: span 3;
      background: linear-gradient(135deg, rgba(234, 88, 12, 0.8), rgba(194, 65, 12, 0.9));
      border-color: rgba(253, 186, 116, 0.4);
      font-size: 0.85rem;
      letter-spacing: 0.05em;
      padding: 4px;
    }

    @media (orientation: portrait) {
      .main-container { flex-direction: column; padding-top: calc(var(--safe-top) + 44px); }
      .drive-section, .weapon-section { flex: none; height: 45vh; }
      .weapon-section { height: 35vh; }
    }
  </style>
</head>
<body>
  <div class="hud">
    <div class="hud-pill"><span id="statusDot" class="status-dot"></span><span id="linkLabel">LINK LOST</span></div>
    <div class="hud-pill" style="color: var(--text-muted); border-color: rgba(255,255,255,0.1);">ESP32</div>
  </div>

  <div class="main-container">
    <div class="panel glass drive-section">
      <div class="panel-header"><div class="panel-title">Mobility Drive</div></div>
      <div class="sticks-container">
        <div class="stick-wrapper">
          <div class="stick-label">L-TRACK</div>
          <div id="stickL" class="stick-body"><div class="stick-center-line"></div><div id="knobL" class="stick-knob"></div></div>
          <div class="stick-readout"><div id="valL" class="stick-value">0%</div><div class="meter-bg"><div id="fillL" class="meter-fill"></div></div></div>
        </div>
        <div class="stick-wrapper">
          <div class="stick-label">R-TRACK</div>
          <div id="stickR" class="stick-body"><div class="stick-center-line"></div><div id="knobR" class="stick-knob"></div></div>
          <div class="stick-readout"><div id="valR" class="stick-value">0%</div><div class="meter-bg"><div id="fillR" class="meter-fill"></div></div></div>
        </div>
      </div>
    </div>

    <div class="panel glass weapon-section">
      <div class="panel-header"><div class="panel-title">Tactical</div></div>
      <div class="controls-grid">
        <button class="btn btn-turret" id="turret_left"><span class="btn-icon">&#x21BA;</span><span>TURRET L</span></button>
        <button class="btn btn-barrel" id="barrel_up"><span class="btn-icon">&#x2191;</span><span>ELEVATE</span></button>
        <button class="btn btn-fire" id="fire"><span class="btn-icon">&#x1F4A5;</span><span style="font-size:0.8rem">FIRE</span></button>
        <button class="btn btn-turret" id="turret_right"><span class="btn-icon">&#x21BB;</span><span>TURRET R</span></button>
        <button class="btn btn-barrel" id="barrel_down"><span class="btn-icon">&#x2193;</span><span>DEPRESS</span></button>
        <button class="btn btn-stop" id="stop">&#x26A0; E-STOP</button>
      </div>
    </div>
  </div>

  <script>
    document.addEventListener('contextmenu', e => e.preventDefault());
    document.addEventListener('touchmove', e => {
      if (e.touches && e.touches.length > 1) e.preventDefault();
    }, { passive: false });

    const wsUrl = 'ws://' + location.hostname + '/ws';
    let ws = null;
    let reconnectTimer = null;
    let L = 0, R = 0, lastSent = '';

    const statusDot = document.getElementById('statusDot');
    const linkLabel = document.getElementById('linkLabel');

    function setLinkState(connected) {
      if (linkLabel) linkLabel.textContent = connected ? 'LINK ACTIVE' : 'LINK LOST';
      if (statusDot) {
        statusDot.style.backgroundColor = connected ? 'var(--accent-green)' : 'var(--accent-red)';
        statusDot.style.boxShadow = connected ? '0 0 6px var(--accent-green)' : '0 0 6px var(--accent-red)';
      }
    }

    function connect() {
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
      ws = new WebSocket(wsUrl);
      ws.onopen = () => setLinkState(true);
      ws.onclose = () => {
        setLinkState(false);
        if (reconnectTimer) clearTimeout(reconnectTimer);
        reconnectTimer = setTimeout(connect, 1200);
      };
      ws.onerror = () => setLinkState(false);
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
      const fill = document.getElementById(side === 'L' ? 'fillL' : 'fillR');
      if (!stick || !knob || !txt || !fill) return;

      const h = stick.clientHeight;
      const kh = knob.clientHeight;
      const mid = (h - kh) / 2;
      const yOffset = -(val / 100) * mid;

      knob.style.top = 'calc(50% + ' + yOffset + 'px)';
      txt.textContent = val + '%';

      const absVal = Math.abs(val);
      fill.style.width = (absVal / 2) + '%';
      if (val >= 0) {
        fill.style.left = '50%';
        fill.style.backgroundColor = 'var(--accent-green)';
      } else {
        fill.style.left = (50 - (absVal / 2)) + '%';
        fill.style.backgroundColor = 'var(--accent-orange)';
      }
    }

    function bindStick(id, side) {
      const stick = document.getElementById(id);
      if (!stick) return;

      let dragging = false;

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

      // Pointer path
      stick.addEventListener('pointerdown', e => {
        dragging = true;
        if (stick.setPointerCapture) {
          try { stick.setPointerCapture(e.pointerId); } catch (_) {}
        }
        setByY(e.clientY);
      });
      stick.addEventListener('pointermove', e => { if (dragging) setByY(e.clientY); });
      stick.addEventListener('pointerup', () => { dragging = false; resetStick(); });
      stick.addEventListener('pointercancel', () => { dragging = false; resetStick(); });
      stick.addEventListener('lostpointercapture', () => { dragging = false; resetStick(); });

      // Touch fallback
      stick.addEventListener('touchstart', e => {
        if (!e.touches || !e.touches[0]) return;
        e.preventDefault();
        setByY(e.touches[0].clientY);
      }, { passive: false });
      stick.addEventListener('touchmove', e => {
        if (!e.touches || !e.touches[0]) return;
        e.preventDefault();
        setByY(e.touches[0].clientY);
      }, { passive: false });
      stick.addEventListener('touchend', e => { e.preventDefault(); resetStick(); }, { passive: false });
      stick.addEventListener('touchcancel', e => { e.preventDefault(); resetStick(); }, { passive: false });
    }

    function bindHoldButton(id, startCmd, stopCmd) {
      const el = document.getElementById(id);
      if (!el) return;

      const start = e => { if (e) e.preventDefault(); send(startCmd); };
      const stop = e => { if (e) e.preventDefault(); send(stopCmd); };

      el.addEventListener('pointerdown', start);
      el.addEventListener('pointerup', stop);
      el.addEventListener('pointercancel', stop);
      el.addEventListener('lostpointercapture', stop);
      el.addEventListener('pointerleave', e => { if (e.buttons === 1) stop(e); });

      el.addEventListener('touchstart', start, { passive: false });
      el.addEventListener('touchend', stop, { passive: false });
      el.addEventListener('touchcancel', stop, { passive: false });

      el.addEventListener('mousedown', start);
      el.addEventListener('mouseup', stop);
      el.addEventListener('mouseleave', e => { if (e.buttons === 1) stop(e); });
    }

    function bindSinglePress(id) {
      const el = document.getElementById(id);
      if (!el) return;
      const fire = e => { if (e) e.preventDefault(); send(id); };
      el.addEventListener('pointerdown', fire);
      el.addEventListener('touchstart', fire, { passive: false });
      el.addEventListener('mousedown', fire);
    }

    function resetAll() {
      L = 0; R = 0;
      draw('L', 0); draw('R', 0);
      sendTracks(true);
      send('stop');
      send('turret_stop');
      send('barrel_stop');
    }

    document.addEventListener('DOMContentLoaded', () => {
      setLinkState(false);
      connect();

      draw('L', 0);
      draw('R', 0);

      bindStick('stickL', 'L');
      bindStick('stickR', 'R');

      bindHoldButton('turret_left', 'turret_left_start', 'turret_stop');
      bindHoldButton('turret_right', 'turret_right_start', 'turret_stop');
      bindHoldButton('barrel_up', 'barrel_up_start', 'barrel_stop');
      bindHoldButton('barrel_down', 'barrel_down_start', 'barrel_stop');

      bindSinglePress('fire');
      bindSinglePress('stop');

      setInterval(() => sendTracks(true), 100);
      window.addEventListener('blur', resetAll);
      document.addEventListener('visibilitychange', () => { if (document.hidden) resetAll(); });
      window.addEventListener('pagehide', resetAll);
    });
  </script>
</body>
</html>
)rawliteral";

// 电机A运动控制（左履带）：speed > 0 前进，speed < 0 后退
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

// 电机B运动控制（右履带）
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
  turretMoveDir = 0;
  barrelMoveDir = 0;
}

// 把摇杆百分比 (-100..100) 映射到有手感的 PWM：
// 1) 死区去抖 2) 二次曲线细化低速 3) 起步补偿克服静摩擦
int16_t mapTrackPercentToPwm(int16_t pct) {
  pct = constrain(pct, -100, 100);
  int16_t sign = (pct < 0) ? -1 : 1;
  int16_t ap = abs(pct);

  if (ap <= JOYSTICK_DEADZONE) {
    return 0;
  }

  int16_t linear = (int16_t)(((int32_t)(ap - JOYSTICK_DEADZONE) * 100) /
                             (100 - JOYSTICK_DEADZONE));
  int16_t curved =
      (int16_t)(((int32_t)linear * linear) / 100); // quadratic curve

  int16_t pwm =
      MOTOR_MIN_EFFECTIVE_PWM +
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

  leftCurrentPwm =
      approachWithStep(leftCurrentPwm, leftTargetPwm, TRACK_SLEW_STEP);
  rightCurrentPwm =
      approachWithStep(rightCurrentPwm, rightTargetPwm, TRACK_SLEW_STEP);

  motorA_Move(leftCurrentPwm);
  motorB_Move(rightCurrentPwm);
}

void applyServoMotion() {
  const uint32_t now = millis();
  if (now - lastServoUpdateMs < SERVO_UPDATE_INTERVAL_MS) {
    return;
  }
  lastServoUpdateMs = now;

  if (turretMoveDir != 0) {
    turretAngle = constrain(turretAngle + turretMoveDir * TURRET_STEP_PER_TICK,
                            TURRET_MIN, TURRET_MAX);
    turretServo.write(turretAngle);
  }

  if (barrelMoveDir != 0) {
    barrelAngle = constrain(barrelAngle + barrelMoveDir * BARREL_STEP_PER_TICK,
                            BARREL_MIN, BARREL_MAX);
    barrelServo.write(barrelAngle);
  }
}

bool parseTrackCommand(char *payload) {
  // 格式: "<left>:<right>"，例如 "35:-60"
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

  leftTargetPwm = mapTrackPercentToPwm(lPct);
  rightTargetPwm = mapTrackPercentToPwm(rPct);
  lastTrackCmdMs = millis();
  return true;
}

// WebSocket 事件回调
// 文本命令:
// tracks:<left_pct>:<right_pct>
// turret_left_start/turret_right_start/turret_stop
// barrel_up_start/barrel_down_start/barrel_stop
// fire/stop
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

    // 避免 String 动态分配，使用定长栈缓冲区
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
    } else if (strcmp(msg, "turret_left_start") == 0) {
      turretMoveDir = +1;
    } else if (strcmp(msg, "turret_right_start") == 0) {
      turretMoveDir = -1;
    } else if (strcmp(msg, "turret_stop") == 0) {
      turretMoveDir = 0;
    } else if (strcmp(msg, "barrel_up_start") == 0) {
      barrelMoveDir = -1;
    } else if (strcmp(msg, "barrel_down_start") == 0) {
      barrelMoveDir = +1;
    } else if (strcmp(msg, "barrel_stop") == 0) {
      barrelMoveDir = 0;
    } else if (strcmp(msg, "fire") == 0) {
      if (!isFiring) {
        isFiring = true;
        fireStartTime = millis();
        fireServo.write(FIRE_TRIGGER_POS);
        LOG_PRINTLN(F("Fire triggered"));
      }
    }
    break;
  }

  case WS_EVT_PONG:
  case WS_EVT_ERROR:
    break;
  }
}

void initWiFiAP() {
  if (!WiFi.softAP(ap_ssid, ap_password)) {
    LOG_PRINTLN(F("AP failed to start"));
    return;
  }

  if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
    LOG_PRINTLN(F("Failed to set AP IP address"));
    return;
  }

  LOG_PRINTLN(F("Access Point started"));
  IPAddress ip = WiFi.softAPIP();
  LOG_PRINTF("IP Address: %u.%u.%u.%u\n", ip[0], ip[1], ip[2], ip[3]);
}

void setup() {
  Serial.begin(115200);

  turretServo.attach(TURRET_SERVO_PIN, 500, 2400);
  barrelServo.attach(BARREL_SERVO_PIN, 500, 2400);
  fireServo.attach(FIRE_SERVO_PIN, 500, 2400);

  turretServo.write(turretAngle);
  barrelServo.write(barrelAngle);
  fireServo.write(FIRE_READY_POS);

  if (ENABLE_SERVO_SELF_TEST) {
    delay(1000);
    turretServo.write(TURRET_MIN);
    barrelServo.write(BARREL_MIN);
    fireServo.write(FIRE_TRIGGER_POS);
    delay(700);
    turretServo.write(TURRET_MAX);
    barrelServo.write(BARREL_MAX);
    fireServo.write(FIRE_READY_POS);
    delay(700);
    turretServo.write(turretAngle);
    barrelServo.write(barrelAngle);
    fireServo.write(FIRE_READY_POS);
  }

  pinMode(AIN1, OUTPUT);
  pinMode(AIN2, OUTPUT);
  pinMode(BIN1, OUTPUT);
  pinMode(BIN2, OUTPUT);

  ledcAttach(PWMA, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(PWMB, PWM_FREQ, PWM_RESOLUTION);
  stopAllMotion();

  initWiFiAP();

  ws.onEvent(onEvent);
  server.addHandler(&ws);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.begin();
  LOG_PRINTLN(F("HTTP + WebSocket server started"));
}

void loop() {
  ws.cleanupClients();
  applyTrackOutput();
  applyServoMotion();

  if (isFiring && (millis() - fireStartTime >= fireDuration)) {
    isFiring = false;
    fireServo.write(FIRE_READY_POS);
  }
}







