import asyncio
import copy
import io
import json
import threading
import time
import uuid
import wave
from array import array
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional

import websockets

try:
    import serial  # type: ignore
except Exception:
    serial = None

try:
    from openai import OpenAI
except Exception:
    OpenAI = None  # type: ignore

try:
    import opuslib  # type: ignore
except Exception:
    opuslib = None  # type: ignore


ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "config.json"
EXAMPLE_CONFIG_PATH = ROOT / "config.example.json"


def now_ts() -> int:
    return int(time.time())


def ensure_default_config() -> Dict[str, Any]:
    if CONFIG_PATH.exists():
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    if not EXAMPLE_CONFIG_PATH.exists():
        raise FileNotFoundError("Missing agent/config.example.json")
    data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
    CONFIG_PATH.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    return data


class ConfigStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._cfg = ensure_default_config()

    def get(self) -> Dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._cfg)

    def save(self, cfg: Dict[str, Any]) -> None:
        with self._lock:
            self._cfg = copy.deepcopy(cfg)
            CONFIG_PATH.write_text(json.dumps(self._cfg, ensure_ascii=False, indent=2), encoding="utf-8")


class AgentStats:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._data: Dict[str, Any] = {
            "connected_clients": 0,
            "sessions_started": 0,
            "sessions_completed": 0,
            "sessions_aborted": 0,
            "last_stt": "",
            "last_llm": "",
            "last_error": "",
            "last_event": "boot",
            "last_event_ts": now_ts(),
        }

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            return copy.deepcopy(self._data)

    def set_connected(self, count: int) -> None:
        with self._lock:
            self._data["connected_clients"] = max(0, int(count))
            self._data["last_event"] = "client_count_changed"
            self._data["last_event_ts"] = now_ts()

    def add_connected(self, delta: int) -> None:
        with self._lock:
            cur = int(self._data.get("connected_clients", 0))
            self._data["connected_clients"] = max(0, cur + int(delta))
            self._data["last_event"] = "client_count_changed"
            self._data["last_event_ts"] = now_ts()

    def inc(self, key: str) -> None:
        with self._lock:
            self._data[key] = int(self._data.get(key, 0)) + 1
            self._data["last_event"] = key
            self._data["last_event_ts"] = now_ts()

    def set_text(self, key: str, value: str) -> None:
        with self._lock:
            self._data[key] = (value or "")[:500]
            self._data["last_event"] = key
            self._data["last_event_ts"] = now_ts()

    def set_error(self, value: str) -> None:
        self.set_text("last_error", value)


class ControlBridge:
    @staticmethod
    def send(cfg: Dict[str, Any], cmd: Dict[str, Any]) -> Optional[Dict[str, Any]]:
        board_cfg = cfg.get("control_board", {})
        if not board_cfg.get("enabled", False):
            return None
        if serial is None:
            return {"ok": False, "reason": "pyserial_not_installed"}

        port = str(board_cfg.get("port", "")).strip()
        baud = int(board_cfg.get("baud", 115200))
        if not port:
            return {"ok": False, "reason": "serial_port_empty"}

        line = json.dumps(cmd, ensure_ascii=True) + "\n"
        try:
            with serial.Serial(port, baud, timeout=0.4) as ser:  # type: ignore[attr-defined]
                ser.write(line.encode("utf-8"))
                resp = ser.readline()
        except Exception as e:
            return {"ok": False, "reason": f"serial_error:{e}"}

        if not resp:
            return {"ok": False, "reason": "no_board_response"}
        try:
            return json.loads(resp.decode("utf-8", errors="ignore").strip())
        except Exception:
            return {"ok": False, "reason": "invalid_board_response"}


class LlmClient:
    def __init__(self, cfg: Dict[str, Any]):
        self.base_url = str(cfg.get("base_url", "")).strip()
        self.api_key = str(cfg.get("api_key", "")).strip()
        self.model = str(cfg.get("model", "")).strip()
        self.client = None
        if OpenAI and self.base_url and self.api_key and self.model:
            self.client = OpenAI(base_url=self.base_url, api_key=self.api_key)

    def chat(self, text: str) -> str:
        if not text:
            return "I did not catch that. Please repeat."
        if self.client is None:
            return f"[llm_not_configured] received: {text}"
        try:
            resp = self.client.chat.completions.create(
                model=self.model,
                messages=[
                    {"role": "system", "content": "You are a concise voice assistant."},
                    {"role": "user", "content": text},
                ],
                temperature=0.3,
            )
            msg = resp.choices[0].message.content or ""
            return msg.strip() or "Received."
        except Exception as e:
            return f"[llm_error] {e}"


class AsrClient:
    def __init__(self, cfg: Dict[str, Any]):
        self.provider = str(cfg.get("provider", "openai")).strip()
        self.base_url = str(cfg.get("base_url", "")).strip()
        self.api_key = str(cfg.get("api_key", "")).strip()
        self.model = str(cfg.get("model", "")).strip()
        self.client = None
        if (
            self.provider == "openai"
            and OpenAI
            and self.base_url
            and self.api_key
            and self.model
        ):
            self.client = OpenAI(base_url=self.base_url, api_key=self.api_key)

    @staticmethod
    def pcm16_to_wav_bytes(pcm: bytes, sample_rate: int = 16000, channels: int = 1) -> bytes:
        bio = io.BytesIO()
        with wave.open(bio, "wb") as wf:
            wf.setnchannels(channels)
            wf.setsampwidth(2)
            wf.setframerate(sample_rate)
            wf.writeframes(pcm)
        return bio.getvalue()

    def transcribe_pcm16(self, pcm: bytes, sample_rate: int = 16000, channels: int = 1) -> str:
        if not pcm:
            return ""
        if self.client is None:
            return "[asr_not_configured]"
        try:
            wav_bytes = self.pcm16_to_wav_bytes(pcm, sample_rate=sample_rate, channels=channels)
            file_obj = io.BytesIO(wav_bytes)
            file_obj.name = "speech.wav"
            resp = self.client.audio.transcriptions.create(
                model=self.model,
                file=file_obj,
            )
            text = getattr(resp, "text", "")
            return str(text).strip()
        except Exception as e:
            return f"[asr_error] {e}"


class TtsClient:
    def __init__(self, cfg: Dict[str, Any]):
        self.provider = str(cfg.get("provider", "openai")).strip()
        self.base_url = str(cfg.get("base_url", "")).strip()
        self.api_key = str(cfg.get("api_key", "")).strip()
        self.model = str(cfg.get("model", "")).strip()
        self.voice = str(cfg.get("voice", "alloy")).strip()
        self.format = str(cfg.get("format", "wav")).strip().lower()
        self.client = None
        if (
            self.provider == "openai"
            and OpenAI
            and self.base_url
            and self.api_key
            and self.model
        ):
            self.client = OpenAI(base_url=self.base_url, api_key=self.api_key)

    def synthesize(self, text: str) -> bytes:
        if not text:
            return b""
        if self.client is None:
            return b""
        try:
            resp = self.client.audio.speech.create(
                model=self.model,
                voice=self.voice,
                input=text,
                format=self.format,
            )
            if hasattr(resp, "read"):
                return resp.read()
            if hasattr(resp, "content"):
                return resp.content
            if isinstance(resp, (bytes, bytearray)):
                return bytes(resp)
            return b""
        except Exception:
            return b""

    @staticmethod
    def wav_to_pcm16_mono_16k(wav_bytes: bytes) -> bytes:
        if not wav_bytes:
            return b""
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            channels = wf.getnchannels()
            sample_width = wf.getsampwidth()
            src_rate = wf.getframerate()
            pcm = wf.readframes(wf.getnframes())

        if sample_width != 2:
            return b""
        if channels == 2:
            # Downmix stereo to mono using pure Python.
            samples = array("h")
            samples.frombytes(pcm)
            mono = array("h")
            for i in range(0, len(samples), 2):
                l = samples[i]
                r = samples[i + 1] if i + 1 < len(samples) else l
                mono.append(int((l + r) / 2))
            pcm = mono.tobytes()
            channels = 1
        if channels != 1:
            return b""
        if src_rate != 16000:
            # Minimal fallback: skip unsupported sample rate conversion.
            return b""
        return pcm


class OpusDecoder:
    @staticmethod
    def decode_frames(
        frames: list[bytes],
        sample_rate: int = 16000,
        channels: int = 1,
        frame_duration_ms: int = 60,
    ) -> bytes:
        if not frames:
            return b""
        if opuslib is None:
            raise RuntimeError("opuslib_not_installed")
        decoder = opuslib.Decoder(sample_rate, channels)
        frame_size = int(sample_rate * frame_duration_ms / 1000)
        out = bytearray()
        for frame in frames:
            if not frame:
                continue
            pcm = decoder.decode(frame, frame_size, decode_fec=False)
            out.extend(pcm)
        return bytes(out)


class OpusEncoder:
    @staticmethod
    def encode_pcm16(
        pcm: bytes,
        sample_rate: int = 16000,
        channels: int = 1,
        frame_duration_ms: int = 60,
    ) -> list[bytes]:
        if not pcm:
            return []
        if opuslib is None:
            raise RuntimeError("opuslib_not_installed")
        encoder = opuslib.Encoder(sample_rate, channels, "voip")
        frame_samples = int(sample_rate * frame_duration_ms / 1000)
        frame_bytes = frame_samples * channels * 2
        frames: list[bytes] = []
        idx = 0
        total = len(pcm)
        while idx < total:
            chunk = pcm[idx:idx + frame_bytes]
            idx += frame_bytes
            if len(chunk) < frame_bytes:
                chunk = chunk + (b"\x00" * (frame_bytes - len(chunk)))
            encoded = encoder.encode(chunk, frame_samples)
            frames.append(encoded)
        return frames


def infer_intent(text: str) -> Optional[Dict[str, Any]]:
    t = text.lower().strip()
    if not t:
        return None
    if "stop" in t:
        return {"name": "stop", "args": {}}
    if "fire" in t:
        return {"name": "fire", "args": {}}
    if "forward" in t:
        return {"name": "drive", "args": {"left": 40, "right": 40, "duration": 1200}}
    if "back" in t:
        return {"name": "drive", "args": {"left": -40, "right": -40, "duration": 1200}}
    if "left" in t:
        return {"name": "drive", "args": {"left": -30, "right": 30, "duration": 800}}
    if "right" in t:
        return {"name": "drive", "args": {"left": 30, "right": -30, "duration": 800}}
    return None


def parse_audio_payload(raw: bytes, proto_version: int) -> bytes:
    if proto_version == 2:
        if len(raw) < 16:
            return b""
        payload_size = int.from_bytes(raw[12:16], "big", signed=False)
        return raw[16:16 + payload_size]
    if proto_version == 3:
        if len(raw) < 4:
            return b""
        payload_size = int.from_bytes(raw[2:4], "big", signed=False)
        return raw[4:4 + payload_size]
    return raw


def pack_audio_payload(payload: bytes, proto_version: int) -> bytes:
    if proto_version == 2:
        version = (2).to_bytes(2, "big", signed=False)
        ptype = (0).to_bytes(2, "big", signed=False)
        reserved = (0).to_bytes(4, "big", signed=False)
        timestamp = (0).to_bytes(4, "big", signed=False)
        size = len(payload).to_bytes(4, "big", signed=False)
        return version + ptype + reserved + timestamp + size + payload
    if proto_version == 3:
        ptype = (0).to_bytes(1, "big", signed=False)
        reserved = (0).to_bytes(1, "big", signed=False)
        size = len(payload).to_bytes(2, "big", signed=False)
        return ptype + reserved + size + payload
    return payload


def split_sentences(text: str) -> list[str]:
    if not text:
        return []
    parts: list[str] = []
    buf = []
    seps = set(".!?;,\n")
    for ch in text:
        buf.append(ch)
        if ch in seps:
            s = "".join(buf).strip()
            if s:
                parts.append(s)
            buf = []
    tail = "".join(buf).strip()
    if tail:
        parts.append(tail)
    return parts or [text]


async def send_json(ws: websockets.WebSocketServerProtocol, payload: Dict[str, Any]) -> None:
    payload.setdefault("ts", now_ts())
    await ws.send(json.dumps(payload, ensure_ascii=False))


class ConfigHttpHandler(BaseHTTPRequestHandler):
    store: ConfigStore = None  # type: ignore
    stats: AgentStats = None  # type: ignore

    def _send_json(self, data: Dict[str, Any], status: int = 200) -> None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_json_body(self) -> Dict[str, Any]:
        size = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(size) if size > 0 else b"{}"
        return json.loads(raw.decode("utf-8"))

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/api/config":
            self._send_json({"ok": True, "config": self.store.get()})
            return
        if self.path == "/api/status":
            self._send_json({"ok": True, "status": self.stats.snapshot()})
            return
        if self.path == "/" or self.path == "/index.html":
            page = build_config_page_html().encode("utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            self.wfile.write(page)
            return
        self.send_response(HTTPStatus.NOT_FOUND)
        self.end_headers()

    def do_POST(self) -> None:  # noqa: N802
        if self.path == "/api/config":
            try:
                data = self._read_json_body()
                cfg = data.get("config", {})
                if not isinstance(cfg, dict):
                    raise ValueError("config must be object")
                self.store.save(cfg)
                self._send_json({"ok": True})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 400)
            return
        if self.path == "/api/test-llm":
            try:
                data = self._read_json_body()
                text = str(data.get("text", "")).strip()
                cfg = self.store.get()
                llm = LlmClient(cfg.get("llm", {}))
                reply = llm.chat(text)
                self._send_json({"ok": True, "reply": reply})
            except Exception as e:
                self._send_json({"ok": False, "error": str(e)}, 400)
            return
        self.send_response(HTTPStatus.NOT_FOUND)
        self.end_headers()

    def log_message(self, fmt: str, *args: Any) -> None:
        return


def build_config_page_html() -> str:
    return """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" />
  <title>AI Agent Config</title>
  <style>
    body { font-family: "Segoe UI", sans-serif; margin: 24px; background:#f5f7fb; color:#222; }
    .card { max-width: 980px; background:#fff; border-radius:12px; padding:20px; box-shadow:0 6px 20px rgba(0,0,0,.08); }
    h1 { margin-top:0; font-size: 22px; }
    textarea { width:100%; min-height:380px; font-family: Consolas, monospace; font-size:13px; }
    input[type=text] { width:100%; padding:8px; }
    .row { display:flex; gap:12px; margin-top:12px; flex-wrap:wrap; }
    .status { margin-top:12px; padding:10px; background:#f1f4f9; border-radius:8px; font-family: Consolas, monospace; font-size:12px; white-space:pre-wrap; }
    button { padding:10px 14px; border:none; border-radius:8px; background:#0d6efd; color:#fff; cursor:pointer; }
    button.secondary { background:#6c757d; }
    .log { margin-top:10px; font-size:13px; color:#333; white-space:pre-wrap; }
  </style>
</head>
<body>
  <div class="card">
    <h1>AI Agent Config</h1>
    <div>Edit JSON config and click Save. New sessions use latest config.</div>
    <div class="row">
      <button id="reloadBtn" class="secondary">Reload</button>
      <button id="saveBtn">Save Config</button>
    </div>
    <div class="row">
      <input id="testText" type="text" placeholder="Type a test prompt" />
      <button id="testBtn" class="secondary">Test LLM</button>
    </div>
    <div id="status" class="status"></div>
    <textarea id="cfg"></textarea>
    <div class="log" id="log"></div>
  </div>
  <script>
    const log = (m) => document.getElementById('log').textContent = m;
    async function loadCfg() {
      const res = await fetch('/api/config');
      const data = await res.json();
      if (!data.ok) throw new Error('load failed');
      document.getElementById('cfg').value = JSON.stringify(data.config, null, 2);
      log('Config loaded');
    }
    async function saveCfg() {
      const txt = document.getElementById('cfg').value;
      const cfg = JSON.parse(txt);
      const res = await fetch('/api/config', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({config: cfg})
      });
      const data = await res.json();
      if (!data.ok) throw new Error(data.error || 'save failed');
      log('Config saved');
    }
    async function testLlm() {
      const text = document.getElementById('testText').value.trim();
      const res = await fetch('/api/test-llm', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({text})
      });
      const data = await res.json();
      if (!data.ok) throw new Error(data.error || 'test failed');
      log('LLM reply:\\n' + data.reply);
    }
    async function refreshStatus() {
      const res = await fetch('/api/status');
      const data = await res.json();
      if (!data.ok) throw new Error('status failed');
      document.getElementById('status').textContent = JSON.stringify(data.status, null, 2);
    }
    document.getElementById('reloadBtn').onclick = () => loadCfg().catch(e => log(String(e)));
    document.getElementById('saveBtn').onclick = () => saveCfg().catch(e => log(String(e)));
    document.getElementById('testBtn').onclick = () => testLlm().catch(e => log(String(e)));
    loadCfg().catch(e => log(String(e)));
    refreshStatus().catch(e => log(String(e)));
    setInterval(() => refreshStatus().catch(() => {}), 1000);
  </script>
</body>
</html>"""


def run_http_server(store: ConfigStore, stats: AgentStats, host: str, port: int) -> None:
    ConfigHttpHandler.store = store
    ConfigHttpHandler.stats = stats
    server = ThreadingHTTPServer((host, port), ConfigHttpHandler)
    print(f"[agent] config ui: http://{host}:{port}")
    server.serve_forever()


async def run_ws_server(store: ConfigStore, stats: AgentStats) -> None:
    cfg = store.get()
    server_cfg = cfg.get("server", {})
    host = str(server_cfg.get("host", "0.0.0.0"))
    port = int(server_cfg.get("port", 8765))


    async def process_session(
        ws: websockets.WebSocketServerProtocol,
        sid: str,
        session: Dict[str, Any],
        proto_version: int,
    ) -> None:
        frames = session.get("audio", [])
        sample_rate = int(session.get("sample_rate", 16000))
        channels = int(session.get("channels", 1))
        frame_duration = int(session.get("frame_duration", 60))

        stt_text = ""
        if frames:
            try:
                pcm = OpusDecoder.decode_frames(
                    frames,
                    sample_rate=sample_rate,
                    channels=channels,
                    frame_duration_ms=frame_duration,
                )
                run_cfg = store.get()
                asr = AsrClient(run_cfg.get("asr", {}))
                stt_text = await asyncio.to_thread(
                    asr.transcribe_pcm16,
                    pcm,
                    sample_rate,
                    channels,
                )
            except Exception as e:
                stt_text = f"[asr_pipeline_error] {e}"
                stats.set_error(stt_text)
        if not stt_text:
            stt_text = "hello"
        stats.set_text("last_stt", stt_text)

        await send_json(ws, {"type": "stt", "session_id": sid, "text": stt_text})

        run_cfg = store.get()
        llm = LlmClient(run_cfg.get("llm", {}))
        llm_text = await asyncio.to_thread(llm.chat, stt_text)
        stats.set_text("last_llm", llm_text)
        tts = TtsClient(run_cfg.get("tts", {}))

        await send_json(ws, {"type": "llm", "session_id": sid, "emotion": "neutral", "text": ":)"})
        await send_json(ws, {"type": "tts", "session_id": sid, "state": "start"})
        for sentence in split_sentences(llm_text):
            await send_json(
                ws,
                {
                    "type": "tts",
                    "session_id": sid,
                    "state": "sentence_start",
                    "text": sentence,
                },
            )
            wav_bytes = await asyncio.to_thread(tts.synthesize, sentence)
            try:
                pcm_tts = TtsClient.wav_to_pcm16_mono_16k(wav_bytes)
                opus_frames = OpusEncoder.encode_pcm16(
                    pcm_tts,
                    sample_rate=16000,
                    channels=1,
                    frame_duration_ms=60,
                )
                for f in opus_frames:
                    await ws.send(pack_audio_payload(f, proto_version))
            except Exception:
                stats.set_error("tts_audio_send_failed")
            await asyncio.sleep(0.02)
        await send_json(ws, {"type": "tts", "session_id": sid, "state": "stop"})
        stats.inc("sessions_completed")

        intent = infer_intent(stt_text)
        if intent:
            board_resp = None
            if intent["name"] == "drive":
                board_resp = ControlBridge.send(run_cfg, {"cmd": "drive", **intent["args"]})
            elif intent["name"] == "stop":
                board_resp = ControlBridge.send(run_cfg, {"cmd": "stop"})
            elif intent["name"] == "fire":
                board_resp = ControlBridge.send(run_cfg, {"cmd": "fire"})
            if board_resp is not None:
                await send_json(
                    ws,
                    {
                        "type": "mcp",
                        "session_id": sid,
                        "payload": {
                            "jsonrpc": "2.0",
                            "method": "tool/result",
                            "params": {"name": intent["name"], "result": board_resp},
                        },
                    },
                )

    async def on_client(ws: websockets.WebSocketServerProtocol) -> None:
        stats.add_connected(1)
        sessions: Dict[str, Dict[str, Any]] = {}
        session_tasks: Dict[str, asyncio.Task] = {}
        session_id = ""
        proto_version = 1
        conn_audio_params = {"sample_rate": 16000, "channels": 1, "frame_duration": 60}
        try:
            hv = ws.request_headers.get("Protocol-Version")
            if hv:
                proto_version = int(hv)
        except Exception:
            proto_version = 1

        try:
            async for message in ws:
                if isinstance(message, (bytes, bytearray)):
                    if session_id and session_id in sessions:
                        audio = parse_audio_payload(bytes(message), proto_version)
                        sessions[session_id]["audio"].append(audio)
                    continue

                try:
                    msg = json.loads(message)
                except Exception:
                    stats.set_error("invalid_json")
                    await send_json(ws, {"type": "error", "message": "invalid json"})
                    continue

                mtype = msg.get("type")
                if mtype == "hello":
                    audio_params = msg.get("audio_params", {})
                    if isinstance(audio_params, dict):
                        conn_audio_params["sample_rate"] = int(audio_params.get("sample_rate", 16000))
                        conn_audio_params["channels"] = int(audio_params.get("channels", 1))
                        conn_audio_params["frame_duration"] = int(audio_params.get("frame_duration", 60))
                    session_id = str(uuid.uuid4())
                    await send_json(
                        ws,
                        {
                            "type": "hello",
                            "transport": "websocket",
                            "session_id": session_id,
                            "audio_params": {
                                "format": "opus",
                                "sample_rate": 16000,
                                "channels": 1,
                                "frame_duration": 60,
                            },
                        },
                    )
                    continue

                if mtype == "listen":
                    state = str(msg.get("state", ""))
                    if state == "start":
                        sid = str(msg.get("session_id", session_id))
                        if not sid:
                            sid = str(uuid.uuid4())
                        old_task = session_tasks.pop(sid, None)
                        if old_task and not old_task.done():
                            old_task.cancel()
                        session_id = sid
                        sessions[session_id] = {
                            "audio": [],
                            "started_at": now_ts(),
                            "sample_rate": conn_audio_params["sample_rate"],
                            "channels": conn_audio_params["channels"],
                            "frame_duration": conn_audio_params["frame_duration"],
                        }
                        stats.inc("sessions_started")
                        continue
                    if state == "stop":
                        sid = str(msg.get("session_id", session_id))
                        session = sessions.pop(sid, {"audio": []})
                        old_task = session_tasks.pop(sid, None)
                        if old_task and not old_task.done():
                            old_task.cancel()
                        session_tasks[sid] = asyncio.create_task(process_session(ws, sid, session, proto_version))
                        continue
                    if state == "detect":
                        continue

                if mtype == "abort":
                    sid = str(msg.get("session_id", session_id))
                    task = session_tasks.pop(sid, None)
                    if task and not task.done():
                        task.cancel()
                    sessions.pop(sid, None)
                    stats.inc("sessions_aborted")
                    await send_json(ws, {"type": "tts", "session_id": sid, "state": "stop"})
                    continue

                stats.set_error(f"unsupported_type:{mtype}")
                await send_json(ws, {"type": "error", "message": f"unsupported type: {mtype}"})
        except websockets.ConnectionClosed:
            pass
        finally:
            for sid, task in list(session_tasks.items()):
                if not task.done():
                    task.cancel()
            session_tasks.clear()
            stats.add_connected(-1)

    print(f"[agent] ws server: ws://{host}:{port}")
    async with websockets.serve(on_client, host, port, max_size=4 * 1024 * 1024):
        await asyncio.Future()


async def main() -> None:
    store = ConfigStore()
    stats = AgentStats()
    cfg = store.get()
    ui_cfg = cfg.get("web_ui", {})
    ui_host = str(ui_cfg.get("host", "0.0.0.0"))
    ui_port = int(ui_cfg.get("port", 18080))

    http_thread = threading.Thread(target=run_http_server, args=(store, stats, ui_host, ui_port), daemon=True)
    http_thread.start()
    await run_ws_server(store, stats)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[agent] stopped")
