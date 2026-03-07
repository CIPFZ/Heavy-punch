/*
 * WiFi Motion Core (ESP32 Tank Control)
 * Board: ESP32-S3
 * Libraries: ESPAsyncWebServer, AsyncTCP, ESP32Servo
 */

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESP32Servo.h>

// Wi-Fi AP config
const char* ap_ssid = "ESP32_WiFi_Motion_Control";
const char* ap_password = "12345678";
IPAddress local_IP(192, 168, 1, 4);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);

// Web server + WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Motor pins
#define PWMA 5
#define AIN2 6
#define AIN1 7
#define PWMB 18
#define BIN2 17
#define BIN1 16

// Servo pins
#define TURRET_SERVO_PIN 13
#define BARREL_SERVO_PIN 12
#define FIRE_SERVO_PIN 11

// PWM config
const int PWM_FREQ = 5000;
const int PWM_RESOLUTION = 8;

// Drive speed presets
const int DRIVE_SPEED = 200;
const int TURN_SPEED = 200;

// Servo objects
Servo turretServo;
Servo barrelServo;
Servo fireServo;

// Servo state + limits
int turretAngle = 90;
int barrelAngle = 90;
const int TURRET_MIN = 10;
const int TURRET_MAX = 170;
const int BARREL_MIN = 70;
const int BARREL_MAX = 130;
const int FIRE_READY_POS = 90;
const int FIRE_TRIGGER_POS = 130;
const int SERVO_STEP = 5;

// Non-blocking fire state
bool isFiring = false;
unsigned long fireStartTime = 0;
const unsigned long fireDuration = 500;

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
  <title>Tank Control</title>
  <style>
    html, body {
      margin: 0;
      padding: 0;
      width: 100%;
      height: 100%;
      overflow: hidden;
      font-family: sans-serif;
      background: #2c3e50;
      -webkit-tap-highlight-color: transparent;
    }
    .main-container {
      display: flex;
      flex-direction: row;
      width: 100%;
      height: 100%;
      box-sizing: border-box;
      padding: 20px;
      gap: 20px;
    }
    .left-panel, .right-panel {
      flex: 1;
      display: flex;
      justify-content: center;
      align-items: center;
    }
    .d-pad {
      display: grid;
      grid-template-areas:
        ". up ."
        "left center right"
        ". down .";
      gap: 15px;
      justify-content: center;
      align-items: center;
    }
    .btn {
      width: 80px;
      height: 80px;
      border: none;
      border-radius: 50%;
      color: #fff;
      font-size: 34px;
      background: #34495e;
      cursor: pointer;
      box-shadow: 0 5px #2c3e50;
      transition: all 0.1s ease;
      user-select: none;
      -webkit-user-select: none;
      touch-action: manipulation;
    }
    .btn:active {
      transform: translateY(3px);
      box-shadow: 0 2px #2c3e50;
    }
    #stop { grid-area: center; background: #7f8c8d; font-size: 28px; }
    #forward { grid-area: up; }
    #left { grid-area: left; }
    #right { grid-area: right; }
    #backward { grid-area: down; }
    #barrel_up { grid-area: up; }
    #turret_left { grid-area: left; }
    #turret_right { grid-area: right; }
    #barrel_down { grid-area: down; }
    #fire {
      grid-area: center;
      width: 90px;
      height: 90px;
      font-size: 40px;
      background: #e74c3c;
      box-shadow: 0 5px #c0392b;
    }
    #fire:active { box-shadow: 0 2px #c0392b; }
    #fullscreen-toggle {
      position: absolute;
      top: 15px;
      right: 15px;
      width: 45px;
      height: 45px;
      border-radius: 50%;
      border: 1px solid rgba(255,255,255,0.4);
      color: #fff;
      background: rgba(255,255,255,0.2);
      cursor: pointer;
      z-index: 1000;
    }
    @media (orientation: portrait) {
      .main-container {
        flex-direction: column;
        height: auto;
        overflow: auto;
      }
    }
  </style>
</head>
<body>
  <button id="fullscreen-toggle" aria-label="Toggle full screen">&#9974;</button>

  <div class="main-container">
    <div class="left-panel">
      <div class="d-pad">
        <button class="btn" id="forward">&#9650;</button>
        <button class="btn" id="left">&#9664;</button>
        <button class="btn" id="stop">&#9632;</button>
        <button class="btn" id="right">&#9654;</button>
        <button class="btn" id="backward">&#9660;</button>
      </div>
    </div>
    <div class="right-panel">
      <div class="d-pad">
        <button class="btn" id="barrel_up">&#9650;</button>
        <button class="btn" id="turret_left">&#9664;</button>
        <button class="btn" id="fire">F</button>
        <button class="btn" id="turret_right">&#9654;</button>
        <button class="btn" id="barrel_down">&#9660;</button>
      </div>
    </div>
  </div>

  <script>
    const gateway = `ws://${window.location.hostname}/ws`;
    let websocket = null;

    function initWebSocket() {
      websocket = new WebSocket(gateway);
      websocket.onopen = () => console.log('WS connected');
      websocket.onclose = () => {
        console.log('WS disconnected');
        setTimeout(initWebSocket, 2000);
      };
    }

    function sendMessage(message) {
      if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(message);
      }
    }

    function bindClickControl(id) {
      const el = document.getElementById(id);
      if (!el) return;
      el.addEventListener('click', () => sendMessage(id));
    }

    function bindHoldControl(id) {
      const el = document.getElementById(id);
      if (!el) return;

      const start = (e) => {
        e.preventDefault();
        sendMessage(id);
      };
      const stop = (e) => {
        e.preventDefault();
        sendMessage('stop');
      };

      el.addEventListener('pointerdown', start);
      el.addEventListener('pointerup', stop);
      el.addEventListener('pointercancel', stop);
      el.addEventListener('lostpointercapture', stop);
      el.addEventListener('pointerleave', (e) => {
        if (e.buttons === 1) stop(e);
      });
    }

    window.addEventListener('load', () => {
      initWebSocket();

      ['turret_left', 'turret_right', 'barrel_up', 'barrel_down', 'fire', 'stop']
        .forEach(bindClickControl);
      ['forward', 'backward', 'left', 'right'].forEach(bindHoldControl);

      window.addEventListener('blur', () => sendMessage('stop'));
      document.addEventListener('visibilitychange', () => {
        if (document.hidden) sendMessage('stop');
      });
    });

    document.getElementById('fullscreen-toggle').addEventListener('click', async () => {
      if (!document.fullscreenElement) {
        if (document.documentElement.requestFullscreen) {
          await document.documentElement.requestFullscreen();
        }
      } else if (document.exitFullscreen) {
        await document.exitFullscreen();
      }
    });
  </script>
</body>
</html>
)rawliteral";

void motorA_Move(int speed) {
  speed = constrain(speed, -255, 255);
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

void motorB_Move(int speed) {
  speed = constrain(speed, -255, 255);
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
}

void onEvent(AsyncWebSocket* serverRef, AsyncWebSocketClient* client, AwsEventType type,
             void* arg, uint8_t* data, size_t len) {
  (void)serverRef;

  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n",
                    client->id(), client->remoteIP().toString().c_str());
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      stopAllMotion();
      break;

    case WS_EVT_DATA: {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (!(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT)) {
        return;
      }

      String msg((const char*)data, len);
      Serial.printf("Received command: %s\n", msg.c_str());

      if (msg == "forward") {
        motorA_Move(DRIVE_SPEED);
        motorB_Move(DRIVE_SPEED);
      } else if (msg == "backward") {
        motorA_Move(-DRIVE_SPEED);
        motorB_Move(-DRIVE_SPEED);
      } else if (msg == "left") {
        motorA_Move(-TURN_SPEED);
        motorB_Move(TURN_SPEED);
      } else if (msg == "right") {
        motorA_Move(TURN_SPEED);
        motorB_Move(-TURN_SPEED);
      } else if (msg == "stop") {
        stopAllMotion();
      } else if (msg == "turret_left") {
        turretAngle = constrain(turretAngle + SERVO_STEP, TURRET_MIN, TURRET_MAX);
        turretServo.write(turretAngle);
      } else if (msg == "turret_right") {
        turretAngle = constrain(turretAngle - SERVO_STEP, TURRET_MIN, TURRET_MAX);
        turretServo.write(turretAngle);
      } else if (msg == "barrel_up") {
        barrelAngle = constrain(barrelAngle - SERVO_STEP, BARREL_MIN, BARREL_MAX);
        barrelServo.write(barrelAngle);
      } else if (msg == "barrel_down") {
        barrelAngle = constrain(barrelAngle + SERVO_STEP, BARREL_MIN, BARREL_MAX);
        barrelServo.write(barrelAngle);
      } else if (msg == "fire") {
        if (!isFiring) {
          isFiring = true;
          fireStartTime = millis();
          fireServo.write(FIRE_TRIGGER_POS);
          Serial.println("Fire triggered");
        } else {
          Serial.println("Fire ignored: sequence in progress");
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
    Serial.println("AP failed to start");
    return;
  }

  if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
    Serial.println("Failed to set AP IP address");
    return;
  }

  Serial.println("Access Point started");
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);

  turretServo.attach(TURRET_SERVO_PIN, 500, 2400);
  barrelServo.attach(BARREL_SERVO_PIN, 500, 2400);
  fireServo.attach(FIRE_SERVO_PIN, 500, 2400);

  turretServo.write(turretAngle);
  barrelServo.write(barrelAngle);
  fireServo.write(FIRE_READY_POS);

  Serial.println("Servo initialized");
  Serial.println("Servo self-check start");
  delay(1000);

  turretServo.write(10);
  barrelServo.write(70);
  fireServo.write(FIRE_TRIGGER_POS);
  delay(1000);

  turretServo.write(170);
  barrelServo.write(130);
  fireServo.write(FIRE_READY_POS);
  delay(1000);

  turretServo.write(turretAngle);
  barrelServo.write(barrelAngle);
  fireServo.write(FIRE_READY_POS);
  Serial.println("Servo self-check done");

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
  Serial.println("HTTP + WebSocket server started");
}

void loop() {
  ws.cleanupClients();

  if (isFiring && (millis() - fireStartTime >= fireDuration)) {
    isFiring = false;
    fireServo.write(FIRE_READY_POS);
    Serial.println("Fire sequence complete");
  }
}
