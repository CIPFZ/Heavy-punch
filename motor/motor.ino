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

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP32Servo.h>
#include <string.h>
#include <stdlib.h>

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
const char* ap_ssid = "ESP32_WiFi_Motion_Control";
const char* ap_password = "12345678";
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
const int16_t JOYSTICK_DEADZONE = 8;         // 摇杆死区：防抖
const int16_t MOTOR_MIN_EFFECTIVE_PWM = 85;  // 电机起转补偿
const uint8_t TRACK_SLEW_STEP = 12;          // 每次更新最大 PWM 变化，越小越平滑
const uint16_t DRIVE_UPDATE_INTERVAL_MS = 15;
const uint16_t TRACK_SIGNAL_TIMEOUT_MS = 350;  // 摇杆信号超时自动停车

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
const int16_t SERVO_STEP = 5;
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
int8_t turretMoveDir = 0;  // -1: right, +1: left
int8_t barrelMoveDir = 0;  // -1: up, +1: down
uint32_t lastServoUpdateMs = 0;

// 控制页面（存储在 Flash，减少 RAM 占用）
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<title>Tank Command Deck</title>
<style>
:root{
  --bg0:#0b0f1a;
  --bg1:#111a2b;
  --panel:#0f1727cc;
  --panel-border:#345073;
  --text:#e7eef8;
  --muted:#93abc8;
  --accent:#38bdf8;
  --accent2:#0ea5e9;
  --ok:#22c55e;
  --warn:#f59e0b;
  --danger:#ef4444;
}
*{box-sizing:border-box}
html,body{
  margin:0;height:100%;overflow:hidden;color:var(--text);
  font-family:"Segoe UI","Noto Sans SC","PingFang SC","Microsoft YaHei",sans-serif;
  -webkit-tap-highlight-color:transparent;
  background:
    radial-gradient(1400px 700px at -10% -20%,#1d4ed855,transparent),
    radial-gradient(1200px 600px at 120% 0%,#0ea5e922,transparent),
    linear-gradient(160deg,var(--bg0),var(--bg1));
}
.hud{
  position:fixed;top:0;left:0;right:0;z-index:2;
  display:flex;justify-content:space-between;align-items:center;
  padding:10px 14px;color:var(--muted);font-size:12px;letter-spacing:.04em
}
.chip{
  border:1px solid #3a516f;border-radius:999px;padding:5px 10px;
  background:#0b1323b3
}
.main{display:flex;gap:14px;padding:44px 14px 14px;height:100%}
.panel{
  background:var(--panel);border:1px solid var(--panel-border);
  border-radius:16px;padding:12px;backdrop-filter:blur(6px)
}
.left{flex:1.2;display:flex;flex-direction:column;gap:10px}
.right{flex:1;display:flex;flex-direction:column;gap:10px}
.title{font-size:14px;font-weight:700;letter-spacing:.04em;text-transform:uppercase;color:#c6ddf7}
.subtitle{font-size:12px;color:var(--muted)}
.sticks{display:flex;justify-content:space-around;gap:14px;flex:1}
.stickWrap{display:flex;flex-direction:column;align-items:center;gap:8px;min-width:136px}
.stickLabel{font-size:12px;color:var(--muted);letter-spacing:.03em}
.stick{
  position:relative;width:104px;height:280px;border-radius:20px;touch-action:none;
  border:1px solid #476285;
  background:linear-gradient(180deg,#0a1120,#1e293b);
  box-shadow:inset 0 10px 24px #00000055,inset 0 -10px 24px #ffffff08,0 12px 25px #0000004a;
}
.centerLine{position:absolute;left:10px;right:10px;top:50%;height:2px;background:#557291}
.knob{
  position:absolute;left:10px;right:10px;height:48px;border-radius:12px;
  border:1px solid #8fd8ff;
  background:linear-gradient(180deg,var(--accent),var(--accent2));
  box-shadow:0 8px 14px #0284c799
}
.val{font-size:14px;font-weight:700;color:#b7e8ff}
.meter{height:10px;width:100%;background:#26364b;border:1px solid #3f5c7c;border-radius:999px;overflow:hidden}
.fill{height:100%;width:0;background:var(--ok);transition:width .08s linear}
.row{display:grid;grid-template-columns:repeat(3,1fr);gap:10px}
.btn{
  height:58px;border:1px solid #4a6688;border-radius:12px;color:#f7fbff;
  background:linear-gradient(180deg,#2b3b52,#1a2636);font-size:18px;font-weight:700;
  box-shadow:0 6px 10px #00000040;transition:transform .05s ease,box-shadow .05s ease,filter .1s ease;
}
.btn:active{transform:translateY(2px);box-shadow:0 2px 6px #00000066;filter:brightness(1.1)}
.fire{background:linear-gradient(180deg,#ef4444,#b91c1c);border-color:#fb7185}
.stop{background:linear-gradient(180deg,#fb923c,#c2410c);border-color:#fdba74}
.wide{grid-column:span 3}
@media (max-width:980px),(orientation:portrait){
  .main{flex-direction:column;padding-top:42px}
  .left,.right{flex:none}
  .stick{height:240px}
}
</style></head><body>
<div class="hud">
  <div class="chip">TANK COMMAND DECK</div>
  <div class="chip">WS CONTROL</div>
</div>

<div class="main">
  <div class="left panel">
    <div class="title">Dual Track Drive</div>
    <div class="subtitle">Left stick controls left track, right stick controls right track.</div>
    <div class="sticks">
      <div class="stickWrap">
        <div class="stickLabel">LEFT TRACK</div>
        <div id="stickL" class="stick"><div class="centerLine"></div><div id="knobL" class="knob"></div></div>
        <div id="valL" class="val">0%</div><div class="meter"><div id="fillL" class="fill"></div></div>
      </div>
      <div class="stickWrap">
        <div class="stickLabel">RIGHT TRACK</div>
        <div id="stickR" class="stick"><div class="centerLine"></div><div id="knobR" class="knob"></div></div>
        <div id="valR" class="val">0%</div><div class="meter"><div id="fillR" class="fill"></div></div>
      </div>
    </div>
  </div>

  <div class="right panel">
    <div class="title">Turret And Fire</div>
    <div class="subtitle">Hold buttons to rotate/elevate continuously, release to stop.</div>
    <div class="row">
      <button class="btn" id="barrel_up">BARREL +</button>
      <button class="btn fire" id="fire">FIRE</button>
      <button class="btn" id="turret_right">TURRET ▶</button>
      <button class="btn" id="turret_left">◀ TURRET</button>
      <button class="btn" id="barrel_down">BARREL -</button>
      <button class="btn stop" id="stop">EMERGENCY STOP</button>
    </div>
  </div>
</div>

<script>
const wsUrl='ws://'+location.hostname+'/ws';let ws=null;
let L=0,R=0,lastSent='';let tmr=0;
function connect(){ws=new WebSocket(wsUrl);ws.onclose=()=>setTimeout(connect,1200)}
function send(msg){if(ws&&ws.readyState===1)ws.send(msg)}
function sendTracks(force){const msg='tracks:'+L+':'+R;if(force||msg!==lastSent){send(msg);lastSent=msg}}
function draw(side,val){const stick=document.getElementById(side==='L'?'stickL':'stickR');
  const knob=document.getElementById(side==='L'?'knobL':'knobR');
  const txt=document.getElementById(side==='L'?'valL':'valR');
  const fill=document.getElementById(side==='L'?'fillL':'fillR');
  const h=stick.clientHeight,kh=46,mid=(h-kh)/2,range=mid;
  const y=mid-(val/100)*range; knob.style.top=y+'px'; txt.textContent=val+'%';
  fill.style.width=Math.abs(val)+'%'; fill.style.background=val>=0?'#22c55e':'#f59e0b';
}
function clamp(v,a,b){return v<a?a:v>b?b:v}
function bindStick(id,side){const stick=document.getElementById(id);
  const setByY=(clientY)=>{const r=stick.getBoundingClientRect();const c=r.top+r.height/2;
    const half=r.height/2;let p=Math.round(((c-clientY)/half)*100);p=clamp(p,-100,100);
    if(side==='L')L=p;else R=p;draw(side,p);sendTracks(false)};
  const up=()=>{if(side==='L')L=0;else R=0;draw(side,0);sendTracks(true)};
  stick.addEventListener('pointerdown',e=>{stick.setPointerCapture(e.pointerId);setByY(e.clientY)});
  stick.addEventListener('pointermove',e=>{if(e.buttons===1)setByY(e.clientY)});
  stick.addEventListener('pointerup',up);stick.addEventListener('pointercancel',up);
}
function bindHoldBtn(id,startCmd,stopCmd){
  const el=document.getElementById(id); if(!el) return;
  const start=(e)=>{e.preventDefault();send(startCmd)};
  const stop=(e)=>{if(e)e.preventDefault();send(stopCmd)};
  el.addEventListener('pointerdown',start);
  el.addEventListener('pointerup',stop);
  el.addEventListener('pointercancel',stop);
  el.addEventListener('lostpointercapture',stop);
  el.addEventListener('pointerleave',(e)=>{if(e.buttons===1)stop(e)});
}
window.addEventListener('load',()=>{
  connect();draw('L',0);draw('R',0);
  bindStick('stickL','L');bindStick('stickR','R');
  bindHoldBtn('turret_left','turret_left_start','turret_stop');
  bindHoldBtn('turret_right','turret_right_start','turret_stop');
  bindHoldBtn('barrel_up','barrel_up_start','barrel_stop');
  bindHoldBtn('barrel_down','barrel_down_start','barrel_stop');
  ['fire','stop'].forEach(id=>{
    const el=document.getElementById(id);el&&el.addEventListener('click',()=>send(id));
  });
  tmr=setInterval(()=>sendTracks(true),100);
  window.addEventListener('blur',()=>{L=0;R=0;draw('L',0);draw('R',0);sendTracks(true);send('stop');send('turret_stop');send('barrel_stop')});
  document.addEventListener('visibilitychange',()=>{if(document.hidden){L=0;R=0;draw('L',0);draw('R',0);sendTracks(true);send('stop');send('turret_stop');send('barrel_stop')}});
});
</script></body></html>
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

  int16_t linear = (int16_t)(((int32_t)(ap - JOYSTICK_DEADZONE) * 100) / (100 - JOYSTICK_DEADZONE));
  int16_t curved = (int16_t)(((int32_t)linear * linear) / 100);  // quadratic curve

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

  if (turretMoveDir != 0) {
    turretAngle = constrain(turretAngle + turretMoveDir * TURRET_STEP_PER_TICK, TURRET_MIN, TURRET_MAX);
    turretServo.write(turretAngle);
  }

  if (barrelMoveDir != 0) {
    barrelAngle = constrain(barrelAngle + barrelMoveDir * BARREL_STEP_PER_TICK, BARREL_MIN, BARREL_MAX);
    barrelServo.write(barrelAngle);
  }
}

bool parseTrackCommand(char* payload) {
  // 格式: "<left>:<right>"，例如 "35:-60"
  char* mid = strchr(payload, ':');
  if (mid == nullptr) {
    return false;
  }

  *mid = '\0';
  char* end1 = nullptr;
  char* end2 = nullptr;

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
void onEvent(AsyncWebSocket* serverRef, AsyncWebSocketClient* client, AwsEventType type,
             void* arg, uint8_t* data, size_t len) {
  (void)serverRef;

  switch (type) {
    case WS_EVT_CONNECT:
      LOG_PRINTF("WebSocket client #%u connected from %s\n",
                 client->id(), client->remoteIP().toString().c_str());
      break;

    case WS_EVT_DISCONNECT:
      LOG_PRINTF("WebSocket client #%u disconnected\n", client->id());
      stopAllMotion();
      break;

    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) {
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
      } else if (strcmp(msg, "turret_left") == 0) {
        // 兼容旧前端单击协议
        turretAngle = constrain(turretAngle + SERVO_STEP, TURRET_MIN, TURRET_MAX);
        turretServo.write(turretAngle);
      } else if (strcmp(msg, "turret_right") == 0) {
        turretAngle = constrain(turretAngle - SERVO_STEP, TURRET_MIN, TURRET_MAX);
        turretServo.write(turretAngle);
      } else if (strcmp(msg, "barrel_up") == 0) {
        barrelAngle = constrain(barrelAngle - SERVO_STEP, BARREL_MIN, BARREL_MAX);
        barrelServo.write(barrelAngle);
      } else if (strcmp(msg, "barrel_down") == 0) {
        barrelAngle = constrain(barrelAngle + SERVO_STEP, BARREL_MIN, BARREL_MAX);
        barrelServo.write(barrelAngle);
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
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
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
