# Heavy Punch FPV Track Drive

ESP-IDF firmware for an ESP32-S3 tracked vehicle with OV2640 FPV video, WebRTC browser video, dual-track motor control, and camera tilt control.

## Runtime

- The ESP32-S3 starts its own Wi-Fi access point.
- AP SSID: `HeavyPunch-Track`
- AP password: `12345678`
- Control URL: `http://192.168.4.1`
- Primary camera stream: ESP-WebRTC Solution with H264 send-only video.
- WebRTC page and local signaling:
  - `GET /`
  - `GET /webrtc`
  - `GET /webrtc/signal`
  - `POST /webrtc/signal/post`
- Vehicle control WebSocket: `ws://192.168.4.1/ws`

The old HTTP MJPEG, binary JPEG WebSocket, and snapshot endpoints are removed from the runtime path. The OV2640 is now owned by `esp_capture` and provided directly to `esp_webrtc`, avoiding competing camera drivers and duplicate frame buffers.

## Video

The current video profile is:

- Sensor: OV2640
- Capture stack: Espressif `esp_capture` DVP source
- Browser transport: WebRTC
- WebRTC video codec: H264
- Frame size: `QVGA 320x240`
- Target frame rate: `20 fps`
- Audio: disabled
- Data channel: disabled

The browser WebRTC path uses H264 because mainstream browsers do not negotiate MJPEG as a WebRTC video codec. On ESP32-S3 this runs through the software H264 encoder, so QVGA is the conservative profile for stability.

Browser note: WebRTC APIs are normally tied to secure contexts. Some browsers allow WebRTC on private/local origins during development, while others may require HTTPS or browser flags. The firmware currently uses local HTTP signaling on the ESP32 AP to keep the embedded server small and focused.

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

The control WebSocket path is intentionally simple:

- `tracks:<left>:<right>`
- `tilt:<percent>`
- `stop`

`tilt` controls the FPV camera pitch servo from `-100%` to `+100%`; positive values tilt the camera upward.

Safety behavior:

- If the browser disconnects, goes hidden, loses focus, or stops sending control frames, the firmware stops the tracks.
- Firmware command timeout is `350 ms`.
- The UI sends repeated track and tilt frames every `120 ms` while open.
- The UI prevents browser double-tap zoom and reconnects the control WebSocket automatically after refresh or network drop.

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

- `main/app_main.c`: NVS, track/tilt init, media init, Wi-Fi AP, web server, WebRTC startup
- `main/media_sys.c`: OV2640 DVP camera source and `esp_capture` provider
- `main/webrtc_app.c`: `esp_webrtc` H264 send-only peer and local SSE/POST signaling
- `main/camera_tilt.c`: FPV camera pitch servo output
- `main/track_math.c`: percentage-to-PWM mapping, command parsing, slew helper
- `main/track_drive.c`: GPIO and LEDC hardware output
- `main/web_server.c`: HTTP root page, WebRTC routes, and simplified control WebSocket
- `main/web_ui.h`: embedded WebRTC FPV video and dual-track control page
- `components/`: vendored ESP-WebRTC Solution components required by this firmware
- `partitions.csv`: custom 3 MB app partition for WebRTC dependencies
- `test/host/test_track_math.c`: host-style tests for the core track math

## Build

From an ESP-IDF PowerShell environment:

```powershell
idf.py set-target esp32s3
idf.py build
```

If ESP-IDF tools are not exported into `PATH`, open an ESP-IDF PowerShell environment or run
the export script from your local ESP-IDF installation first:

```powershell
. <your-esp-idf-installation>\export.ps1
idf.py build
```

## Flash

```powershell
idf.py -p COMx flash monitor
```

Replace `COMx` with the connected ESP32-S3 serial port.

Expected serial log includes:

- `AP started: ssid=HeavyPunch-Track password=12345678 url=http://192.168.4.1`
- `web_server: started on http://192.168.4.1`
- `media_sys: initialized DVP MJPEG capture source`

## Tuning

Track control constants live in `main/track_math.h` and `main/track_drive.h`:

- `TRACK_DEADZONE_PERCENT`
- `TRACK_MIN_EFFECTIVE_PWM`
- `TRACK_SLEW_STEP`
- `TRACK_DRIVE_UPDATE_INTERVAL_MS`
- `TRACK_COMMAND_TIMEOUT_MS`

If a track direction is reversed, swap that motor's two direction wires or invert the corresponding direction logic in `main/track_drive.c`.
