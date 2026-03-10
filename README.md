# Tank Command Deck (ESP32-S3)

Arduino + ESP32 + 双电机 + 三舵机（炮塔/炮管/开火）履带坦克控制项目。

## Files
- `TankCommandDeck.ino`: 主程序（热点 + Web 控制台 + WebSocket + 电机/舵机控制）
- `README.md`: 项目说明

## Hardware
- 主控: ESP32-S3 Dev Module
- 电机驱动: 双路 H 桥（A/B 两路）
- 舵机: 
  - `TURRET_SERVO_PIN` 炮塔旋转
  - `BARREL_SERVO_PIN` 炮管俯仰
  - `FIRE_SERVO_PIN` 开火机构

## Pin Mapping
- 电机 A（左履带）
  - `PWMA=5`, `AIN2=6`, `AIN1=7`
- 电机 B（右履带）
  - `PWMB=18`, `BIN2=17`, `BIN1=16`
- 舵机
  - `TURRET_SERVO_PIN=13`
  - `BARREL_SERVO_PIN=12`
  - `FIRE_SERVO_PIN=11`

## Build / Upload
1. Arduino IDE 打开 `TankCommandDeck.ino`
2. `Tools -> Board`: `ESP32S3 Dev Module`
3. 串口选择开发板端口
4. 编译并上传

## Runtime
- ESP32 启动后创建 AP:
  - SSID: `ESP32_WiFi_Motion_Control`
  - Password: `12345678`
  - IP: `192.168.4.1`
- 手机连接 AP 后访问:
  - `http://192.168.4.1`

## Control Model
- 双摇杆履带控制
  - 左摇杆控制左履带
  - 右摇杆控制右履带
- 炮塔/炮管为按住连续转动
  - 按下开始，松开停止
- 开火为非阻塞动作

## WebSocket Commands
- 履带: `tracks:<left_pct>:<right_pct>` (`-100..100`)
- 炮塔:
  - `turret_left_start`
  - `turret_right_start`
  - `turret_stop`
- 炮管:
  - `barrel_up_start`
  - `barrel_down_start`
  - `barrel_stop`
- 其他:
  - `fire`
  - `stop`

## Tuning Parameters (in .ino)
- 履带手感
  - `JOYSTICK_DEADZONE`
  - `MOTOR_MIN_EFFECTIVE_PWM`
  - `TRACK_SLEW_STEP`
  - `TRACK_SIGNAL_TIMEOUT_MS`
  - `LEFT_TRACK_GAIN_PERCENT`
  - `RIGHT_TRACK_GAIN_PERCENT`
- 舵机速度
  - `SERVO_UPDATE_INTERVAL_MS`
  - `TURRET_STEP_PER_TICK`
  - `BARREL_STEP_PER_TICK`

## Notes
- 如果履带方向与期望相反，可调整对应电机方向引脚逻辑或交换电机线序。
- 如果舵机方向相反，可在接收命令处调整方向符号。
- 调试日志开关: `ENABLE_DEBUG_LOG`。
