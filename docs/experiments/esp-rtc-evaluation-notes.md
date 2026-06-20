# ESP-RTC / ESP WebRTC Evaluation Notes

Date: 2026-06-20
Branch: `codex/esp-rtc-evaluation`

## Scope

This branch evaluates ESP-RTC / ESP WebRTC options only. It does not replace `/video-ws`, `:81/stream`, `/capture.jpg`, or `/ws`, and it does not change track or tilt control.

The stable integration branch remains `codex/fpv-architecture-refactor`.

## Current Baseline

Confirmed from `README.md`, `main/camera_stream.c`, and `main/web_server.c`:

| Item | Current value |
|---|---|
| Board target | ESP32-S3 |
| Control transport | WebSocket `/ws` |
| Control protocol | `tracks:<left>:<right>`, `tilt:<percent>`, `stop` |
| Primary video | Binary JPEG WebSocket `/video-ws` |
| Fallback video | HTTP MJPEG `http://192.168.4.1:81/stream` |
| Snapshot | `/capture.jpg` |
| Camera | OV2640 |
| Frame size | QVGA 320x240 |
| JPEG quality | 24 |
| Capture limit | 20 fps (`CAMERA_CAPTURE_INTERVAL_MS=50`) |
| Camera frame buffers | 2 when PSRAM is enabled |
| Latest frame cache | 48 KiB max, allocated in PSRAM when possible |
| Camera task | `camera_capture`, Core 1, priority 6 |
| Video WebSocket task | `video_ws`, Core 0, priority 4 |
| MJPEG accept task | `mjpeg_server`, Core 0, priority 4 |
| MJPEG client task | `mjpeg_client`, Core 0, priority 4 |
| Control task | `control_task`, Core 1, priority 12 |

Runtime-only items not measured in this session because no board/AP connection was used:

- Phone-visible latency.
- Observed `/video-ws` FPS in browser.
- Free heap / PSRAM after startup.
- Control responsiveness under live video.
- Slow or disconnected video client behavior.

## Candidate Matrix

| Candidate | ESP32-S3 | ESP-IDF 5.5.x | Browser peer | Signaling | AP-local | OV2640 JPEG/MJPEG path | H.264/H.265 risk | Status |
|---|---:|---:|---:|---|---|---|---|---|
| ESP-RTC announcement | Claimed around ESP32-S3-Korvo-2 | Not verified | Unclear; SIP/RTP style, not browser WebRTC-first | SIP-style signaling | Unclear | Announcement mentions MJPEG | Likely lower if MJPEG RTP is real | Postpone: no obvious public repo found |
| `esp-webrtc-solution` / `esp_peer` | Yes | Build-proven for `peer_demo`; docs commonly mention IDF v5.4/master, manifests allow `>=5.0` | Yes in product demos | AppRTC, HTTP/SSE local, WHIP, KVS, Janus, Kurento depending on demo | Yes for `doorbell_local` design | Component exposes MJPEG enum, but main browser video demo config uses H.264 for WebRTC send | High unless MJPEG-to-browser path is proven | Best candidate for future spike, but not ready to integrate |
| KVS WebRTC SDK for ESP | ESP32-S3 likely possible but not validated here | Not built | Browser via AWS KVS viewer/signaling | AWS KVS signaling | No for AP-local default | Unknown | High/cloud-oriented | Reject for this AP-local FPV use case |

## Official Source Checks

External source was cloned only under `scratch/`:

- `scratch/esp-webrtc-solution`
- Git branch: `main`
- Git commit: `8dc1d8c`

`scratch/` is ignored and should not be committed.

The separate GitHub URLs `espressif/esp-peer`, `espressif/esp-rtc`, and `espressif/esp_kvs_webrtc` did not resolve as public repositories during this session. `esp_peer` is available inside `esp-webrtc-solution` and through the Espressif component registry.

## Build Spike Results

### `esp-webrtc-solution/solutions/peer_demo`

Command:

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1
idf.py set-target esp32s3
idf.py build
```

Result: success.

Evidence:

- Built with ESP-IDF 5.5.3.
- Target: `esp32s3`.
- Output binary: `peer_demo.bin`.
- Binary size: `0x11f9e0` bytes, about 1.18 MiB.

Interpretation:

- `esp_peer` can compile for ESP32-S3 under the local IDF 5.5.3 environment.
- This is not an FPV browser-video proof. The demo is a two-board chat/data/audio style example using AppRTC signaling, not an AP-local browser viewer and not OV2640 JPEG video.

### `esp-webrtc-solution/solutions/doorbell_local`

Command:

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1
idf.py -DIDF_TARGET=esp32s3 build
```

Result: failed during dependency preparation/configure.

Observed failure:

- Component manager attempted to prepare `espressif/esp_video` 2.2.0.
- On Windows, copying cached `esp_video` example files failed with missing destination paths under long nested `examples/.../esp32-p4-function-ev-board-v1.x/...` directories.
- Configure stopped before a valid ESP32-S3 build was generated.

Earlier `idf.py set-target esp32s3; idf.py build` also showed the same dependency preparation failure, then a polluted retry fell back to default `esp32` and failed on the demo's 4.9 MiB partition table versus default 2 MiB flash. The explicit `-DIDF_TARGET=esp32s3` run is the cleaner result to rely on.

Interpretation:

- `doorbell_local` is the closest official AP-local browser candidate because it hosts signaling on the ESP over HTTPS and serves a browser test URL.
- It is not currently build-proven in this workspace on Windows + IDF 5.5.3.
- Its default hardware path targets ESP32-P4 Function EV Board and S3 Korvo-style board support, not the current custom OV2640 pinout.
- Code inspection shows `media_sys.c` can configure capture as MJPEG, but `webrtc.c` configures the outgoing peer video codec and capture sink as H.264:
  - `ESP_PEER_VIDEO_CODEC_H264`
  - `ESP_CAPTURE_FMT_ID_H264`
- That means the official browser-facing doorbell path does not prove OV2640 JPEG pass-through.

## Candidate Selection

Selected for any next spike: `esp-webrtc-solution` with `esp_peer`, starting from `doorbell_local` concepts rather than direct firmware integration.

Reason:

- It is the only candidate with an official local browser signaling pattern.
- It has an ESP32-S3 build-proven lower-level peer component via `peer_demo`.
- It keeps WebRTC as a Core 0 delivery concern conceptually, so `/ws` control can remain independent.

But it is not ready for integration because:

- The closest browser video demo failed to build in this workspace.
- The official local browser demo appears H.264-oriented for WebRTC output.
- A direct OV2640 JPEG/MJPEG-to-browser WebRTC path remains unproven.
- The demo has significant media/audio/board dependencies that are heavier than the current FPV stack.

## AP-Local Browser Feasibility

`doorbell_local` is promising for signaling only:

- ESP acts as HTTPS signaling server.
- Browser opens a local URL printed by the device.
- Signaling uses SSE and HTTP POST.
- It is designed for local peer setup without an external signaling server.

Risks:

- The README says Chrome/Edge may require disabling mDNS ICE candidates so local IP candidates are visible. That is a poor default workflow for a phone FPV UI.
- HTTPS with a self-signed certificate requires manual browser trust.
- Only one peer is supported.
- This has not been validated in ESP AP mode in this session.

Conclusion: AP-local signaling is plausible, but not yet acceptable for the Heavy Punch main branch until browser setup is simpler and AP-mode operation is proven.

## Camera Source Feasibility

The current project produces OV2640 JPEG frames and can stream MJPEG without H.264/H.265. That is the correct shape for ESP32-S3.

`esp_peer` exposes:

- `ESP_PEER_VIDEO_CODEC_MJPEG`
- `ESP_PEER_VIDEO_CODEC_H264`

However, the closest official browser demo (`doorbell_local`) configures H.264 for WebRTC output. A MJPEG enum alone is not enough to prove modern browser compatibility because browser WebRTC video codec support is normally negotiated through SDP and common browser codecs are VP8/VP9/H.264/AV1, not MJPEG.

Rejection condition for main-branch migration:

- If browser viewing requires software H.264/H.265 encoding on ESP32-S3, reject and keep the current JPEG WebSocket/MJPEG design.

## CPU / Heap / PSRAM Risk

Known risk from the build and component graph:

- Even `peer_demo` pulls a large component graph including `esp_capture`, `av_render`, codecs, SRTP, DTLS, SCTP, and media libraries.
- `esp_peer` README examples show video jitter/cache/send pool defaults in the hundreds of KiB range unless tuned down.
- `doorbell_local` adds HTTPS server, signaling, audio/media board support, and optional pedestrian detection.
- Any RTP retransmission/jitter queue can violate latest-frame semantics unless explicitly configured or bypassed.

For Heavy Punch, RTC/WebRTC must stay on Core 0 and must consume latest JPEG frames only. It must never call control APIs or queue stale video.

## Decision

Do not migrate ESP-RTC / ESP WebRTC into `codex/fpv-architecture-refactor` now.

Keep the current baseline:

- `/video-ws` binary JPEG as primary.
- `:81/stream` MJPEG fallback.
- `/capture.jpg` snapshot.
- `/ws` control independent from video.

Continue only with isolated RTC/WebRTC spikes if useful.

## Proposed Next Spike

The next useful spike should be smaller than `doorbell_local`:

1. Create a minimal local signaling proof using the `doorbell_local` HTTP/SSE signaling approach.
2. Use `esp_peer` directly with `ESP_PEER_VIDEO_CODEC_MJPEG`.
3. Feed synthetic JPEG/MJPEG frames first, not the real camera.
4. Test against a browser page in local LAN/AP mode.
5. Confirm whether the browser accepts/decodes MJPEG over WebRTC.
6. If MJPEG is not accepted by browser WebRTC, stop the RTC path for ESP32-S3 FPV.

Success criteria for continuing:

- ESP32-S3 build succeeds under ESP-IDF 5.5.3.
- Phone browser can connect AP-local without cloud signaling.
- No browser flag changes are required.
- No software H.264/H.265 encoding is required.
- Video send can be latest-only with bounded buffers.
- `/ws` control remains separate and responsive.

Failure criteria:

- Requires cloud/LAN signaling server for basic viewing.
- Requires browser flags or native app.
- Requires software H.264/H.265.
- Requires large media queues or retransmission buffers that build latency.
- Cannot be built repeatably under IDF 5.5.x for ESP32-S3.

