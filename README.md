# Heavy Punch FPV Track Drive

ESP-IDF firmware for an ESP32-S3 tracked vehicle with OV2640 FPV video, dual-track motor control, and camera tilt servo control. Turret, barrel, fire-servo, chat, sensor, and display features remain outside the runtime surface.

## Runtime

- The ESP32-S3 starts its own Wi-Fi access point.
- AP SSID: `HeavyPunch-Track`
- AP password: `12345678`
- Control URL: `http://192.168.4.1`
- Primary camera stream: WebSocket binary JPEG at `ws://192.168.4.1/video-ws`
- Fallback camera stream: `http://192.168.4.1:81/stream`
- Snapshot: `http://192.168.4.1/capture.jpg`
- The page uses a control WebSocket at `/ws`.
- The access point is configured for up to 5 client devices.

## Video

The current video test profile is:

- Sensor: OV2640
- Primary stream format: JPEG frames over a binary WebSocket on `/video-ws`
- Fallback stream format: MJPEG over HTTP on port `81`
- Frame size: `QVGA 320x240`
- JPEG quality: `24` (`esp32-camera` uses lower numbers for larger, higher-quality JPEGs)
- Capture limit: about `20 fps`
- Maximum MJPEG clients: `5`

Camera capture is isolated from network streaming. A dedicated capture task reads the OV2640 on Core 1 and stores the latest JPEG frame in PSRAM. WebSocket video clients, fallback MJPEG clients, and snapshots read from that latest-frame cache, so slow network clients do not directly block sensor capture.

The firmware does not use H.264/H.265. ESP32-S3 has no hardware H.264 encoder, and software H.264 encoding at useful FPV resolutions competes with Wi-Fi, camera DMA, control handling, and PSRAM bandwidth. For higher-resolution smooth video, the practical hardware path is a camera/SoC with hardware video encoding or an external encoder.

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

The WebSocket control path is intentionally simple:

- `tracks:<left>:<right>`
- `tilt:<percent>`
- `stop`

`tilt` controls the FPV camera pitch servo from `-100%` to `+100%`; positive values tilt the camera upward.

Safety behavior:

- If the browser disconnects, goes hidden, loses focus, or stops sending control frames, the firmware stops the tracks.
- Firmware command timeout is `350 ms`.
- The UI sends repeated track and tilt frames every `120 ms` while open.
- The UI prevents browser double-tap zoom and reconnects the control WebSocket automatically after a refresh or network drop.

## Pin Mapping

Motor A is the left track:

- `PWMA`: GPIO12
- `AIN2`: GPIO6
- `AIN1`: GPIO7

Motor B is the right track:

- `PWMB`: GPIO18
- `BIN2`: GPIO17
- `BIN1`: GPIO16

OV2640 camera module:

- `PCLK`: GPIO14
- `VSYNC`: GPIO21
- `HREF`: GPIO47
- `SIOD`: GPIO9
- `SIOC`: GPIO8
- `D0`: GPIO1
- `D1`: GPIO2
- `D2`: GPIO4
- `D3`: GPIO10
- `D4`: GPIO11
- `D5`: GPIO38
- `D6`: GPIO39
- `D7`: GPIO40
- `PWDN`: GPIO41
- `RESET`: GPIO42
- `XCLK`: GPIO15
- `FLASH`: GPIO3

Camera pitch servo:

- `PWM`: GPIO13

## Firmware Structure

- `main/app_main.c`: NVS, Wi-Fi AP, web server startup, drive update task
- `main/camera_stream.c`: OV2640 init, latest-frame capture task, latest-frame cache, fallback MJPEG stream on port 81, snapshot endpoint
- `main/camera_tilt.c`: FPV camera pitch servo output
- `main/track_math.c`: percentage-to-PWM mapping, command parsing, slew helper
- `main/track_drive.c`: GPIO and LEDC hardware output
- `main/web_server.c`: HTTP root page, snapshot routing, simplified control WebSocket, binary JPEG video WebSocket
- `main/web_ui.h`: embedded FPV video and dual-track control page
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
