# Heavy Punch Track Drive

ESP-IDF firmware for an ESP32-S3 tracked vehicle. The current firmware only drives the two track motors; all turret, barrel, fire-servo, chat, sensor, and display features have been removed from the runtime surface.

## Runtime

- The ESP32-S3 starts its own Wi-Fi access point.
- AP SSID: `HeavyPunch-Track`
- AP password: `12345678`
- Control URL: `http://192.168.4.1`
- The page uses a WebSocket at `/ws`.

## Control Model

The phone UI has two vertical levers, matching real dual-track controls:

- Left lever controls the left track.
- Right lever controls the right track.
- Center is `0%`.
- Push up for forward `+1..+100%`.
- Pull down for reverse `-1..-100%`.
- Holding a lever position keeps that track running at the corresponding percentage.
- Releasing a lever returns that track to `0%`.
- The `STOP` button immediately brakes both tracks.

Safety behavior:

- If the browser disconnects, goes hidden, loses focus, or stops sending control frames, the firmware stops the tracks.
- Firmware command timeout is `350 ms`.
- The UI sends repeated track frames every `80 ms` while open.

## Pin Mapping

Motor A is the left track:

- `PWMA`: GPIO5
- `AIN2`: GPIO6
- `AIN1`: GPIO7

Motor B is the right track:

- `PWMB`: GPIO18
- `BIN2`: GPIO17
- `BIN1`: GPIO16

## Firmware Structure

- `main/app_main.c`: NVS, Wi-Fi AP, web server startup, drive update task
- `main/track_math.c`: percentage-to-PWM mapping, command parsing, slew helper
- `main/track_drive.c`: GPIO and LEDC hardware output
- `main/web_server.c`: HTTP root page and WebSocket command handling
- `main/web_ui.h`: embedded mobile control page
- `test/host/test_track_math.c`: host-style tests for the core track math

## Build

This machine has ESP-IDF under:

- `C:\Espressif\frameworks\esp-idf-v5.5.3`

From an ESP-IDF PowerShell environment:

```powershell
idf.py set-target esp32s3
idf.py build
```

If ESP-IDF tools are not exported into `PATH`, use the same environment variables as the local scripts or run Espressif's export script first.

## Flash

```powershell
idf.py -p COMx flash monitor
```

Replace `COMx` with the connected ESP32-S3 serial port.

Expected serial log includes:

- `AP started: ssid=HeavyPunch-Track password=12345678 url=http://192.168.4.1`
- `web_server: started on http://192.168.4.1`

## Tuning

Track control constants live in `main/track_math.h` and `main/track_drive.h`:

- `TRACK_DEADZONE_PERCENT`
- `TRACK_MIN_EFFECTIVE_PWM`
- `TRACK_SLEW_STEP`
- `TRACK_DRIVE_UPDATE_INTERVAL_MS`
- `TRACK_COMMAND_TIMEOUT_MS`

If a track direction is reversed, swap that motor's two direction wires or invert the corresponding direction logic in `main/track_drive.c`.
