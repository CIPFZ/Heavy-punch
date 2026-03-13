# AI Board <-> Agent Protocol (MVP)

This protocol is for the AI board (ESP32-S3) talking to the local Agent over WebSocket.

## Transport

- WebSocket
- Direction:
  - AI board -> Agent
  - Agent -> AI board
- Message encoding:
  - JSON text frames for control/status
  - Base64 audio payload in JSON for MVP simplicity

## Common Envelope

All JSON messages should include:

```json
{
  "type": "message_type",
  "ts": 1730000000
}
```

- `type`: required
- `ts`: optional unix timestamp (seconds)

## AI Board -> Agent

### `hello`

```json
{
  "type": "hello",
  "device_id": "tank-ai-001",
  "fw": "0.1.0"
}
```

### `status`

```json
{
  "type": "status",
  "wifi": true,
  "recording": false,
  "battery": 7.6
}
```

### `listen_start`

```json
{
  "type": "listen_start",
  "session_id": "sess-001"
}
```

### `audio_chunk`

```json
{
  "type": "audio_chunk",
  "session_id": "sess-001",
  "format": "pcm16",
  "sample_rate": 16000,
  "data": "<base64>"
}
```

### `listen_stop`

```json
{
  "type": "listen_stop",
  "session_id": "sess-001"
}
```

## Agent -> AI Board

### `hello_ack`

```json
{
  "type": "hello_ack",
  "device_id": "tank-ai-001",
  "server": "local-agent",
  "version": "0.1.0"
}
```

### `asr_result`

```json
{
  "type": "asr_result",
  "session_id": "sess-001",
  "text": "forward for two seconds"
}
```

### `llm_text`

```json
{
  "type": "llm_text",
  "session_id": "sess-001",
  "text": "OK, moving forward for two seconds."
}
```

### `tts_chunk`

```json
{
  "type": "tts_chunk",
  "session_id": "sess-001",
  "format": "mp3",
  "data": "<base64>"
}
```

### `tts_end`

```json
{
  "type": "tts_end",
  "session_id": "sess-001"
}
```

### `intent`

```json
{
  "type": "intent",
  "name": "drive",
  "args": {
    "left": 40,
    "right": 40,
    "duration": 2000
  }
}
```

### `error`

```json
{
  "type": "error",
  "code": "asr_failed",
  "message": "ASR provider timeout"
}
```

## Reliability Rules

- AI board sends `audio_chunk` in order.
- Agent processes messages per `session_id`.
- AI board should send `listen_stop` to close a recording session.
- Agent should always answer terminally with either:
  - `llm_text` + optional `tts_*`
  - `error`

## Security Notes

- This MVP assumes trusted local network.
- Production version should add:
  - device token
  - optional TLS
  - replay protection (nonce / signature)
