# Feishu Bridge

这个桥接服务跑在宿主机上，不跑在 ESP32 上。

链路：

1. 飞书事件回调到宿主机 `POST /feishu/events`
2. 宿主机把文本消息转发到板子的 `POST /api/chat + SSE /api/chat/stream`
3. 宿主机再把最终回复发回飞书会话

## Why

飞书机器人通常需要一个稳定的 HTTPS 回调入口。ESP32 在家庭网络后面，不适合直接暴露公网回调，所以这里用宿主机桥接。

## Files

- `agent/feishu_bridge.py`
- `agent/config.json`

## Config

在 `agent/config.json` 里填：

- `feishu.app_id`
- `feishu.app_secret`
- `feishu.verification_token`
- `zclaw.base_url`

`zclaw.base_url` 先填局域网地址，例如：

```json
"zclaw": {
  "base_url": "http://192.168.1.6",
  "timeout_sec": 90
}
```

## Run

```powershell
python .\agent\feishu_bridge.py
```

默认监听：

- `GET /healthz`
- `POST /feishu/events`

## Feishu App Setup

在飞书开放平台里：

1. 创建一个企业自建应用
2. 打开机器人能力
3. 打开事件订阅
4. 把事件回调 URL 指向：
   - `https://你的公网域名/feishu/events`
5. 订阅消息接收事件：
   - `im.message.receive_v1`
6. 在应用权限里开通消息发送相关权限

## Notes

- 当前 MVP 只处理 `text` 文本消息
- 当前回复直接发回原聊天 `chat_id`
- 当前未实现飞书加密消息体，只支持 verification token 校验
- 宿主机必须能访问板子的 `zclaw` HTTP 页面
- 如果要给飞书用，宿主机还需要一个公网 HTTPS 入口，可以用反向代理或隧道工具
