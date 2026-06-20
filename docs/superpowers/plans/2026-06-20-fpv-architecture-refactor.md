# FPV Architecture Refactor Plan

> **For the next Codex session:** continue on branch `codex/fpv-architecture-refactor`.
> This plan is intentionally implementation-oriented. Keep the first phase focused on architecture boundaries and core affinity; do not introduce ESP-RTC or UDP in the same pass.

## Goal

Refactor the FPV firmware so video and control are isolated by design:

- Video stalls, slow clients, JPEG decode delays, or MJPEG fallback must not block track or tilt control.
- Core 1 is the realtime/hardware core.
- Core 0 is the network/delivery core.
- Video uses a producer-consumer model with latest-frame semantics: no old-frame queues, no backlog chasing.
- The first implementation phase keeps the current protocols:
  - Control WebSocket: `/ws`
  - Primary video: binary JPEG WebSocket `/video-ws`
  - Fallback video: HTTP MJPEG `:81/stream`
  - Snapshot: `/capture.jpg`

## Current State

| Area | Current Location | Current Core | Priority | Issue |
|---|---|---:|---:|---|
| HTTP server | `main/web_server.c:web_server_start()` | 0 | 5 | Good: already network-side |
| `/ws` control | `main/web_server.c:ws_handler()` | 0 | httpd task | Bad: directly calls motor and servo modules |
| `/video-ws` | `main/web_server.c:video_ws_task()` | 0 | 4 | Mostly good: Core 0, per-client scratch, timeout |
| Camera capture | `main/camera_stream.c:camera_capture_task()` | 1 | 6 | Good direction, but shares latest-frame mutex with network consumers |
| MJPEG accept | `main/camera_stream.c:stream_server_task()` | 1 | 4 | Bad: socket accept belongs on Core 0 |
| MJPEG client | `main/camera_stream.c:stream_client_task()` | 0 | 4 | Good direction |
| Drive update | `main/app_main.c:drive_task()` | unpinned | 12 | Bad: hardware realtime task is not pinned |
| Track hardware | `main/track_drive.c` | caller core | n/a | Bad: Core 0 handler writes targets while drive task reads them |
| Tilt servo | `main/camera_tilt.c` | caller core | n/a | Bad: Core 0 handler writes LEDC directly |

## Target Architecture

```text
Core 0: Network / Delivery
  esp_http_server task
    GET /             -> serve embedded UI
    GET /capture.jpg  -> copy latest JPEG snapshot
    WS  /ws           -> parse only, submit latest control command
    WS  /video-ws     -> spawn per-client latest-frame sender

  MJPEG fallback :81
    accept task       -> Core 0
    client tasks      -> Core 0

  Rules:
    - never call track_drive_* or camera_tilt_* directly
    - never hold control locks
    - never queue old video frames
    - slow video clients timeout/disconnect

Cross-core boundary:
  control command handoff
    Core 0 -> Core 1
    latest state semantics
    track and tilt commands must not block network handlers
    STOP must have priority

  latest JPEG store
    Core 1 -> Core 0
    camera producer publishes latest complete JPEG
    network consumers copy latest complete JPEG
    no multi-frame backlog

Core 1: Realtime / Hardware
  control_task, high priority
    owns track motor PWM
    owns camera tilt servo PWM
    consumes latest control state
    enforces command timeout / safety stop
    runs motor slew update

  camera_capture_task, lower priority
    owns esp_camera_fb_get()
    fixed capture cadence
    publishes latest JPEG
    never waits on network
```

## Phase 1: Enforce Core And Control Boundaries

**Purpose:** make video unable to block hardware control without changing the external protocol.

### Files

- Add: `main/control_loop.h`
- Add: `main/control_loop.c`
- Modify: `main/CMakeLists.txt`
- Modify: `main/app_main.c`
- Modify: `main/web_server.c`
- Modify: `main/camera_stream.c`

### Tasks

- [ ] Add `control_loop.h/.c`.
- [ ] Move the realtime control task into `control_loop.c`.
- [ ] Pin `control_task` to Core 1 with priority `12`.
- [ ] Make `control_task` the only code path that calls:
  - `track_drive_set_percent()`
  - `track_drive_stop()`
  - `track_drive_update()`
  - `camera_tilt_set_percent()`
- [ ] Delete the unpinned `drive_task` from `main/app_main.c`.
- [ ] Replace `xTaskCreate(drive_task, ...)` with `control_loop_start()`.
- [ ] Remove direct hardware includes from `main/web_server.c`:
  - `camera_tilt.h`
  - `track_drive.h`
- [ ] Keep `track_math.h` in `web_server.c` for parsing `tracks:<left>:<right>`.
- [ ] Change `dispatch_control_message()` so it only submits commands to `control_loop`.
- [ ] Move `stream_server_task` from Core 1 to Core 0 in `camera_stream_start()`.
- [ ] Add startup logs that print actual task core IDs for:
  - `control_task`
  - `camera_capture`
  - `video_ws`
  - `mjpeg_server`
  - `mjpeg_client`

### Recommended Control API

```c
typedef struct {
  int16_t left;
  int16_t right;
} control_tracks_t;

esp_err_t control_loop_init(void);
esp_err_t control_loop_start(void);
bool control_loop_submit_tracks(int16_t left, int16_t right);
bool control_loop_submit_tilt(int16_t percent);
void control_loop_submit_stop(void);
```

### Recommended Internals

Use latest-state handoff, not FIFO command history:

- `tracks_queue`: length 1, `xQueueOverwrite()`
- `tilt_queue`: length 1, `xQueueOverwrite()`
- `stop_event`: event bit or direct-to-task notification

Do not use one shared queue for both tracks and tilt if `xQueueOverwrite()` is used; tilt could overwrite a track command. Track, tilt, and stop have different semantics.

Control task loop:

```text
loop every 5 ms:
  if stop_event:
    track_drive_stop()
    clear stop_event

  drain latest tracks_queue:
    track_drive_set_percent(left, right)

  drain latest tilt_queue:
    camera_tilt_set_percent(percent)

  track_drive_update()
```

Keep track timeout enforcement inside `track_drive_update()` as it is today.

## Phase 2: Stabilize Video Producer-Consumer Semantics

**Purpose:** make capture and delivery independent, and make video smooth by dropping old frames instead of chasing them.

### Files

- Modify first: `main/camera_stream.c`
- Optional later:
  - Add `main/latest_frame_store.h`
  - Add `main/latest_frame_store.c`

### Tasks

- [ ] Keep one camera producer: only `camera_capture_task()` calls `esp_camera_fb_get()`.
- [ ] Keep network consumers as read-only users of latest JPEG.
- [ ] Ensure producer never waits on socket/network work.
- [ ] Keep consumer behavior as latest-only:
  - each consumer tracks `last_seq`
  - duplicate seq is skipped
  - missed seq values are counted as dropped/skipped frames, not resent
- [ ] Consider changing producer publish lock from `50 ms` wait to fail-fast or very short wait.
- [ ] Keep network send outside latest-frame mutex. This is already correct.
- [ ] Add lightweight counters:
  - captured frames
  - published frames
  - oversized drops
  - lock-busy drops
  - latest JPEG size
  - video WS sent frames
  - video WS skipped frames
  - send failures

### Important Design Note

The current single-buffer latest-frame store is simple and safe because copy happens under mutex. It may be enough for QVGA. Do not jump straight to a lock-free or triple-buffer implementation unless metrics show mutex contention.

If optimizing later, prefer a dedicated `latest_frame_store.c` abstraction. Avoid a normal frame queue.

## Phase 3: Pace Video Delivery And Frontend Rendering

**Purpose:** make video look smoother on the phone, not merely maximize raw frame sending.

### Backend Tasks

- [ ] Add an explicit `VIDEO_WS_TARGET_FPS` or use the capture FPS as the sender cadence.
- [ ] Make `/video-ws` sender paced:
  - wait for send tick
  - copy latest frame
  - skip duplicate seq
  - send with timeout
  - disconnect slow client on send failure
- [ ] Add a max client count for `/video-ws`, similar to `CAMERA_MAX_STREAM_CLIENTS`.
- [ ] Do not allocate frame queues per video client.

### Frontend Tasks

- [ ] Change `main/web_ui.h` video rendering to latest-pending semantics:
  - WebSocket `onmessage` stores only the latest pending Blob.
  - If an image decode/render is already in flight, do not start another.
  - When `img.onload` fires, render the latest pending frame if present.
  - Revoke stale Blob URLs.
- [ ] Keep UI controls responsive even when frames arrive quickly.

## Phase 4: Split Large Modules

**Purpose:** make ownership explicit once the first refactor is stable.

Current `main/camera_stream.c` owns too many responsibilities:

- OV2640 pins/config/tuning
- capture task
- latest-frame cache
- snapshot handler
- raw MJPEG socket server

Recommended split:

- `main/camera_capture.c/.h`
  - camera pins and config
  - `camera_capture_init()`
  - `camera_capture_start()`
  - `camera_capture_task()`
- `main/latest_frame_store.c/.h`
  - latest JPEG cache
  - wait/copy/publish APIs
- `main/video_mjpeg.c/.h`
  - port 81 fallback server
- Optional `main/video_ws.c/.h`
  - `/video-ws` client task if `web_server.c` grows too large

Keep this as a later phase. The first commit should prioritize boundaries, not cosmetic file movement.

## Phase 5: Parameterization And Experiments

**Purpose:** make video tuning measurable.

- [ ] Centralize video parameters:
  - frame size: QVGA/VGA/SVGA
  - JPEG quality
  - capture FPS
  - WebSocket send FPS
  - fb_count
- [ ] Keep the default conservative at first:
  - QVGA
  - quality around `24`
  - 15-20 FPS
  - `CAMERA_GRAB_LATEST`
- [ ] Treat `fb_count = 1` as an experiment, not a default truth.
- [ ] Do not introduce ESP-RTC in this phase.

## ESP-RTC Position

ESP-RTC is a later experimental transport direction, not Phase 1.

Reason:

- The immediate architecture problem is task ownership, core isolation, and backpressure control.
- ESP-RTC would change the transport layer and reintroduce significant complexity.
- A clean Core 0/Core 1 boundary will make ESP-RTC easier to evaluate later if needed.

Create a separate experiment branch only after Phase 1-3 are stable.

## Verification

### Build

```powershell
. C:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1
idf.py build
```

### Runtime Logs

After flashing, confirm logs show:

- `control_task` running on Core 1
- `camera_capture` running on Core 1
- HTTP server configured on Core 0
- `video_ws` running on Core 0
- `mjpeg_server` running on Core 0
- `mjpeg_client` running on Core 0

### Functional Tests

- [ ] Open the UI and verify `/ws` control works without opening video.
- [ ] Move both track levers quickly while `/video-ws` is connected.
- [ ] Confirm video connection/disconnection does not affect steering.
- [ ] Trigger browser blur/hidden/refresh and confirm tracks stop within `350 ms`.
- [ ] Use `CENTER` and verify tilt returns to `0%`.
- [ ] Open MJPEG fallback and verify control remains responsive.
- [ ] Create a slow or overloaded video client and verify:
  - video may drop/close
  - control remains responsive
  - Core 1 tasks do not block on socket send

### Regression Checks

- [ ] No WebRTC reintroduction in Phase 1.
- [ ] No UDP transport in Phase 1.
- [ ] No normal FIFO video frame queues.
- [ ] No direct `track_drive_*` or `camera_tilt_*` calls from `web_server.c`.
- [ ] No unpinned realtime/hardware task.

