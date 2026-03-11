# IronSight FPV Scout (ESP32-S3)

ESP32-S3 N16R8 + OV2640 + MJPEG + WebSocket 履带控制的本地 AP / 局域网 FPV 侦察车项目。

## What Changed
- 保留双履带差速控制
- 保留双自由度云台
- 移除开火机构
- 增加 OV2640 摄像头支持
- 增加 MJPEG 视频流
- 前端升级为全屏 FPV 视频 + 悬浮控制层

## Runtime
- AP SSID: `IronSight_FPV_Scout`
- Password: `12345678`
- AP IP: `192.168.4.1`
- 控制页面: `http://192.168.4.1`
- 视频流: `http://192.168.4.1:81/stream`
- 抓拍: `http://192.168.4.1:81/capture.jpg`

## Build
1. Arduino IDE 打开 `TankCommandDeck/TankCommandDeck.ino`
2. `Tools -> Board`: `ESP32S3 Dev Module`
3. 确认已安装 `ESPAsyncWebServer`、`AsyncTCP`、`ESP32Servo`
4. 确认 Arduino ESP32 Core 含 `esp_camera`
5. 编译并上传

## Pin Map
### Tracks
- 左履带 PWM: `GPIO5`
- 左履带方向: `GPIO6`, `GPIO7`
- 右履带 PWM: `GPIO18`
- 右履带方向: `GPIO17`, `GPIO16`

### Gimbal
- Pan Servo: `GPIO13`
- Tilt Servo: `GPIO12`

### OV2640 Camera
- `XCLK -> GPIO15`
- `PCLK -> GPIO14`
- `VSYNC -> GPIO21`
- `HREF -> GPIO47`
- `SIOD -> GPIO9`
- `SIOC -> GPIO8`
- `D0 -> GPIO1`
- `D1 -> GPIO2`
- `D2 -> GPIO4`
- `D3 -> GPIO10`
- `D4 -> GPIO11`
- `D5 -> GPIO38`
- `D6 -> GPIO39`
- `D7 -> GPIO40`
- `PWDN -> GPIO41`
- `RESET -> GPIO42`

## Wiring Notes
- `GPIO19 / GPIO20` 预留给板载原生 USB，不用于摄像头
- `GPIO48` 预留给板载 RGB LED
- `GPIO0` 不用于摄像头
- `GPIO35 / GPIO36 / GPIO37` 不纳入本方案
- 摄像头供电请按模组实际标注接 `3.3V` 或 `5V`
- 电机电源与控制电源要做好退耦并共地

## WebSocket Commands
- 履带:
  - `tracks:<left_pct>:<right_pct>`
  - `stop`
- 云台:
  - `pan_left_start`
  - `pan_right_start`
  - `pan_stop`
  - `tilt_up_start`
  - `tilt_down_start`
  - `tilt_stop`
  - `center_view`

## Tuning Parameters
- 履带:
  - `JOYSTICK_DEADZONE`
  - `MOTOR_MIN_EFFECTIVE_PWM`
  - `TRACK_SLEW_STEP`
  - `TRACK_SIGNAL_TIMEOUT_MS`
  - `LEFT_TRACK_GAIN_PERCENT`
  - `RIGHT_TRACK_GAIN_PERCENT`
- 云台:
  - `PAN_MIN`, `PAN_MAX`
  - `TILT_MIN`, `TILT_MAX`
  - `PAN_STEP_PER_TICK`
  - `TILT_STEP_PER_TICK`
- 视频:
  - `CAMERA_FRAME_SIZE`
  - `CAMERA_JPEG_QUALITY`
  - `CAMERA_FB_COUNT`
