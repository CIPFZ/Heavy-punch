# AI Board Minimal Bring-up (xiaozhi-esp32)

This project now includes a custom board target:

- `vendor/xiaozhi-esp32/main/boards/tank-ai-s3`

## What is done

- Added `tank-ai-s3` board type for ESP32-S3 with your exact pins:
  - INMP441: WS=4, SCK=5, SD=6
  - MAX98357A: DIN=7, BCLK=15, LRC=16
  - OLED I2C: SDA=41, SCL=42
- Added local websocket-first startup logic:
  - If NVS `websocket.url` exists, device uses websocket protocol directly.
  - OTA check no longer blocks startup with long retries in this local mode.
- Added compile-time default websocket config in `config.h`:
  - `DEFAULT_WEBSOCKET_URL`
  - `DEFAULT_WEBSOCKET_TOKEN`
  - `DEFAULT_WEBSOCKET_VERSION`

## How to use now

1. Edit:
   - `vendor/xiaozhi-esp32/main/boards/tank-ai-s3/config.h`
2. Set:
   - `DEFAULT_WEBSOCKET_URL` (example: `ws://192.168.1.100:8000/xiaozhi`)
   - `DEFAULT_WEBSOCKET_TOKEN` (if server requires auth)
   - `DEFAULT_WEBSOCKET_VERSION` (`1/2/3`, server-dependent)
3. Build and flash `tank-ai-s3`.
4. First boot writes websocket defaults to NVS and uses them directly.

## Local agent (this repo)

1. Copy:
   - `agent/config.example.json` -> `agent/config.json`
2. Start:
   - `python agent/app.py`
3. Open config page:
   - `http://<agent-ip>:18080`
4. Fill your OpenAI-compatible LLM config (`base_url/api_key/model`) and save.
5. Keep board websocket URL pointing to this agent (`ws://<agent-ip>:8765`).

## Important compatibility note

`xiaozhi-esp32` uses its own websocket message protocol (`hello/listen/tts/stt/mcp` flow).
Your backend must be xiaozhi-protocol compatible.

Current status of this minimal agent:

- It supports xiaozhi websocket handshake and message flow.
- It decodes upstream Opus frames with `opuslib`, then calls configured ASR.
- It sends downstream TTS as Opus binary frames (with protocol v1/v2/v3 packet format).
- If Opus/ASR/TTS is not available, it falls back to text placeholder behavior.
