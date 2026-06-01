#include "webrtc_app.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_peer_default.h"
#include "esp_peer_signaling.h"
#include "esp_timer.h"
#include "esp_webrtc.h"
#include "esp_webrtc_defaults.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "media_sys.h"

#define SIGNAL_QUEUE_LEN 8
#define SIGNAL_MAX_BODY (16 * 1024)
#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
#define VIDEO_FPS 20

static const char *TAG = "webrtc_app";
static QueueHandle_t signal_queue;
static esp_peer_signaling_cfg_t signal_cfg;
static esp_webrtc_handle_t webrtc;
static httpd_req_t *sse_req;
static bool sse_connected;
static bool sse_stopping;
static bool webrtc_started;

static const char WEBRTC_HTML[] =
"<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\"><title>Heavy Punch WebRTC</title><style>html,body{margin:0;height:100%;background:#05080d;color:#eaf6ff;font-family:system-ui,sans-serif;overflow:hidden}main{height:100%;display:grid;grid-template-rows:1fr auto;gap:10px;padding:10px}video{width:100%;height:100%;object-fit:contain;background:#000;border:1px solid #26384a;border-radius:8px}.bar{display:flex;gap:8px;align-items:center}button{height:44px;border-radius:8px;border:1px solid #416078;background:#172333;color:#eaf6ff;font-weight:900;padding:0 14px}.ok{color:#9ff4d0}.bad{color:#fecaca}</style></head><body><main><video id=\"remote\" autoplay playsinline muted></video><div class=\"bar\"><button id=\"connect\">CONNECT</button><button id=\"hangup\">HANGUP</button><span id=\"status\" class=\"bad\">idle</span></div></main><script>"
"let pc=null,es=null;const v=document.getElementById('remote'),st=document.getElementById('status');function status(s,ok){st.textContent=s;st.className=ok?'ok':'bad'}async function post(u,o){const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o||{})});if(!r.ok)throw new Error(await r.text());return r.text()}async function connect(){await hangup();pc=new RTCPeerConnection({iceServers:[]});pc.ontrack=e=>{v.srcObject=e.streams[0];status('media connected',true)};pc.onconnectionstatechange=()=>status(pc.connectionState,pc.connectionState==='connected');pc.onicecandidate=e=>{if(e.candidate)post('/webrtc/signal/post',{type:'candidate',candidate:e.candidate.candidate}).catch(()=>{})};pc.addTransceiver('video',{direction:'recvonly'});es=new EventSource('/webrtc/signal');es.onmessage=async ev=>{const m=JSON.parse(ev.data);if(m.type==='connected')return;if(m.type==='offer'){await pc.setRemoteDescription({type:'offer',sdp:m.sdp});const answer=await pc.createAnswer();await pc.setLocalDescription(answer);await post('/webrtc/signal/post',{type:'answer',sdp:answer.sdp});status('answer sent',true)}else if(m.type==='candidate'){try{await pc.addIceCandidate({candidate:m.candidate,sdpMid:'0',sdpMLineIndex:0})}catch(e){}}else if(m.type==='bye'){await hangup()}};status('signaling open',true)}async function hangup(){if(es){es.close();es=null}if(pc){pc.close();pc=null}try{await post('/webrtc/signal/post',{type:'bye'})}catch(e){}v.srcObject=null;status('idle',false)}document.getElementById('connect').onclick=connect;document.getElementById('hangup').onclick=hangup;"
"</script></body></html>";

static int send_sse(httpd_req_t *req, const char *data) {
  const int len = strlen(data) + 8;
  char *buf = malloc(len);
  if (buf == NULL) {
    return -1;
  }
  const int written = snprintf(buf, len, "data: %s\n\n", data);
  int ret = httpd_resp_send_chunk(req, buf, written);
  free(buf);
  return ret;
}

static void signal_send_task(void *arg) {
  (void)arg;
  int64_t last_heartbeat = 0;
  while (!sse_stopping) {
    char *msg = NULL;
    if (xQueueReceive(signal_queue, &msg, pdMS_TO_TICKS(100)) == pdTRUE && msg != NULL) {
      int ret = send_sse(sse_req, msg);
      free(msg);
      if (ret != 0) {
        break;
      }
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now - last_heartbeat > 5000) {
      last_heartbeat = now;
      if (send_sse(sse_req, "{\"type\":\"heartbeat\"}") != 0) {
        break;
      }
    }
  }
  httpd_req_async_handler_complete(sse_req);
  sse_req = NULL;
  sse_connected = false;
  sse_stopping = false;
  vTaskDelete(NULL);
}

static int signaling_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *sig) {
  signal_cfg = *cfg;
  esp_peer_signaling_ice_info_t ice = {
      .is_initiator = true,
  };
  signal_cfg.on_ice_info(&ice, cfg->ctx);
  signal_cfg.on_connected(cfg->ctx);
  *sig = signal_queue;
  return 0;
}

static int signaling_send_msg(esp_peer_signaling_handle_t sig, esp_peer_signaling_msg_t *msg) {
  if (sig != signal_queue || msg == NULL) {
    return -1;
  }
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return -1;
  }

  switch (msg->type) {
    case ESP_PEER_SIGNALING_MSG_SDP:
      cJSON_AddStringToObject(root, "type", "offer");
      cJSON_AddStringToObject(root, "sdp", (const char *)msg->data);
      break;
    case ESP_PEER_SIGNALING_MSG_CANDIDATE:
      cJSON_AddStringToObject(root, "type", "candidate");
      cJSON_AddStringToObject(root, "candidate", (const char *)msg->data);
      break;
    case ESP_PEER_SIGNALING_MSG_BYE:
      cJSON_AddStringToObject(root, "type", "bye");
      break;
    case ESP_PEER_SIGNALING_MSG_CUSTOMIZED:
      cJSON_AddStringToObject(root, "type", "customized");
      cJSON_AddStringToObject(root, "data", (const char *)msg->data);
      break;
    default:
      cJSON_Delete(root);
      return 0;
  }

  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json == NULL) {
    return -1;
  }

  if (xQueueSend(signal_queue, &json, pdMS_TO_TICKS(100)) != pdTRUE) {
    free(json);
  }
  return 0;
}

static int signaling_stop(esp_peer_signaling_handle_t sig) {
  (void)sig;
  if (sse_connected) {
    sse_stopping = true;
  }
  return 0;
}

static const esp_peer_signaling_impl_t SIGNALING_IMPL = {
    .start = signaling_start,
    .send_msg = signaling_send_msg,
    .stop = signaling_stop,
};

static int on_webrtc_event(esp_webrtc_event_t *event, void *ctx) {
  (void)ctx;
  ESP_LOGI(TAG, "event=%d", event ? event->type : 0);
  return 0;
}

static esp_err_t read_body(httpd_req_t *req, char **out) {
  if (req->content_len <= 0 || req->content_len > SIGNAL_MAX_BODY) {
    return ESP_ERR_INVALID_SIZE;
  }
  char *buf = calloc(1, req->content_len + 1);
  if (!buf) return ESP_ERR_NO_MEM;
  int got = 0;
  while (got < req->content_len) {
    int ret = httpd_req_recv(req, buf + got, req->content_len - got);
    if (ret <= 0) {
      free(buf);
      return ESP_FAIL;
    }
    got += ret;
  }
  *out = buf;
  return ESP_OK;
}

esp_err_t webrtc_app_init(void) {
  signal_queue = xQueueCreate(SIGNAL_QUEUE_LEN, sizeof(char *));
  if (!signal_queue) {
    return ESP_ERR_NO_MEM;
  }

  esp_peer_default_cfg_t peer_cfg = {
      .agent_recv_timeout = 100,
      .data_ch_cfg = {
          .recv_cache_size = 1536,
          .send_cache_size = 1536,
      },
      .rtp_cfg = {
          .audio_recv_jitter = {
              .cache_size = 1024,
          },
          .video_recv_jitter = {
              .cache_size = 1024,
          },
          .send_pool_size = 1024,
          .send_queue_num = 10,
      },
      .max_candidates = 2,
      .ice_use_lite_mode = true,
  };
  esp_webrtc_cfg_t cfg = {
      .peer_cfg = {
          .video_info = {
              .codec = ESP_PEER_VIDEO_CODEC_H264,
              .width = VIDEO_WIDTH,
              .height = VIDEO_HEIGHT,
              .fps = VIDEO_FPS,
          },
          .audio_dir = ESP_PEER_MEDIA_DIR_NONE,
          .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
          .enable_data_channel = false,
          .no_auto_reconnect = false,
          .extra_cfg = &peer_cfg,
          .extra_size = sizeof(peer_cfg),
      },
      .signaling_cfg = {
          .signal_url = "local://heavy-punch",
      },
      .peer_impl = esp_peer_get_default_impl(),
      .signaling_impl = &SIGNALING_IMPL,
  };
  int ret = esp_webrtc_open(&cfg, &webrtc);
  if (ret != 0) {
    ESP_LOGE(TAG, "esp_webrtc_open failed: %d", ret);
    return ESP_FAIL;
  }
  esp_webrtc_set_event_handler(webrtc, on_webrtc_event, NULL);
  return ESP_OK;
}

esp_err_t webrtc_app_start(void) {
  if (webrtc_started) {
    esp_webrtc_stop(webrtc);
    webrtc_started = false;
  }
  esp_webrtc_media_provider_t provider = {0};
  ESP_ERROR_CHECK(media_sys_get_provider(&provider));
  ESP_ERROR_CHECK(esp_webrtc_set_media_provider(webrtc, &provider) == 0 ? ESP_OK : ESP_FAIL);
  int cert_ret = esp_peer_pre_generate_cert();
  ESP_LOGI(TAG, "esp_peer_pre_generate_cert ret=%d", cert_ret);
  int ret = esp_webrtc_start(webrtc);
  if (ret == 0) {
    webrtc_started = true;
    return ESP_OK;
  }
  return ESP_FAIL;
}

void webrtc_app_stop(void) {
  if (webrtc) {
    esp_webrtc_stop(webrtc);
    webrtc_started = false;
  }
}

esp_err_t webrtc_app_page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, WEBRTC_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t webrtc_app_signal_get_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/event-stream");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  if (sse_connected) {
    send_sse(req, "{\"error\":\"only one WebRTC listener allowed\"}");
    return ESP_OK;
  }
  send_sse(req, "{\"type\":\"connected\"}");
  sse_connected = true;
  httpd_req_async_handler_begin(req, &sse_req);
  xTaskCreate(signal_send_task, "webrtc_sse", 4096, NULL, 5, NULL);
  if (webrtc_app_start() != ESP_OK) {
    ESP_LOGE(TAG, "failed to start WebRTC for signaling client");
  }
  return ESP_OK;
}

esp_err_t webrtc_app_signal_post_handler(httpd_req_t *req) {
  char *body = NULL;
  if (read_body(req, &body) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(body);
  if (root == NULL) {
    free(body);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    return ESP_FAIL;
  }

  const cJSON *type = cJSON_GetObjectItem(root, "type");
  const cJSON *sdp = cJSON_GetObjectItem(root, "sdp");
  const cJSON *candidate = cJSON_GetObjectItem(root, "candidate");
  esp_peer_signaling_msg_t msg = {0};
  if (cJSON_IsString(type) && strcmp(type->valuestring, "answer") == 0 && cJSON_IsString(sdp)) {
    msg.type = ESP_PEER_SIGNALING_MSG_SDP;
    msg.data = (uint8_t *)sdp->valuestring;
    msg.size = strlen(sdp->valuestring);
  } else if (cJSON_IsString(type) && strcmp(type->valuestring, "candidate") == 0 &&
             cJSON_IsString(candidate)) {
    msg.type = ESP_PEER_SIGNALING_MSG_CANDIDATE;
    msg.data = (uint8_t *)candidate->valuestring;
    msg.size = strlen(candidate->valuestring);
  } else if (cJSON_IsString(type) && strcmp(type->valuestring, "bye") == 0) {
    msg.type = ESP_PEER_SIGNALING_MSG_BYE;
  }

  if (msg.type != ESP_PEER_SIGNALING_MSG_NONE && signal_cfg.on_msg) {
    signal_cfg.on_msg(&msg, signal_cfg.ctx);
  }
  cJSON_Delete(root);
  free(body);
  return httpd_resp_sendstr(req, "OK");
}
