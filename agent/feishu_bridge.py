import json
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, Optional


ROOT = Path(__file__).resolve().parent
CONFIG_PATH = ROOT / "config.json"
EXAMPLE_CONFIG_PATH = ROOT / "config.example.json"


def now_ts() -> int:
    return int(time.time())


def ensure_config() -> Dict[str, Any]:
    if CONFIG_PATH.exists():
        return json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    data = json.loads(EXAMPLE_CONFIG_PATH.read_text(encoding="utf-8"))
    CONFIG_PATH.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    return data


class ConfigStore:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._cfg = ensure_config()

    def get(self) -> Dict[str, Any]:
        with self._lock:
            return json.loads(json.dumps(self._cfg))


class MessageDeduper:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._seen: Dict[str, int] = {}

    def seen(self, message_id: str, ttl_sec: int = 600) -> bool:
        now = now_ts()
        with self._lock:
            expired = [k for k, ts in self._seen.items() if now - ts > ttl_sec]
            for key in expired:
                self._seen.pop(key, None)
            if message_id in self._seen:
                return True
            self._seen[message_id] = now
            return False


class FeishuApi:
    def __init__(self, cfg: Dict[str, Any]) -> None:
        self._cfg = cfg
        self._token = ""
        self._token_expire_at = 0
        self._lock = threading.Lock()

    @property
    def app_id(self) -> str:
        return str(self._cfg.get("app_id", "")).strip()

    @property
    def app_secret(self) -> str:
        return str(self._cfg.get("app_secret", "")).strip()

    @property
    def verification_token(self) -> str:
        return str(self._cfg.get("verification_token", "")).strip()

    def _post_json(self, url: str, payload: Dict[str, Any], headers: Optional[Dict[str, str]] = None) -> Dict[str, Any]:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(url, data=body, method="POST")
        req.add_header("Content-Type", "application/json; charset=utf-8")
        for key, value in (headers or {}).items():
            req.add_header(key, value)
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def tenant_access_token(self) -> str:
        with self._lock:
            if self._token and now_ts() < self._token_expire_at - 60:
                return self._token
            if not self.app_id or not self.app_secret:
                raise RuntimeError("feishu app_id/app_secret not configured")
            data = self._post_json(
                "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
                {"app_id": self.app_id, "app_secret": self.app_secret},
            )
            if int(data.get("code", -1)) != 0:
                raise RuntimeError(f"feishu auth failed: {data}")
            self._token = str(data["tenant_access_token"])
            self._token_expire_at = now_ts() + int(data.get("expire", 7200))
            return self._token

    def send_text_to_chat(self, chat_id: str, text: str) -> Dict[str, Any]:
        token = self.tenant_access_token()
        payload = {
            "receive_id": chat_id,
            "msg_type": "text",
            "content": json.dumps({"text": text}, ensure_ascii=False),
        }
        return self._post_json(
            "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=chat_id",
            payload,
            headers={"Authorization": f"Bearer {token}"},
        )


class ZclawClient:
    def __init__(self, cfg: Dict[str, Any]) -> None:
        self._cfg = cfg

    @property
    def base_url(self) -> str:
        return str(self._cfg.get("base_url", "")).rstrip("/")

    @property
    def timeout_sec(self) -> int:
        return int(self._cfg.get("timeout_sec", 90))

    def ask(self, message: str) -> str:
        if not self.base_url:
            raise RuntimeError("zclaw base_url not configured")

        post_body = json.dumps({"message": message}).encode("utf-8")
        post_req = urllib.request.Request(
            f"{self.base_url}/api/chat",
            data=post_body,
            method="POST",
            headers={"Content-Type": "application/json; charset=utf-8", "Connection": "close"},
        )
        with urllib.request.urlopen(post_req, timeout=15) as resp:
            job = json.loads(resp.read().decode("utf-8"))
        job_id = int(job["id"])

        stream_req = urllib.request.Request(
            f"{self.base_url}/api/chat/stream?id={job_id}",
            headers={"Accept": "text/event-stream", "Connection": "close"},
        )
        final_reply = ""
        current_event = ""
        current_data = []
        started = time.time()
        with urllib.request.urlopen(stream_req, timeout=self.timeout_sec) as resp:
            while time.time() - started < self.timeout_sec:
                raw = resp.readline()
                if not raw:
                    break
                line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                if line.startswith("event: "):
                    current_event = line[7:]
                elif line.startswith("data: "):
                    current_data.append(line[6:])
                elif line == "":
                    if current_event:
                        payload = {}
                        if current_data:
                            try:
                                payload = json.loads("\n".join(current_data))
                            except Exception:
                                payload = {}
                        if current_event == "final":
                            final_reply = str(payload.get("reply", "")).strip()
                            break
                        if current_event == "failure":
                            raise RuntimeError(str(payload.get("error", "zclaw request failed")))
                    current_event = ""
                    current_data = []
        if not final_reply:
            raise RuntimeError("zclaw did not return final reply")
        return final_reply


class FeishuBridgeHandler(BaseHTTPRequestHandler):
    server_version = "zclaw-feishu-bridge/0.1"

    def log_message(self, format: str, *args: Any) -> None:
        return

    def _json_response(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/healthz":
            self._json_response(HTTPStatus.OK, {"ok": True, "time": now_ts()})
            return
        self._json_response(HTTPStatus.NOT_FOUND, {"error": "not_found"})

    def do_POST(self) -> None:
        if self.path != "/feishu/events":
            self._json_response(HTTPStatus.NOT_FOUND, {"error": "not_found"})
            return

        try:
            content_len = int(self.headers.get("Content-Length", "0"))
        except Exception:
            content_len = 0
        if content_len <= 0:
            self._json_response(HTTPStatus.BAD_REQUEST, {"error": "empty_body"})
            return

        try:
            raw = self.rfile.read(content_len)
            payload = json.loads(raw.decode("utf-8"))
        except Exception as exc:
            self._json_response(HTTPStatus.BAD_REQUEST, {"error": f"invalid_json:{exc}"})
            return

        challenge = payload.get("challenge")
        if challenge:
            self._json_response(HTTPStatus.OK, {"challenge": challenge})
            return

        cfg = self.server.config_store.get()  # type: ignore[attr-defined]
        feishu_cfg = cfg.get("feishu", {})
        verification_token = str(feishu_cfg.get("verification_token", "")).strip()
        header = payload.get("header", {}) or {}
        if verification_token and header.get("token") not in ("", verification_token):
            self._json_response(HTTPStatus.FORBIDDEN, {"error": "invalid_verification_token"})
            return

        event_type = str(header.get("event_type", ""))
        event = payload.get("event", {}) or {}
        message = event.get("message", {}) or {}
        sender = event.get("sender", {}) or {}
        message_id = str(message.get("message_id", ""))

        if event_type != "im.message.receive_v1":
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": event_type or "unknown"})
            return

        if not message_id:
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": "missing_message_id"})
            return

        if self.server.deduper.seen(message_id):  # type: ignore[attr-defined]
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": "duplicate"})
            return

        sender_type = str(sender.get("sender_type", ""))
        if sender_type == "app":
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": "self_message"})
            return

        if str(message.get("message_type", "")) != "text":
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": "non_text"})
            return

        try:
            content = json.loads(str(message.get("content", "{}")))
            text = str(content.get("text", "")).strip()
        except Exception:
            text = ""
        if not text:
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": "empty_text"})
            return

        chat_id = str(message.get("chat_id", "")).strip()
        if not chat_id:
            self._json_response(HTTPStatus.OK, {"ok": True, "ignored": "missing_chat_id"})
            return

        try:
            reply = self.server.zclaw.ask(text)  # type: ignore[attr-defined]
            feishu_resp = self.server.feishu.send_text_to_chat(chat_id, reply)  # type: ignore[attr-defined]
            self._json_response(HTTPStatus.OK, {"ok": True, "reply_sent": feishu_resp.get("code", -1) == 0})
        except urllib.error.HTTPError as exc:
            self._json_response(HTTPStatus.BAD_GATEWAY, {"error": f"http_error:{exc.code}"})
        except Exception as exc:
            self._json_response(HTTPStatus.BAD_GATEWAY, {"error": str(exc)})


class FeishuBridgeServer(ThreadingHTTPServer):
    def __init__(self, server_address: tuple[str, int], handler_cls: type[FeishuBridgeHandler], config_store: ConfigStore):
        super().__init__(server_address, handler_cls)
        cfg = config_store.get()
        self.config_store = config_store
        self.deduper = MessageDeduper()
        self.feishu = FeishuApi(cfg.get("feishu", {}))
        self.zclaw = ZclawClient(cfg.get("zclaw", {}))


def main() -> None:
    config_store = ConfigStore()
    cfg = config_store.get()
    bridge_cfg = cfg.get("feishu", {})
    host = str(bridge_cfg.get("host", "0.0.0.0"))
    port = int(bridge_cfg.get("port", 19090))
    server = FeishuBridgeServer((host, port), FeishuBridgeHandler, config_store)
    print(f"Feishu bridge listening on http://{host}:{port}")
    print("POST /feishu/events")
    print("GET  /healthz")
    server.serve_forever()


if __name__ == "__main__":
    main()
