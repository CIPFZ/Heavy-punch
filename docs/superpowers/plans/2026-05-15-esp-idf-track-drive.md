# ESP-IDF Track Drive Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Arduino tank firmware with a focused ESP-IDF firmware that exposes a phone-friendly dual-track control UI and drives only the two tracked motors.

**Architecture:** Build a clean ESP-IDF project at the repository root. Keep motor math and command parsing in small testable C modules, use ESP-IDF Wi-Fi AP plus native HTTP/WebSocket server for the control path, and use LEDC/GPIO for H-bridge output.

**Tech Stack:** ESP-IDF C, FreeRTOS, esp_http_server WebSocket support, LEDC PWM, GPIO, host-side C unit tests compiled with local GCC when available.

---

### Task 1: Project Skeleton

**Files:**
- Create: `CMakeLists.txt`
- Create: `main/CMakeLists.txt`
- Create: `main/app_main.c`
- Create: `main/track_math.h`
- Create: `main/track_math.c`
- Create: `main/track_drive.h`
- Create: `main/track_drive.c`
- Create: `main/web_server.h`
- Create: `main/web_server.c`
- Create: `main/web_ui.h`
- Create: `sdkconfig.defaults`

- [ ] Create a standard ESP-IDF project rooted at the repository.
- [ ] Register one `main` component containing the firmware modules.
- [ ] Target ESP32-S3 by default through build workflow, with source compatible with ESP-IDF 5.x.

### Task 2: Host Tests First

**Files:**
- Create: `test/host/test_track_math.c`

- [ ] Write tests for percent clamping, deadzone, minimum effective PWM, max PWM, command parsing, invalid command rejection, and slew behavior.
- [ ] Run the tests before implementation and confirm they fail because `track_math.c` behavior is not available in the new project.
- [ ] Implement the minimum `track_math` behavior and rerun tests until they pass.

### Task 3: Track Drive Hardware Layer

**Files:**
- Modify: `main/track_drive.h`
- Modify: `main/track_drive.c`

- [ ] Configure direction GPIO pins for left/right H-bridge channels.
- [ ] Configure LEDC PWM channels for left/right speed pins.
- [ ] Implement braking, coast/zero output, target update, periodic slew update, and command timeout stop.
- [ ] Keep all hardware writes behind the track drive module.

### Task 4: Web UI And WebSocket Control

**Files:**
- Modify: `main/web_ui.h`
- Modify: `main/web_server.h`
- Modify: `main/web_server.c`
- Modify: `main/app_main.c`

- [ ] Serve a single mobile-first HTML page.
- [ ] UI contains only two vertical track levers, live percentages, connection state, and emergency stop.
- [ ] WebSocket endpoint accepts `tracks:<left>:<right>` and `stop`.
- [ ] UI sends continuous control frames while a lever is held and sends zero on release, blur, hidden, or disconnect.

### Task 5: Wi-Fi AP Runtime

**Files:**
- Modify: `main/app_main.c`
- Modify: `sdkconfig.defaults`

- [ ] Initialize NVS, network interface, event loop, and Wi-Fi AP.
- [ ] Use SSID `HeavyPunch-Track`, password `12345678`, and fixed AP IP `192.168.4.1`.
- [ ] Start the web server after AP startup.
- [ ] Start a FreeRTOS drive task that updates output at fixed intervals.

### Task 6: Documentation And Cleanup

**Files:**
- Modify: `README.md`
- Delete or leave obsolete Arduino files based on implementation outcome.

- [ ] Document ESP-IDF build, flash, runtime, pin mapping, and UI model.
- [ ] Remove servo feature references from project docs.

### Task 7: Verification

**Commands:**
- `gcc -std=c11 -Wall -Wextra -Werror -I main test/host/test_track_math.c main/track_math.c -o build_host/test_track_math.exe`
- `build_host/test_track_math.exe`
- `idf.py set-target esp32s3`
- `idf.py build`
- `idf.py flash monitor`

- [ ] Host tests pass.
- [ ] ESP-IDF build exits 0.
- [ ] Firmware flashes to the connected COM port.
- [ ] Serial output shows AP IP and web server startup.
- [ ] Browser or HTTP check can retrieve the control UI.
