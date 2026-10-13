# Heavy Punch Track Drive

ESP-IDF firmware for an ESP32-S3 tracked vehicle. The firmware drives the two
track motors only; turret, barrel, fire-servo, chat, sensor and display
features are intentionally out of scope.

This branch (`feature/safety-redesign`) reworks the control path around an
explicit safety state machine, a single control owner, an independent motion
watchdog, bidirectional status and runtime-tunable parameters.

## Runtime

- The ESP32-S3 starts its own Wi-Fi access point.
- AP SSID: `HeavyPunch-Track`
- AP password: `12345678`
- Control URL: `http://192.168.4.1`
- The page uses a WebSocket at `/ws`.

## Control Model

The phone UI has two vertical levers, matching real dual-track controls:

- Left lever controls the left track, right lever controls the right track.
- Center is `0%`, push up for forward `+1..+100%`, pull down for reverse `-1..-100%`.
- Releasing a lever returns that track to `0%`.
- `ARM` requests control and enables the levers.
- `STOP` brakes both tracks, disarms, and clears a FAULT.

Levers are disabled until the client has armed and the firmware confirms the
`ARMED` state. Only one client can hold control at a time; others connect as
spectators and see live status.

## Safety Model

```
DISARMED --arm--> ARMED <--control(zero)--> RUNNING
   ^                |   control(moving) ^      |
   |                +--------------------+      |
   +---- stop / timeout / controller lost ------+

any state --fault--> FAULT --stop--> DISARMED
```

Three independent layers stop the tracks:

1. Browser: lever release, window blur, page hidden and WebSocket close all
   zero the levers or send `stop`. Frames are sent every `80 ms` while armed.
2. Control task: no accepted control frame for `timeout` ms (default `350`)
   disarms and brakes immediately. Controller disconnect does the same.
3. Motion watchdog: an `esp_timer` one-shot running at `3 x timeout` (min
   `600 ms`) on the high-priority timer task. If the control task itself stops
   processing, the watchdog brakes the motors directly and latches a hardware
   fault that blocks new targets until the operator sends `stop`.

Control frames carry a sequence number; stale or duplicate frames are dropped.
Leaving `FAULT` always requires an explicit `stop`, so a reconnecting client
cannot restart the tracks by accident. Config changes are refused while armed.

Optional low-battery fault: set `lowbat` (mV) to a non-zero value and the
firmware enters `FAULT` after four consecutive low readings.

## Protocol

Client to firmware (text frames):

| Frame | Meaning |
| --- | --- |
| `arm` | Request control and arm the drive |
| `stop` | Brake, disarm, clear FAULT |
| `c:<seq>:<L>:<R>` | Control frame, `seq` is an increasing u32, `L`/`R` are `-100..100` |
| `cfg:get` | Request current config |
| `cfg:<key>=<value>` | Set one parameter (only while DISARMED or FAULT) |
| `cfg:save` | Persist config to NVS |

Firmware to client:

| Frame | Meaning |
| --- | --- |
| `s:<STATE>:<Lpwm>:<Rpwm>:<vbat_mv>:<fault>:<owner>` | Status at 10 Hz. `owner` is `1` for the controlling client |
| `cfg:dz=8,minpwm=85,slew=12,timeout=350,lowbat=0,invl=0,invr=0` | Current config |
| `ok:...` / `err:...` | Command results |

The UI treats the link as alive only while status frames keep arriving, not
merely while the WebSocket is open.

## Tuning

| Key | Range | Default | Purpose |
| --- | --- | --- | --- |
| `dz` | 0..40 | 8 | Lever deadzone in percent |
| `minpwm` | 0..254 | 85 | First PWM value that actually moves the motor |
| `slew` | 1..255 | 12 | Max PWM change per 15 ms update |
| `timeout` | 100..5000 | 350 | Software failsafe in ms |
| `lowbat` | 0..30000 | 0 | Low-battery fault threshold in mV, 0 disables |
| `invl` / `invr` | 0..1 | 0 | Invert left / right motor direction |

Example from the browser console on the control page:

```js
ws.send('cfg:invl=1'); ws.send('cfg:timeout=500'); ws.send('cfg:save');
```

Values are validated on the firmware and the full config is echoed back after
every successful set.

## Pin Mapping

Motor A is the left track: `PWMA` GPIO5, `AIN2` GPIO6, `AIN1` GPIO7.
Motor B is the right track: `PWMB` GPIO18, `BIN2` GPIO17, `BIN1` GPIO16.
Battery sense (optional): GPIO4 / ADC1_CH3 through a 100k/10k divider; adjust
the macros in `main/track_battery.c` to match the board.

PWM is LEDC low-speed mode, 5 kHz, 8-bit.

## Firmware Structure

Pure C, shared with host tests:

- `main/track_config.c`: parameter struct, defaults, validation, formatting
- `main/track_math.c`: percent-to-PWM curve, protocol parser, slew helper
- `main/track_safety.c`: safety state machine and sequence filter

ESP-IDF:

- `main/track_drive.c`: GPIO/LEDC output with brake and fault latches
- `main/track_watchdog.c`: independent motion watchdog on `esp_timer`
- `main/track_battery.c`: ADC pack voltage sensing
- `main/track_config_store.c`: NVS load/save
- `main/track_control.c`: control task, single owner of state machine and drive
- `main/web_server.c`: HTTP root, WebSocket, control ownership, status broadcast
- `main/web_ui.html`: mobile control page, embedded via `EMBED_TXTFILES`
- `main/app_main.c`: boot order (brake first), Wi-Fi AP

## Host Tests

```sh
test/host/run_tests.sh
```

Builds the three pure modules with `gcc -std=c11 -Wall -Wextra -Werror` and
runs `test_track_config`, `test_track_math` and `test_track_safety`.

## Build And Flash

From an ESP-IDF 5.x environment:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Expected serial log includes:

- `track_drive: initialized ...`
- `track_ctl: started, dz=8,minpwm=85,...`
- `AP started: ssid=HeavyPunch-Track password=12345678 url=http://192.168.4.1`
- `web_server: started on http://192.168.4.1`
