# Agent <-> Control Board Protocol (MVP)

This protocol is for the local Agent talking to the control board (ESP32-S3) over UART.

## Transport

- UART
- Recommended baud rate: `115200`
- Message framing: one JSON object per line (`\n` terminated)

## Agent -> Control Board Commands

### `drive`

```json
{"cmd":"drive","left":40,"right":40,"duration":1200}
```

- `left`: `-100..100`
- `right`: `-100..100`
- `duration`: milliseconds (optional)

### `turret`

```json
{"cmd":"turret","action":"left","duration":600}
```

- `action`: `left|right|stop`
- `duration`: milliseconds (optional)

### `barrel`

```json
{"cmd":"barrel","action":"up","duration":500}
```

- `action`: `up|down|stop`

### `fire`

```json
{"cmd":"fire"}
```

### `stop`

```json
{"cmd":"stop"}
```

### `status`

```json
{"cmd":"status"}
```

## Control Board -> Agent Responses

### Generic ACK

```json
{"ok":true,"cmd":"drive","state":"moving"}
```

### Error

```json
{"ok":false,"cmd":"drive","reason":"bad_args"}
```

### Status

```json
{"ok":true,"state":"idle","battery":7.6,"wifi_clients":1}
```

## Safety Requirements

- Unknown command must return `ok:false`.
- `stop` must always preempt any running action.
- Control board should enforce:
  - max speed clamp
  - action timeout
  - communication timeout auto-stop

## Parsing Rules

- Ignore empty lines.
- If JSON parse fails, return:

```json
{"ok":false,"reason":"invalid_json"}
```

- If required fields are missing, return:

```json
{"ok":false,"reason":"missing_field"}
```
