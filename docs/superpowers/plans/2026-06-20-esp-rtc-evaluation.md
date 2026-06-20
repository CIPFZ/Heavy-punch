# ESP-RTC / ESP WebRTC Evaluation Plan

> **For the next Codex session:** continue on branch `codex/esp-rtc-evaluation`.
> This branch is for evaluation and spike work only. Do not replace the working JPEG WebSocket video path until the success criteria below are met.

## Goal

Evaluate whether an Espressif realtime video stack should replace or supplement the current FPV video transport.

Current stable baseline:

- AP SSID: `HeavyPunch-Track`
- UI: `http://192.168.4.1/`
- Control: WebSocket `/ws`
- Primary video: binary JPEG WebSocket `/video-ws`
- Fallback video: HTTP MJPEG `http://192.168.4.1:81/stream`
- Snapshot: `http://192.168.4.1/capture.jpg`
- Architecture branch: `codex/fpv-architecture-refactor`

The control architecture is not part of this experiment. Control must remain independent and should continue using `/ws` unless a later decision explicitly changes it.

## Candidate Technologies

### Candidate A: ESP-RTC

Espressif announced ESP-RTC as a realtime audio/video communication solution built around ESP32-S3-Korvo-2. It describes SIP signaling and RTP/RTCP/SRTP/TURN-style media transport, with MJPEG video stream support.

Use this as a research target, but verify whether current public code and examples are usable with this project before attempting integration.

Reference:

- [Espressif ESP-RTC announcement](https://www.espressif.com/en/news/ESP-RTC)

### Candidate B: ESP WebRTC Solution / `esp_peer`

Espressif's newer WebRTC direction appears to be `esp-webrtc-solution`, including:

- `esp_webrtc`
- `esp_peer`
- `esp_capture`
- `av_render`

This may be the more active implementation path for browser-compatible WebRTC experiments.

References:

- [espressif/esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution)
- [`esp_peer` component](https://components.espressif.com/components/espressif/esp_peer)

### Candidate C: KVS WebRTC SDK For ESP

Espressif also announced an ESP-IDF adaptation of Amazon KVS WebRTC SDK for ESP in 2026. This is likely more cloud/product oriented than this local AP FPV project, but should be noted as a possible future direction.

Reference:

- [Amazon KVS WebRTC SDK for ESP announcement](https://developer.espressif.com/blog/2026/01/kvs-webrtc-sdk-esp-announcement/)

## Non-Goals

- Do not rewrite track/tilt control.
- Do not make the computer connect to the ESP32 AP during development; keep Codex online.
- Do not remove `/video-ws` or `:81/stream` during evaluation.
- Do not assume UDP is available from browser JavaScript.
- Do not merge experimental signaling, cloud, STUN, or TURN infrastructure into the main refactor branch.
- Do not optimize video quality before proving latency and stability.

## Success Criteria

The experiment is only successful if it beats the current JPEG WebSocket baseline in practical FPV terms:

- [ ] Phone can view video from the ESP32-S3 AP mode without external internet.
- [ ] Browser workflow remains simple enough for the project: ideally open `http://192.168.4.1/`.
- [ ] Control `/ws` remains responsive while video is active.
- [ ] Video stall or reconnect does not affect control safety timeout.
- [ ] End-to-end video latency is visibly lower or smoother than current `/video-ws`.
- [ ] Memory headroom is acceptable on ESP32-S3 with PSRAM.
- [ ] CPU load does not starve Core 1 realtime/hardware tasks.
- [ ] Signaling requirements are understood and are acceptable for an AP-only vehicle.
- [ ] OV2640 JPEG/MJPEG source can be used without expensive software video encoding.

## Rejection Criteria

Stop the experiment and keep the current JPEG WebSocket architecture if any of these are true:

- [ ] Requires a cloud or LAN signaling server for basic AP-local viewing.
- [ ] Requires the phone to install a native app.
- [ ] Requires software H.264/H.265 encoding on ESP32-S3.
- [ ] Consumes enough CPU/heap/PSRAM to destabilize control.
- [ ] Cannot run with ESP-IDF 5.5.x or the current ESP32-S3 target without large framework changes.
- [ ] Browser connection setup is materially more complex than the current UI.
- [ ] Latency is not meaningfully better than current `/video-ws`.

## Baseline Measurements First

Before running ESP-RTC/WebRTC demos, capture a rough baseline from the current architecture branch.

Branch:

```powershell
git switch codex/fpv-architecture-refactor
git pull
```

Record:

- [ ] Current frame size.
- [ ] Current JPEG quality.
- [ ] Current capture FPS.
- [ ] Current `/video-ws` observed FPS.
- [ ] Approximate phone-visible latency.
- [ ] Free heap / PSRAM after startup.
- [ ] Control responsiveness while video is active.
- [ ] Behavior with a slow or disconnected video client.

Keep notes in:

- `docs/experiments/esp-rtc-evaluation-notes.md`

## Phase 1: Research And Candidate Selection

### Tasks

- [ ] Read `esp-webrtc-solution` README and examples.
- [ ] Identify which examples build for ESP32-S3.
- [ ] Identify whether examples support camera video input or only audio/data channel.
- [ ] Identify whether examples support browser peer, ESP peer only, or require a demo signaling server.
- [ ] Identify whether MJPEG/JPEG is supported as a media payload path.
- [ ] Record ESP-IDF version requirements.
- [ ] Record extra managed components and memory implications.

### Output

Create:

- `docs/experiments/esp-rtc-evaluation-notes.md`

Include:

- Candidate selected for spike.
- Why other candidates were rejected or postponed.
- Build requirements.
- Signaling model.
- Expected camera input model.

## Phase 2: External Demo Build Spike

Do this outside the firmware integration first.

Recommended location:

- `scratch/esp-webrtc-solution/`

Tasks:

- [ ] Clone or add the selected Espressif example outside `main/`.
- [ ] Build the smallest relevant ESP32-S3 demo.
- [ ] Confirm required ESP-IDF version.
- [ ] Confirm whether it builds under the local ESP-IDF:

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1
idf.py set-target esp32s3
idf.py build
```

- [ ] Do not commit large third-party source trees unless there is a clear reason.
- [ ] Commit only notes, small patches, or integration docs unless instructed otherwise.

## Phase 3: AP-Local Browser Feasibility

Tasks:

- [ ] Determine whether the selected stack can run without internet.
- [ ] Determine whether signaling can be hosted on the ESP32 itself.
- [ ] Determine whether phone browser can connect in AP-only mode.
- [ ] Determine whether STUN/TURN is unnecessary for AP-local use.
- [ ] If a signaling page/server is needed, sketch the smallest local-only version.

Decision point:

- If external signaling is required for the basic FPV use case, reject the candidate for the main branch.

## Phase 4: Camera Source Feasibility

Tasks:

- [ ] Determine how the stack expects video frames.
- [ ] Confirm whether OV2640 native JPEG can be passed through.
- [ ] Confirm whether the stack wants RTP MJPEG, WebRTC video frame, data channel, or another format.
- [ ] Reject paths that require software H.264/H.265 encoding on ESP32-S3.
- [ ] Estimate per-frame copy count compared with current `/video-ws`.
- [ ] Estimate buffering behavior: latest-only vs queueing.

Decision point:

- If the stack cannot use JPEG/MJPEG without costly transcode, reject for this hardware.

## Phase 5: Minimal Integration Design

Only do this if Phases 1-4 pass.

The intended integration shape:

```text
Core 1: realtime/hardware
  control_task       unchanged
  camera_capture     unchanged or lightly adapted
  latest_frame_store publishes OV2640 JPEG

Core 0: network/delivery
  /ws control         unchanged
  /video-ws           kept as fallback until WebRTC proven
  :81/stream          kept as fallback
  rtc_video_task      consumes latest JPEG frames
  rtc signaling       local-only if needed
```

Rules:

- Do not move control into the RTC stack.
- Do not let RTC code call `track_drive_*` or `camera_tilt_*`.
- Do not let RTC video queues accumulate old frames.
- Keep `/video-ws` available behind a compile-time or runtime switch until RTC is validated.

## Phase 6: Main Branch Migration Criteria

Only migrate into `codex/fpv-architecture-refactor` if:

- [ ] Demo builds repeatably.
- [ ] AP-local browser workflow is proven.
- [ ] Video latency/smoothness is better than `/video-ws`.
- [ ] Control remains independent under video stress.
- [ ] Memory and CPU headroom are measured.
- [ ] The required code change is explainable and maintainable.

Migration strategy:

```powershell
git switch codex/fpv-architecture-refactor
git pull
git switch -c codex/rtc-video-integration
```

Then port only the proven minimal pieces. Do not merge the whole experiment branch blindly.

## Review Checklist For The Follow-Up Session

- [ ] Is the selected candidate still maintained and compatible with ESP-IDF 5.5.x?
- [ ] Does it support ESP32-S3 specifically?
- [ ] Does it solve AP-local browser video without unacceptable signaling complexity?
- [ ] Does it preserve Core 1 realtime/hardware ownership?
- [ ] Does it preserve `/ws` control independence?
- [ ] Does it preserve latest-frame semantics?
- [ ] Does it improve latency/smoothness enough to justify complexity?

