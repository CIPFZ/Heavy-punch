#include "web_ui.h"

#include "agent.h"
#include "config.h"
#include "device_identity.h"
#include "llm.h"
#include "memory.h"
#include "nvs_keys.h"
#include "qrcodegen.h"
#include "text_buffer.h"
#include "wifi_credentials.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_ui";

#define WEB_UI_AP_PASS "zclaw1234"
#define WEB_UI_AP_MAX_CONN 4
#define WEB_UI_CHAT_JOB_COUNT 2
#define WEB_UI_CHAT_STREAM_WAIT_MS 120000
#define WEB_UI_CHAT_POLL_MS 200
#define WEB_UI_REPLY_BUF_SIZE CHANNEL_TX_BUF_SIZE
#define WEB_UI_CHAT_JOB_STACK_SIZE 16384
#define WEB_UI_STATUS_EVENT_COUNT 8
#define WEB_UI_QR_TEXT_MAX_LEN 192
#define WEB_UI_QR_TMP_BUF_LEN qrcodegen_BUFFER_LEN_FOR_VERSION(8)

typedef enum {
    CHAT_JOB_EMPTY = 0,
    CHAT_JOB_QUEUED = 1,
    CHAT_JOB_RUNNING = 2,
    CHAT_JOB_DONE = 3,
    CHAT_JOB_FAILED = 4,
    CHAT_JOB_STREAMING = 5,
} chat_job_state_t;

typedef struct {
    uint32_t seq;
    char event[24];
    char message[192];
} chat_status_event_t;

typedef struct {
    int id;
    chat_job_state_t state;
    char prompt[MAX_MESSAGE_LEN];
    char reply[WEB_UI_REPLY_BUF_SIZE];
    size_t streamed_len;
    char error[160];
    uint32_t status_next_seq;
    chat_status_event_t status_events[WEB_UI_STATUS_EVENT_COUNT];
} chat_job_t;

static httpd_handle_t s_server = NULL;
static web_ui_mode_t s_mode = WEB_UI_MODE_PROVISIONING;
static bool s_ap_started = false;
static bool s_wifi_ap_netif_ready = false;
static bool s_httpd_started = false;
static SemaphoreHandle_t s_jobs_mutex = NULL;
static int s_next_job_id = 1;
static chat_job_t s_jobs[WEB_UI_CHAT_JOB_COUNT];

static const char *k_setup_html =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>zclaw setup</title><link rel='icon' href='data:,'><style>"
"body{margin:0;background:radial-gradient(circle at top,#1a212c 0,#0d1015 55%);color:#e7edf3;font-family:system-ui,-apple-system,sans-serif}"
":root{--bg:#0d1015;--panel:#151a22;--line:#2a313c;--muted:#93a0b0;--text:#e7edf3;--accent:#ff6a1a;--accent2:#6ae8ff}"
".shell{max-width:760px;margin:0 auto;padding:16px 14px 24px;display:grid;gap:14px}.hero,.panel{background:linear-gradient(180deg,rgba(255,255,255,.03),rgba(255,255,255,.01));border:1px solid var(--line);border-radius:22px;padding:16px}.hero{overflow:hidden;position:relative}.hero:after{content:'';position:absolute;inset:auto -40px -40px auto;width:140px;height:140px;background:radial-gradient(circle,rgba(255,106,26,.25),transparent 70%);pointer-events:none}"
".eyebrow{margin:0 0 6px;color:var(--accent2);font:700 11px/1.2 ui-monospace,Consolas,monospace;letter-spacing:.16em;text-transform:uppercase}h1,h2{margin:0}.muted{color:var(--muted);font-size:14px;line-height:1.5}.grid,.stack{display:grid;gap:12px}.two{display:grid;gap:12px}.info{background:rgba(106,232,255,.05);border:1px solid rgba(106,232,255,.15);border-radius:18px;padding:14px}.mono{font:13px/1.5 ui-monospace,Consolas,monospace;color:#c9d5e1}.pillbar{display:flex;flex-wrap:wrap;gap:8px}.pill{padding:7px 10px;border-radius:999px;background:#1d2530;border:1px solid var(--line);color:#d8e2ec;font:700 11px/1.2 ui-monospace,Consolas,monospace;letter-spacing:.06em;text-transform:uppercase}"
".qr{display:grid;gap:8px;justify-items:start}.qr img{width:min(52vw,180px);height:min(52vw,180px);border-radius:18px;border:1px solid var(--line);background:#fff;padding:8px}.field{display:grid;gap:8px}.label{font:700 11px/1.2 ui-monospace,Consolas,monospace;letter-spacing:.12em;text-transform:uppercase;color:var(--accent2)}"
"input,select{width:100%;box-sizing:border-box;min-height:46px;padding:12px 13px;background:#0f141b;border:1px solid var(--line);border-radius:14px;color:var(--text);font:500 15px/1.3 system-ui,-apple-system,sans-serif}"
"button{min-height:46px;padding:12px 16px;border:1px solid var(--line);border-radius:14px;background:linear-gradient(180deg,#ff7b31,#e25300);color:#fff;font:700 13px/1.2 ui-monospace,Consolas,monospace;letter-spacing:.08em;text-transform:uppercase}.secondary{background:#202835;color:#d8e2ec}.actions{display:flex;flex-wrap:wrap;gap:10px}pre{margin:0;background:#0b0f14;color:#d7e6f2;padding:14px;border-radius:18px;border:1px solid var(--line);overflow:auto;white-space:pre-wrap;font:13px/1.5 ui-monospace,Consolas,monospace}@media(min-width:720px){.two{grid-template-columns:1.1fr .9fr}}</style></head><body>"
"<div class='shell'><section class='hero'><p class='eyebrow'>zclaw onboard setup</p><h1>Bring the board online</h1><p class='muted'>Use the setup hotspot first. Save Wi-Fi only when you want the board to join your router. Save AI settings separately whenever you need.</p><div class='pillbar'><span class='pill'>AP 192.168.4.1</span><span class='pill'>DHCP after Wi-Fi</span><span class='pill'>OLED shows IP</span></div></section>"
"<div class='two'><section class='panel stack'><div><p class='eyebrow'>Discovery</p><h2>Open the setup portal</h2></div><div class='info'><div id='setupInfo' class='mono'>Loading...</div></div><div class='qr'><img id='setupQr' alt='Setup QR'><div id='setupQrLabel' class='muted'>Scan to open setup.</div></div></section>"
"<section class='panel stack'><div><p class='eyebrow'>Quick notes</p><h2>What happens next</h2></div><div class='pillbar'><span class='pill'>Join setup AP</span><span class='pill'>Open portal</span><span class='pill'>Save config</span></div><p class='muted'>After Wi-Fi is saved, reconnect your phone to the same router. Then open the DHCP IP shown on the OLED screen.</p></section></div>"
"<section class='panel stack'><div><p class='eyebrow'>Config</p><h2>Wi-Fi</h2></div><div class='grid'><label class='field'><span class='label'>cfg_wifi_ssid</span><input id='ssid' placeholder='Wi-Fi SSID'></label><label class='field'><span class='label'>cfg_wifi_pass</span><input id='password' type='password' placeholder='Wi-Fi password'></label></div><div class='actions'><button id='saveWifiBtn'>Commit Wi-Fi</button></div></section>"
"<section class='panel stack'><div><p class='eyebrow'>Config</p><h2>AI settings</h2></div><div class='grid'><label class='field'><span class='label'>cfg_backend</span><select id='backend'><option value='openai'>OpenAI-compatible</option><option value='anthropic'>Anthropic</option><option value='openrouter'>OpenRouter</option><option value='ollama'>Ollama</option></select></label><label class='field'><span class='label'>cfg_model</span><input id='model' placeholder='Model, e.g. deepseek-chat'></label><label class='field'><span class='label'>cfg_base_url</span><input id='apiUrl' placeholder='Base URL, e.g. https://api.deepseek.com/v1'></label><label class='field'><span class='label'>cfg_api_key</span><input id='apiKey' type='password' placeholder='API key'></label></div><div class='actions'><button id='saveLlmBtn' class='secondary'>Save AI settings</button><button id='restartBtn' class='secondary'>Restart setup hotspot</button></div></section>"
"<pre id='status'>System ready. Save Wi-Fi when you want zclaw to leave setup mode. For OpenAI-compatible backends, you can enter only the base URL and zclaw will append the chat endpoint.</pre></div>"
"<script>"
"const statusEl=document.getElementById('status');const setupInfoEl=document.getElementById('setupInfo');const setupQrEl=document.getElementById('setupQr');const setupQrLabelEl=document.getElementById('setupQrLabel');"
"function setQrImage(imgEl,labelEl,url,label){if(!imgEl)return;const target=(url||'').trim();if(!target){imgEl.removeAttribute('src');if(labelEl)labelEl.textContent='QR unavailable';return;}imgEl.src='/api/qr.svg?text='+encodeURIComponent(target);if(labelEl)labelEl.textContent=(label||'Scan to open')+': '+target;}"
"async function loadSettings(){try{const res=await fetch('/api/settings');const data=await res.json();if(!res.ok)return;"
"document.getElementById('ssid').value=data.wifi_ssid||'';document.getElementById('backend').value=data.llm_backend||'openai';"
"document.getElementById('model').value=data.llm_model||'';document.getElementById('apiUrl').value=data.llm_api_url||'';"
"const setupUrl=data.setup_url||'http://192.168.4.1';setupInfoEl.textContent='SETUP_URL  '+setupUrl+'\\nNEXT_STEP  Reconnect phone to router after Wi-Fi save\\nAPI_KEY    '+(data.has_api_key?'stored':'missing');setQrImage(setupQrEl,setupQrLabelEl,setupUrl,'Scan setup portal');statusEl.textContent='Save Wi-Fi to leave setup mode. Save AI settings separately whenever you need.';}catch(err){setupInfoEl.textContent='SETUP_URL  http://192.168.4.1\\nNEXT_STEP  Open portal from phone';setQrImage(setupQrEl,setupQrLabelEl,'http://192.168.4.1','Scan setup portal');}}"
"document.getElementById('saveWifiBtn').onclick=async()=>{const payload={wifi_ssid:document.getElementById('ssid').value.trim(),wifi_pass:document.getElementById('password').value};statusEl.textContent='Saving Wi-Fi...';"
"try{const res=await fetch('/api/settings/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const data=await res.json();if(!res.ok){statusEl.textContent=data.error||('Wi-Fi save failed ('+res.status+')');return;}statusEl.textContent='Wi-Fi saved. Device is rebooting now. Reconnect your phone to your router, then use the OLED screen or router device list to open the new IP.';}catch(err){statusEl.textContent='Network error: '+err.message;}};"
"document.getElementById('saveLlmBtn').onclick=async()=>{const payload={llm_backend:document.getElementById('backend').value,llm_model:document.getElementById('model').value.trim(),llm_api_url:document.getElementById('apiUrl').value.trim(),api_key:document.getElementById('apiKey').value.trim()};statusEl.textContent='Saving AI settings...';"
"try{const res=await fetch('/api/settings/llm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const data=await res.json();if(!res.ok){statusEl.textContent=data.error||('AI save failed ('+res.status+')');return;}document.getElementById('apiKey').value='';statusEl.textContent='AI settings saved. You can save Wi-Fi separately whenever needed.';}catch(err){statusEl.textContent='Network error: '+err.message;}};"
"document.getElementById('restartBtn').onclick=async()=>{statusEl.textContent='Restarting setup hotspot...';try{const res=await fetch('/api/setup-mode',{method:'POST'});const data=await res.json();if(!res.ok){statusEl.textContent=data.error||('Restart failed ('+res.status+')');return;}statusEl.textContent='Restarting setup hotspot now.';}catch(err){statusEl.textContent='Network error: '+err.message;}};"
"loadSettings();"
"</script></body></html>";

static const char *k_chat_html =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>zclaw chat</title><link rel='icon' href='data:,'><style>"
":root{--bg:#f5f5f7;--card:rgba(255,255,255,.78);--solid:#ffffff;--line:rgba(18,24,35,.08);--text:#111318;--muted:#808792;--blue:#3478f6;--blue2:#5b8cff;--good:#18a957;--warn:#ff9f0a;--error:#d94f45;--sheet:#f7f7fa}*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}html,body{height:100%}body{margin:0;background:linear-gradient(180deg,#f8f8fa 0,#f1f2f6 100%);color:var(--text);font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;overflow:hidden}"
".app{height:100dvh;display:flex;flex-direction:column}.topbar{position:sticky;top:0;z-index:10;display:grid;grid-template-columns:44px 1fr 44px;align-items:center;padding:12px 12px 8px;background:rgba(245,245,247,.76);backdrop-filter:blur(16px);border-bottom:1px solid rgba(18,24,35,.05)}.iconbtn{width:40px;height:40px;border:0;border-radius:20px;background:rgba(255,255,255,.72);box-shadow:0 1px 2px rgba(0,0,0,.04);color:#1f2430;font:600 20px/1 system-ui;padding:0;display:grid;place-items:center}.titlewrap{display:flex;align-items:center;justify-content:center;gap:8px}.title{font-size:17px;font-weight:700;text-align:center}.statusdot{width:7px;height:7px;border-radius:999px;background:var(--good);box-shadow:0 0 0 4px rgba(24,169,87,.12)}.iconbtn.subtle svg{width:18px;height:18px;stroke:#1f2430;stroke-width:1.9;fill:none;stroke-linecap:round}"
".chat{flex:1;overflow:auto;padding:8px 14px 164px;display:grid;align-content:start;gap:10px}.empty{min-height:calc(100dvh - 240px);display:grid;place-items:center;text-align:center;color:var(--muted)}.emptyBox{display:grid;gap:14px;justify-items:center}.logo{width:52px;height:52px;border-radius:18px;background:linear-gradient(180deg,#4e82ff,#3367f5);color:#fff;display:grid;place-items:center;font-size:26px;box-shadow:0 18px 40px rgba(52,120,246,.22)}.emptyTitle{font-size:20px;font-weight:700;color:#141821}.emptyHint{font-size:14px;max-width:240px;line-height:1.5}"
".bubble{max-width:min(88%,720px);padding:12px 14px;border-radius:20px;white-space:pre-wrap;line-height:1.45;box-shadow:0 1px 2px rgba(0,0,0,.04)}.user{justify-self:end;background:linear-gradient(180deg,var(--blue2),var(--blue));color:#fff;border-bottom-right-radius:8px}.agent{justify-self:start;background:var(--solid);border:1px solid var(--line);border-bottom-left-radius:8px}.agent.streaming{position:relative}.agent.streaming::after{content:'';display:inline-block;width:8px;height:1.1em;margin-left:4px;vertical-align:-0.15em;background:var(--blue);animation:blink 1s steps(1,end) infinite}.system{justify-self:start;background:#fff8ec;border:1px solid rgba(255,159,10,.16);color:#7b5600}.system .summary{font-weight:700}.system .detail{display:none;margin-top:8px;padding-top:8px;border-top:1px solid rgba(255,159,10,.18);color:#7a6943}.system.expanded .detail{display:block}.system .toggle{margin-top:8px;border:0;background:transparent;color:#8c6a11;padding:0;font:600 13px/1 system-ui}.error{justify-self:start;background:#fff1f0;border:1px solid rgba(217,79,69,.16);color:#9f3b34}"
".composerWrap{position:fixed;left:0;right:0;bottom:0;padding:10px 12px calc(10px + env(safe-area-inset-bottom));background:linear-gradient(180deg,rgba(245,245,247,0),rgba(245,245,247,.88) 24%,rgba(245,245,247,.96));backdrop-filter:blur(18px)}.composer{max-width:920px;margin:0 auto;background:rgba(28,29,32,.84);border-radius:30px;padding:12px;box-shadow:0 20px 44px rgba(0,0,0,.16);display:grid;gap:8px}.inputShell{display:grid;grid-template-columns:1fr 48px;gap:10px;align-items:end}textarea{width:100%;min-height:44px;max-height:140px;resize:none;border:0;background:transparent;color:#fff;font:500 16px/1.45 system-ui;padding:6px 2px 0;outline:none}textarea::placeholder{color:rgba(255,255,255,.52)}textarea:disabled{opacity:.56}.sendBtn{width:44px;height:44px;border:0;border-radius:22px;background:linear-gradient(180deg,#4e82ff,#3367f5);color:#fff;font-size:18px;box-shadow:0 10px 24px rgba(52,120,246,.34);transition:opacity .18s ease,transform .18s ease}.sendBtn.hidden{opacity:0;transform:scale(.88);pointer-events:none}.sendBtn:disabled{opacity:.5;box-shadow:none}.statustext{font-size:12px;color:#aeb4bf;padding:0 2px}"
".backdrop{position:fixed;inset:0;background:rgba(16,19,26,.24);opacity:0;pointer-events:none;transition:opacity .24s ease;z-index:30}.backdrop.active{opacity:1;pointer-events:auto}.sheet{position:fixed;left:0;right:0;bottom:0;z-index:40;background:rgba(247,247,250,.94);backdrop-filter:blur(20px);border-radius:26px 26px 0 0;transform:translateY(102%);transition:transform .28s cubic-bezier(.2,.8,.2,1);padding:10px 14px calc(18px + env(safe-area-inset-bottom));max-height:min(78dvh,760px);overflow:auto}.sheet.active{transform:translateY(0)}.handle{width:42px;height:5px;border-radius:999px;background:rgba(17,19,24,.14);margin:2px auto 12px}.sheetTitle{font-size:17px;font-weight:700;text-align:center;margin-bottom:8px}.sheetIntro{font-size:13px;line-height:1.5;color:var(--muted);text-align:center;margin:0 0 14px}.group{background:rgba(255,255,255,.72);border:1px solid rgba(18,24,35,.06);border-radius:20px;padding:14px;display:grid;gap:10px;margin-bottom:12px}.group h3{margin:0;font-size:15px}.sheetMuted{font-size:13px;line-height:1.5;color:var(--muted)}.mono{font:13px/1.5 ui-monospace,SFMono-Regular,Consolas,monospace;color:#434a57}.qrBox{display:grid;gap:8px;justify-items:center}.qrBox img{width:min(52vw,190px);height:min(52vw,190px);background:#fff;border-radius:20px;padding:8px;border:1px solid rgba(18,24,35,.06)}.field{display:grid;gap:7px}.label{font-size:11px;letter-spacing:.12em;text-transform:uppercase;color:#717987;font-weight:700}.field input,.field select{width:100%;min-height:46px;border:0;border-radius:14px;background:#eef1f6;color:#141821;padding:0 14px;font:500 16px/1 system-ui;outline:none}.field select{-webkit-appearance:none;-moz-appearance:none;appearance:none;padding:0 40px 0 14px;background-image:url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23808692' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='M6 9l6 6 6-6'/%3E%3C/svg%3E\");background-repeat:no-repeat;background-position:right 14px center;background-size:16px 16px}.row{display:flex;gap:10px;flex-wrap:wrap}.sheet button{min-height:46px;border:0;border-radius:14px;padding:0 16px;font:600 14px/1 system-ui}.primary{background:linear-gradient(180deg,#4e82ff,#3367f5);color:#fff}.secondary{background:#e7ebf2;color:#1d2430}.stagebar{display:flex;gap:8px;flex-wrap:wrap}.stage{padding:8px 11px;border-radius:999px;background:#edf1f6;color:#7b8491;font-size:12px;font-weight:700}.stage.active{background:#dbe8ff;color:#2557ca}.stage.done{background:#def6e8;color:#167e46}@keyframes blink{50%{opacity:0}}</style></head><body>"
"<div class='app'><header class='topbar'><div></div><div class='titlewrap'><div id='titleText' class='title'>Ready</div><div id='statusDot' class='statusdot'></div></div><button id='hubBtn' class='iconbtn subtle' type='button' aria-label='Open device hub'><svg viewBox='0 0 24 24' aria-hidden='true'><line x1='5' y1='6' x2='19' y2='6'/><circle cx='9' cy='6' r='2.2' fill='#1f2430' stroke='none'/><line x1='5' y1='12' x2='19' y2='12'/><circle cx='15' cy='12' r='2.2' fill='#1f2430' stroke='none'/><line x1='5' y1='18' x2='19' y2='18'/><circle cx='11' cy='18' r='2.2' fill='#1f2430' stroke='none'/></svg></button></header><main id='timeline' class='chat'><div id='emptyState' class='empty'><div class='emptyBox'><div class='logo'>Z</div><div class='emptyTitle'>How can I help?</div><div class='emptyHint'>Ask for chat, sensor reads, tool calls, or device help.</div></div></div></main><div class='composerWrap'><div class='composer'><div class='inputShell'><textarea id='message' placeholder='Message zclaw...'></textarea><button id='send' class='sendBtn hidden' type='button'>&uarr;</button></div><div id='status' class='statustext'>Ready</div></div></div><div id='backdrop' class='backdrop'></div><section id='hubSheet' class='sheet'><div class='handle'></div><div class='sheetTitle'>Device Hub</div><p class='sheetIntro'>Connection, quick access, and configuration stay here so chat can stay clean.</p><div class='group'><h3>Connection</h3><div id='deviceInfo' class='mono'>Loading device info...</div><div class='stagebar' id='stagebar'><span class='stage done' data-stage='queued'>Queued</span><span class='stage' data-stage='thinking'>Thinking</span><span class='stage' data-stage='tool'>Tool</span><span class='stage' data-stage='streaming'>Streaming</span><span class='stage' data-stage='final'>Final</span></div></div><div class='group'><h3>Open on phone</h3><div class='qrBox'><img id='deviceQr' alt='Device QR'><div id='deviceQrLabel' class='sheetMuted'>Scan current device IP.</div></div></div><div class='group'><h3>Wi-Fi</h3><div class='field'><span class='label'>ssid</span><input id='settingsSsid' placeholder='New Wi-Fi SSID'></div><div class='field'><span class='label'>password</span><input id='settingsPassword' type='password' placeholder='New Wi-Fi password'></div><div class='row'><button id='saveWifiSettings' class='primary' type='button'>Save Wi-Fi + reboot</button><button id='restartSetup' class='secondary' type='button'>Reopen setup hotspot</button></div></div><div class='group'><h3>AI</h3><div class='field'><span class='label'>backend</span><select id='settingsBackend'><option value='openai'>OpenAI-compatible</option><option value='anthropic'>Anthropic</option><option value='openrouter'>OpenRouter</option><option value='ollama'>Ollama</option></select></div><div class='field'><span class='label'>model</span><input id='settingsModel' placeholder='Model, e.g. deepseek-chat'></div><div class='field'><span class='label'>base url</span><input id='settingsApiUrl' placeholder='Base URL, e.g. https://api.deepseek.com/v1'></div><div class='field'><span class='label'>api key</span><input id='settingsApiKey' type='password' placeholder='Leave blank to keep current key'></div><div class='row'><button id='saveLlmSettings' class='primary' type='button'>Save AI settings</button></div></div><div id='settingsStatus' class='sheetMuted'>Update Wi-Fi and AI settings here without using a serial cable.</div></section></div>"
"<script>"
"const timeline=document.getElementById('timeline');const statusEl=document.getElementById('status');const input=document.getElementById('message');const sendBtn=document.getElementById('send');const emptyState=document.getElementById('emptyState');"
"const settingsStatusEl=document.getElementById('settingsStatus');const deviceQrEl=document.getElementById('deviceQr');const deviceQrLabelEl=document.getElementById('deviceQrLabel');const backdrop=document.getElementById('backdrop');const hubSheet=document.getElementById('hubSheet');const statusDot=document.getElementById('statusDot');const titleText=document.getElementById('titleText');"
"function setQrImage(imgEl,labelEl,url,label){if(!imgEl)return;const target=(url||'').trim();if(!target){imgEl.removeAttribute('src');if(labelEl)labelEl.textContent='QR unavailable';return;}imgEl.src='/api/qr.svg?text='+encodeURIComponent(target);if(labelEl)labelEl.textContent=(label||'Scan to open')+': '+target;}"
"function syncComposer(){const hasText=input.value.trim().length>0;sendBtn.classList.toggle('hidden',!hasText&&!chatBusy);}"
"function autoGrow(){input.style.height='44px';input.style.height=Math.min(140,input.scrollHeight)+'px';syncComposer();}"
"function openSheet(){backdrop.classList.add('active');hubSheet.classList.add('active');}function closeSheets(){backdrop.classList.remove('active');hubSheet.classList.remove('active');}"
"let activeStreamTimer=null;"
"let chatBusy=false;"
"function setChatBusy(busy){chatBusy=!!busy;input.disabled=chatBusy;sendBtn.disabled=chatBusy;sendBtn.classList.toggle('hidden',!chatBusy&&input.value.trim().length===0);if(!chatBusy){input.focus();}}"
"function bubble(kind,text){const el=document.createElement('div');if(emptyState&&emptyState.parentNode){emptyState.remove();}el.className='bubble '+kind;el.textContent=text;timeline.appendChild(el);timeline.scrollTop=timeline.scrollHeight;return el;}"
"function detailBubble(summary,detail){const el=document.createElement('div');const summaryEl=document.createElement('div');const detailEl=document.createElement('div');const toggle=document.createElement('button');el.className='bubble system';summaryEl.className='summary';summaryEl.textContent=summary;detailEl.className='detail';detailEl.textContent=detail||summary;toggle.className='toggle';toggle.type='button';toggle.textContent='Show details';toggle.onclick=()=>{const expanded=el.classList.toggle('expanded');toggle.textContent=expanded?'Hide details':'Show details';timeline.scrollTop=timeline.scrollHeight;};el.appendChild(summaryEl);if(detail&&detail!==summary){el.appendChild(detailEl);el.appendChild(toggle);}timeline.appendChild(el);timeline.scrollTop=timeline.scrollHeight;return el;}"
"function setStreaming(el,on){if(!el)return;el.classList.toggle('streaming',!!on);}"
"function setStatus(mode,detail,klass){statusEl.textContent=detail||mode;titleText.textContent=mode||'Ready';statusDot.style.background=klass==='error'?'var(--error)':klass==='busy'?'var(--warn)':'var(--good)';statusDot.style.boxShadow=klass==='error'?'0 0 0 4px rgba(217,79,69,.12)':klass==='busy'?'0 0 0 4px rgba(255,159,10,.14)':'0 0 0 4px rgba(24,169,87,.12)';}"
"function setStage(stage){const nodes=document.querySelectorAll('#stagebar .stage');let reached=true;nodes.forEach((node)=>{const current=node.getAttribute('data-stage');if(reached){node.classList.add('done');}if(current===stage){node.classList.add('active');reached=false;}else{node.classList.remove('active');if(!reached){node.classList.remove('done');}}});}"
"function summarizeToolResult(text){if(!text)return'Done';const normalized=text.replace(/^Tool result:\\s*/i,'').trim();const primary=normalized.split('|').map(part=>part.trim()).filter(Boolean).slice(0,3).join(' | ');return primary.length>96?primary.slice(0,93)+'...':primary;}"
"function smoothAppend(target,chunk){if(!target||!chunk)return;target.dataset.queue=(target.dataset.queue||'')+chunk;if(activeStreamTimer)return;activeStreamTimer=setInterval(()=>{const queued=target.dataset.queue||'';if(!queued){clearInterval(activeStreamTimer);activeStreamTimer=null;return;}const step=Math.max(1,Math.min(12,queued.length>48?12:Math.ceil(queued.length/3)));target.textContent+=(queued.slice(0,step));target.dataset.queue=queued.slice(step);timeline.scrollTop=timeline.scrollHeight;},24);}"
"async function send(){if(chatBusy)return;const message=input.value.trim();if(!message)return;setChatBusy(true);input.value='';autoGrow();bubble('user',message);const pending=bubble('agent','Thinking...');setStatus('Queued','Waiting for device','busy');setStage('queued');"
"const res=await fetch('/api/chat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({message})});"
"const data=await res.json();if(!res.ok){pending.className='bubble error';pending.textContent=data.error||'Chat request failed';setStatus('Error','Chat request failed','error');setChatBusy(false);return;}"
"const es=new EventSource('/api/chat/stream?id='+encodeURIComponent(data.id));"
"es.addEventListener('queued',()=>{setStatus('Queued','Job accepted','busy');setStage('queued');});"
"es.addEventListener('thinking',()=>{setStatus('Thinking','Model is planning','busy');setStage('thinking');setStreaming(pending,true);pending.textContent='Thinking...';});"
"es.addEventListener('tool_call',(ev)=>{const payload=JSON.parse(ev.data);bubble('system','Using tool: '+(payload.message||'unknown'));setStatus('Tool','Calling '+(payload.message||'tool'),'busy');setStage('tool');});"
"es.addEventListener('tool_result',(ev)=>{const payload=JSON.parse(ev.data);detailBubble('Tool result: '+summarizeToolResult(payload.message||'done'),payload.message||'done');setStatus('Tool','Result received','busy');setStage('tool');});"
"es.addEventListener('delta',(ev)=>{const payload=JSON.parse(ev.data);pending.className='bubble agent';setStreaming(pending,true);if(pending.textContent==='Thinking...'){pending.textContent='';}smoothAppend(pending,payload.chunk||'');setStatus('Streaming','Reply in progress','busy');setStage('streaming');});"
"es.addEventListener('final',(ev)=>{const payload=JSON.parse(ev.data);pending.className='bubble agent';setStreaming(pending,false);pending.dataset.queue='';if(activeStreamTimer){clearInterval(activeStreamTimer);activeStreamTimer=null;}pending.textContent=payload.reply||'';setStatus('Ready','Reply complete','live');setStage('final');setChatBusy(false);es.close();});"
"es.addEventListener('error',(ev)=>{pending.className='bubble error';setStreaming(pending,false);pending.textContent='Stream failed';setStatus('Error','Stream failed','error');setChatBusy(false);es.close();});"
"es.addEventListener('failure',(ev)=>{const payload=JSON.parse(ev.data);pending.className='bubble error';setStreaming(pending,false);pending.textContent=payload.error||'Request failed';setStatus('Error',payload.error||'Request failed','error');setChatBusy(false);es.close();});}"
"async function loadSettings(){try{const [statusRes,settingsRes]=await Promise.all([fetch('/api/status'),fetch('/api/settings')]);const statusData=await statusRes.json();const settingsData=await settingsRes.json();if(statusRes.ok){const ipUrl=statusData.ip?('http://'+statusData.ip):'';document.getElementById('deviceInfo').textContent='Current IP: '+(statusData.ip||'(none)')+'\\nSetup hotspot: http://192.168.4.1 ('+(statusData.setup_ssid||'zclaw-setup')+')\\nWi-Fi saved: '+(statusData.wifi_configured?'yes':'no')+' | API ready: '+(statusData.llm_configured?'yes':'no');setQrImage(deviceQrEl,deviceQrLabelEl,ipUrl||window.location.origin,'Scan current device IP');}if(settingsRes.ok){document.getElementById('settingsSsid').value=settingsData.wifi_ssid||'';document.getElementById('settingsBackend').value=settingsData.llm_backend||'openai';document.getElementById('settingsModel').value=settingsData.llm_model||'';document.getElementById('settingsApiUrl').value=settingsData.llm_api_url||'';}}catch(err){document.getElementById('deviceInfo').textContent='Device info unavailable';setQrImage(deviceQrEl,deviceQrLabelEl,window.location.origin,'Scan current page');}}"
"async function saveWifiSettings(){const payload={wifi_ssid:document.getElementById('settingsSsid').value.trim(),wifi_pass:document.getElementById('settingsPassword').value};settingsStatusEl.textContent='Saving Wi-Fi...';try{const res=await fetch('/api/settings/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const data=await res.json();if(!res.ok){settingsStatusEl.textContent=data.error||('Save failed ('+res.status+')');return;}settingsStatusEl.textContent='Wi-Fi saved. Device is rebooting now. Reopen the new DHCP IP shown on the OLED screen after it rejoins your router.';}catch(err){settingsStatusEl.textContent='Network error: '+err.message;}}"
"async function saveLlmSettings(){const payload={llm_backend:document.getElementById('settingsBackend').value,llm_model:document.getElementById('settingsModel').value.trim(),llm_api_url:document.getElementById('settingsApiUrl').value.trim(),api_key:document.getElementById('settingsApiKey').value.trim()};settingsStatusEl.textContent='Saving AI settings...';try{const res=await fetch('/api/settings/llm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});const data=await res.json();if(!res.ok){settingsStatusEl.textContent=data.error||('Save failed ('+res.status+')');return;}document.getElementById('settingsApiKey').value='';settingsStatusEl.textContent='AI settings saved without reboot.';loadSettings();}catch(err){settingsStatusEl.textContent='Network error: '+err.message;}}"
"async function restartToSetup(){settingsStatusEl.textContent='Rebooting into setup hotspot...';try{const res=await fetch('/api/setup-mode',{method:'POST'});const data=await res.json();if(!res.ok){settingsStatusEl.textContent=data.error||('Restart failed ('+res.status+')');return;}settingsStatusEl.textContent='Restarting. Reconnect your phone to the zclaw setup hotspot in a few seconds.';}catch(err){settingsStatusEl.textContent='Network error: '+err.message;}}"
"document.getElementById('send').onclick=send;document.getElementById('hubBtn').onclick=openSheet;backdrop.onclick=closeSheets;input.addEventListener('input',autoGrow);input.addEventListener('keydown',(e)=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();send();}});"
"document.getElementById('saveWifiSettings').onclick=saveWifiSettings;document.getElementById('saveLlmSettings').onclick=saveLlmSettings;document.getElementById('restartSetup').onclick=restartToSetup;"
"setStatus('Ready','Idle','live');setStage('final');"
"autoGrow();loadSettings();"
"setInterval(loadSettings,10000);"
"</script></body></html>";

static void web_ui_send_json(httpd_req_t *req, int status, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    if (!body) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"json_encode_failed\"}");
        return;
    }

    if (status == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (status == 404) {
        httpd_resp_set_status(req, "404 Not Found");
    } else if (status == 409) {
        httpd_resp_set_status(req, "409 Conflict");
    } else if (status == 500) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    } else {
        httpd_resp_set_status(req, "200 OK");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
}

static esp_err_t web_ui_send_error(httpd_req_t *req, int status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(json, "error", message ? message : "unknown");
    web_ui_send_json(req, status, json);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t web_ui_get_query_param(httpd_req_t *req, const char *key, char *out, size_t out_len)
{
    int query_len;
    char *query = NULL;
    esp_err_t err = ESP_FAIL;

    if (!req || !key || !out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    query_len = httpd_req_get_url_query_len(req);
    if (query_len <= 0) {
        return ESP_ERR_NOT_FOUND;
    }

    query = calloc((size_t)query_len + 1, 1);
    if (!query) {
        return ESP_ERR_NO_MEM;
    }

    err = httpd_req_get_url_query_str(req, query, (size_t)query_len + 1);
    if (err == ESP_OK) {
        err = httpd_query_key_value(query, key, out, out_len);
        if (err != ESP_OK) {
            err = ESP_ERR_NOT_FOUND;
        }
    }

    free(query);
    return err;
}

static esp_err_t web_ui_send_qr_svg(httpd_req_t *req, const char *text)
{
    uint8_t temp_buf[WEB_UI_QR_TMP_BUF_LEN];
    uint8_t qr_buf[WEB_UI_QR_TMP_BUF_LEN];
    int size;
    int border = 2;
    int x;
    int y;

    if (!text || text[0] == '\0') {
        return web_ui_send_error(req, 400, "missing_qr_text");
    }

    if (!qrcodegen_encodeText(text, temp_buf, qr_buf, qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, 8, qrcodegen_Mask_AUTO, true)) {
        return web_ui_send_error(req, 400, "qr_text_too_long");
    }

    size = qrcodegen_getSize(qr_buf);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    {
        char header[256];
        int header_len = snprintf(header, sizeof(header),
                                  "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                                  "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\" shape-rendering=\"crispEdges\" viewBox=\"0 0 "
                                  "%d %d\"><rect width=\"100%%\" height=\"100%%\" fill=\"#ffffff\"/>"
                                  "<path d=\"",
                                  size + border * 2, size + border * 2);
        if (header_len <= 0 || header_len >= (int)sizeof(header)) {
            return ESP_FAIL;
        }
        httpd_resp_sendstr_chunk(req, header);
    }

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            if (qrcodegen_getModule(qr_buf, x, y)) {
                char segment[32];
                int segment_len = snprintf(segment, sizeof(segment), "M%d,%dh1v1h-1z",
                                           x + border, y + border);
                if (segment_len <= 0 || segment_len >= (int)sizeof(segment)) {
                    return ESP_FAIL;
                }
                httpd_resp_sendstr_chunk(req, segment);
            }
        }
    }

    httpd_resp_sendstr_chunk(req, "\" fill=\"#111111\"/></svg>");
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t web_ui_qr_get_handler(httpd_req_t *req)
{
    char text[WEB_UI_QR_TEXT_MAX_LEN + 1] = {0};
    esp_err_t err = web_ui_get_query_param(req, "text", text, sizeof(text));
    if (err != ESP_OK) {
        return web_ui_send_error(req, 400, "missing_qr_text");
    }
    return web_ui_send_qr_svg(req, text);
}

static esp_err_t web_ui_read_json(httpd_req_t *req, cJSON **json_out)
{
    char *buf = NULL;
    int total_len = req->content_len;
    int received = 0;

    if (!json_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *json_out = NULL;

    if (total_len <= 0 || total_len > 4096) {
        return ESP_ERR_INVALID_SIZE;
    }

    buf = calloc((size_t)total_len + 1, 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += ret;
    }

    *json_out = cJSON_Parse(buf);
    free(buf);
    return *json_out ? ESP_OK : ESP_FAIL;
}

static bool web_ui_validate_backend(const char *backend)
{
    return backend &&
           (strcmp(backend, "openai") == 0 ||
            strcmp(backend, "anthropic") == 0 ||
            strcmp(backend, "openrouter") == 0 ||
            strcmp(backend, "ollama") == 0);
}

static bool web_ui_backend_requires_api_key_name(const char *backend)
{
    return backend &&
           (strcmp(backend, "openai") == 0 ||
            strcmp(backend, "anthropic") == 0 ||
            strcmp(backend, "openrouter") == 0);
}

static bool web_ui_wifi_configured(void)
{
    char ssid[64] = {0};
    return memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid)) && ssid[0] != '\0';
}

static bool web_ui_llm_configured(void)
{
    char backend[16] = {0};
    char api_key[LLM_API_KEY_BUF_SIZE] = {0};

    if (!memory_get(NVS_KEY_LLM_BACKEND, backend, sizeof(backend)) || backend[0] == '\0') {
        return false;
    }

    if (!web_ui_backend_requires_api_key_name(backend)) {
        return true;
    }

    return memory_get(NVS_KEY_API_KEY, api_key, sizeof(api_key)) && api_key[0] != '\0';
}

static bool web_ui_device_configured(void)
{
    return web_ui_wifi_configured();
}

static void web_ui_delayed_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void web_ui_delayed_llm_reload_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));
    llm_init();
    vTaskDelete(NULL);
}

static esp_err_t web_ui_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (s_mode == WEB_UI_MODE_PROVISIONING) {
        httpd_resp_sendstr(req, k_setup_html);
    } else {
        httpd_resp_sendstr(req, k_chat_html);
    }
    return ESP_OK;
}

static esp_err_t web_ui_status_get_handler(httpd_req_t *req)
{
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip_info = {0};
    char ip_str[16] = {0};
    char setup_ssid[32] = {0};
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return ESP_FAIL;
    }

    netif = esp_netif_get_handle_from_ifkey(
        s_mode == WEB_UI_MODE_PROVISIONING ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    }

    device_identity_get_setup_ssid(setup_ssid, sizeof(setup_ssid));

    cJSON_AddBoolToObject(json, "configured", web_ui_device_configured());
    cJSON_AddBoolToObject(json, "wifi_configured", web_ui_wifi_configured());
    cJSON_AddBoolToObject(json, "llm_configured", web_ui_llm_configured());
    cJSON_AddStringToObject(json, "mode", s_mode == WEB_UI_MODE_PROVISIONING ? "provisioning" : "chat");
    cJSON_AddStringToObject(json, "ip", ip_str[0] ? ip_str : "");
    cJSON_AddStringToObject(json, "setup_ssid", setup_ssid);
    cJSON_AddBoolToObject(json, "ap_started", s_ap_started);
    web_ui_send_json(req, 200, json);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t web_ui_settings_get_handler(httpd_req_t *req)
{
    char ssid[64] = {0};
    char backend[16] = {0};
    char model[64] = {0};
    char api_url[160] = {0};
    char api_key[LLM_API_KEY_BUF_SIZE] = {0};
    char setup_url[32] = "http://192.168.4.1";
    cJSON *json = cJSON_CreateObject();

    if (!json) {
        return ESP_FAIL;
    }

    memory_get(NVS_KEY_WIFI_SSID, ssid, sizeof(ssid));
    memory_get(NVS_KEY_LLM_BACKEND, backend, sizeof(backend));
    memory_get(NVS_KEY_LLM_MODEL, model, sizeof(model));
    memory_get(NVS_KEY_LLM_API_URL, api_url, sizeof(api_url));
    memory_get(NVS_KEY_API_KEY, api_key, sizeof(api_key));
    cJSON_AddStringToObject(json, "wifi_ssid", ssid);
    cJSON_AddStringToObject(json, "llm_backend", backend[0] ? backend : "openai");
    cJSON_AddStringToObject(json, "llm_model", model);
    cJSON_AddStringToObject(json, "llm_api_url", api_url);
    cJSON_AddBoolToObject(json, "has_api_key", api_key[0] != '\0');
    cJSON_AddStringToObject(json, "setup_url", setup_url);
    web_ui_send_json(req, 200, json);
    cJSON_Delete(json);
    return ESP_OK;
}

static esp_err_t web_ui_captive_portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t web_ui_wifi_settings_post_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    const cJSON *ssid = NULL;
    const cJSON *pass = NULL;
    cJSON *resp = NULL;
    char wifi_error[96] = {0};
    if (web_ui_read_json(req, &json) != ESP_OK || !json) {
        return web_ui_send_error(req, 400, "invalid_json");
    }

    ESP_LOGI(TAG, "Saving Wi-Fi settings via web UI");

    ssid = cJSON_GetObjectItemCaseSensitive(json, "wifi_ssid");
    pass = cJSON_GetObjectItemCaseSensitive(json, "wifi_pass");

    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 400, "missing_required_fields");
    }

    if (!wifi_credentials_validate(ssid->valuestring, pass->valuestring, wifi_error, sizeof(wifi_error))) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 400, wifi_error);
    }

    if (memory_set(NVS_KEY_BOOT_COUNT, "0") != ESP_OK ||
        memory_set(NVS_KEY_WIFI_SSID, ssid->valuestring) != ESP_OK ||
        memory_set(NVS_KEY_WIFI_PASS, pass->valuestring) != ESP_OK) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 500, "failed_to_persist_settings");
    }

    memory_delete(NVS_KEY_SETUP_MODE);
    cJSON_Delete(json);

    resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", true);
    cJSON_AddStringToObject(resp, "message", "wifi_saved_restarting");
    web_ui_send_json(req, 200, resp);
    cJSON_Delete(resp);

    xTaskCreate(web_ui_delayed_restart_task, "web_reboot", 3072, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t web_ui_llm_settings_post_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    const cJSON *backend = NULL;
    const cJSON *api_key = NULL;
    const cJSON *model = NULL;
    const cJSON *api_url = NULL;
    const cJSON *clear_api_key = NULL;
    cJSON *resp = NULL;
    char effective_backend[16] = {0};

    if (web_ui_read_json(req, &json) != ESP_OK || !json) {
        return web_ui_send_error(req, 400, "invalid_json");
    }

    ESP_LOGI(TAG, "Saving AI settings via web UI");

    backend = cJSON_GetObjectItemCaseSensitive(json, "llm_backend");
    api_key = cJSON_GetObjectItemCaseSensitive(json, "api_key");
    model = cJSON_GetObjectItemCaseSensitive(json, "llm_model");
    api_url = cJSON_GetObjectItemCaseSensitive(json, "llm_api_url");
    clear_api_key = cJSON_GetObjectItemCaseSensitive(json, "clear_api_key");

    memory_get(NVS_KEY_LLM_BACKEND, effective_backend, sizeof(effective_backend));
    if (cJSON_IsString(backend) && backend->valuestring[0] != '\0') {
        if (!web_ui_validate_backend(backend->valuestring)) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 400, "invalid_backend");
        }
        snprintf(effective_backend, sizeof(effective_backend), "%s", backend->valuestring);
        if (memory_set(NVS_KEY_LLM_BACKEND, backend->valuestring) != ESP_OK) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 500, "failed_to_persist_settings");
        }
    } else if (effective_backend[0] == '\0') {
        snprintf(effective_backend, sizeof(effective_backend), "openai");
        if (memory_set(NVS_KEY_LLM_BACKEND, effective_backend) != ESP_OK) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 500, "failed_to_persist_settings");
        }
    }

    if (web_ui_backend_requires_api_key_name(effective_backend)) {
        bool clearing = cJSON_IsBool(clear_api_key) && cJSON_IsTrue(clear_api_key);
        bool updating = cJSON_IsString(api_key) && api_key->valuestring[0] != '\0';
        char existing_key[LLM_API_KEY_BUF_SIZE] = {0};

        memory_get(NVS_KEY_API_KEY, existing_key, sizeof(existing_key));
        if (!updating && clearing) {
            memory_delete(NVS_KEY_API_KEY);
        } else if (updating) {
            if (memory_set(NVS_KEY_API_KEY, api_key->valuestring) != ESP_OK) {
                cJSON_Delete(json);
                return web_ui_send_error(req, 500, "failed_to_persist_settings");
            }
        } else if (existing_key[0] == '\0' && s_mode == WEB_UI_MODE_PROVISIONING) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 400, "api_key_required");
        }
    } else if (cJSON_IsString(api_key) && api_key->valuestring[0] != '\0') {
        if (memory_set(NVS_KEY_API_KEY, api_key->valuestring) != ESP_OK) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 500, "failed_to_persist_settings");
        }
    }

    if (cJSON_HasObjectItem(json, "llm_model")) {
        if (cJSON_IsString(model) && model->valuestring[0] != '\0') {
            memory_set(NVS_KEY_LLM_MODEL, model->valuestring);
        } else {
            memory_delete(NVS_KEY_LLM_MODEL);
        }
    }

    if (cJSON_HasObjectItem(json, "llm_api_url")) {
        if (cJSON_IsString(api_url) && api_url->valuestring[0] != '\0') {
            memory_set(NVS_KEY_LLM_API_URL, api_url->valuestring);
        } else {
            memory_delete(NVS_KEY_LLM_API_URL);
        }
    }

    cJSON_Delete(json);

    resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", false);
    cJSON_AddBoolToObject(resp, "llm_configured", true);
    cJSON_AddStringToObject(resp, "message", "llm_settings_saved");
    web_ui_send_json(req, 200, resp);
    cJSON_Delete(resp);

    if (xTaskCreate(web_ui_delayed_llm_reload_task, "web_llm_reload", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Failed to schedule delayed LLM reload");
    }
    return ESP_OK;
}

static esp_err_t web_ui_setup_post_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    const cJSON *ssid = NULL;
    const cJSON *pass = NULL;
    const cJSON *backend = NULL;
    const cJSON *api_key = NULL;
    const cJSON *model = NULL;
    const cJSON *api_url = NULL;
    char wifi_error[96] = {0};
    char effective_backend[16] = {0};
    cJSON *resp = NULL;

    if (web_ui_read_json(req, &json) != ESP_OK || !json) {
        return web_ui_send_error(req, 400, "invalid_json");
    }

    ssid = cJSON_GetObjectItemCaseSensitive(json, "wifi_ssid");
    pass = cJSON_GetObjectItemCaseSensitive(json, "wifi_pass");
    backend = cJSON_GetObjectItemCaseSensitive(json, "llm_backend");
    api_key = cJSON_GetObjectItemCaseSensitive(json, "api_key");
    model = cJSON_GetObjectItemCaseSensitive(json, "llm_model");
    api_url = cJSON_GetObjectItemCaseSensitive(json, "llm_api_url");

    if (!cJSON_IsString(ssid) || !cJSON_IsString(pass)) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 400, "missing_required_fields");
    }

    if (!wifi_credentials_validate(ssid->valuestring, pass->valuestring, wifi_error, sizeof(wifi_error))) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 400, wifi_error);
    }

    if (cJSON_IsString(backend) && backend->valuestring[0] != '\0') {
        if (!web_ui_validate_backend(backend->valuestring)) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 400, "invalid_backend");
        }
        snprintf(effective_backend, sizeof(effective_backend), "%s", backend->valuestring);
    } else {
        memory_get(NVS_KEY_LLM_BACKEND, effective_backend, sizeof(effective_backend));
    }

    if (web_ui_backend_requires_api_key_name(effective_backend) &&
        ((!cJSON_IsString(api_key) || api_key->valuestring[0] == '\0') && !web_ui_llm_configured())) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 400, "api_key_required");
    }

    if (memory_set(NVS_KEY_BOOT_COUNT, "0") != ESP_OK ||
        memory_set(NVS_KEY_WIFI_SSID, ssid->valuestring) != ESP_OK ||
        memory_set(NVS_KEY_WIFI_PASS, pass->valuestring) != ESP_OK) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 500, "failed_to_persist_settings");
    }

    memory_delete(NVS_KEY_SETUP_MODE);

    if (effective_backend[0] != '\0' && memory_set(NVS_KEY_LLM_BACKEND, effective_backend) != ESP_OK) {
        cJSON_Delete(json);
        return web_ui_send_error(req, 500, "failed_to_persist_settings");
    }
    if (cJSON_IsString(api_key) && api_key->valuestring[0] != '\0') {
        if (memory_set(NVS_KEY_API_KEY, api_key->valuestring) != ESP_OK) {
            cJSON_Delete(json);
            return web_ui_send_error(req, 500, "failed_to_persist_settings");
        }
    }
    if (cJSON_HasObjectItem(json, "llm_model")) {
        if (cJSON_IsString(model) && model->valuestring[0] != '\0') {
            memory_set(NVS_KEY_LLM_MODEL, model->valuestring);
        } else {
            memory_delete(NVS_KEY_LLM_MODEL);
        }
    }
    if (cJSON_HasObjectItem(json, "llm_api_url")) {
        if (cJSON_IsString(api_url) && api_url->valuestring[0] != '\0') {
            memory_set(NVS_KEY_LLM_API_URL, api_url->valuestring);
        } else {
            memory_delete(NVS_KEY_LLM_API_URL);
        }
    }
    cJSON_Delete(json);

    resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "rebooting", true);
    cJSON_AddStringToObject(resp, "message", "saved_restarting");
    web_ui_send_json(req, 200, resp);
    cJSON_Delete(resp);

    xTaskCreate(web_ui_delayed_restart_task, "web_reboot", 3072, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t web_ui_setup_mode_post_handler(httpd_req_t *req)
{
    cJSON *resp = NULL;

    if (memory_set(NVS_KEY_BOOT_COUNT, "0") != ESP_OK ||
        memory_set(NVS_KEY_SETUP_MODE, "1") != ESP_OK) {
        return web_ui_send_error(req, 500, "failed_to_enable_setup_mode");
    }

    resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_FAIL;
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "message", "restarting_to_setup_mode");
    web_ui_send_json(req, 200, resp);
    cJSON_Delete(resp);

    xTaskCreate(web_ui_delayed_restart_task, "web_setup_reboot", 3072, NULL, 1, NULL);
    return ESP_OK;
}

static chat_job_t *web_ui_find_job_by_id_locked(int id)
{
    for (int i = 0; i < WEB_UI_CHAT_JOB_COUNT; i++) {
        if (s_jobs[i].state != CHAT_JOB_EMPTY && s_jobs[i].id == id) {
            return &s_jobs[i];
        }
    }
    return NULL;
}

static chat_job_t *web_ui_allocate_job_locked(void)
{
    for (int i = 0; i < WEB_UI_CHAT_JOB_COUNT; i++) {
        if (s_jobs[i].state == CHAT_JOB_EMPTY) {
            memset(&s_jobs[i], 0, sizeof(s_jobs[i]));
            s_jobs[i].id = s_next_job_id++;
            if (s_next_job_id <= 0) {
                s_next_job_id = 1;
            }
            s_jobs[i].state = CHAT_JOB_QUEUED;
            s_jobs[i].streamed_len = 0;
            return &s_jobs[i];
        }
    }
    return NULL;
}

static bool web_ui_stream_chunk_cb(const char *chunk, size_t chunk_len, void *ctx)
{
    chat_job_t *job = (chat_job_t *)ctx;

    if (!job || !chunk || chunk_len == 0) {
        return true;
    }

    xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
    if (job->state == CHAT_JOB_EMPTY) {
        xSemaphoreGive(s_jobs_mutex);
        return false;
    }
    if (!text_buffer_append(job->reply, &job->streamed_len, sizeof(job->reply), chunk, chunk_len)) {
        job->state = CHAT_JOB_FAILED;
        snprintf(job->error, sizeof(job->error), "stream_reply_too_large");
        xSemaphoreGive(s_jobs_mutex);
        return false;
    }
    xSemaphoreGive(s_jobs_mutex);
    return true;
}

static bool web_ui_agent_status_cb(const char *event, const char *message, void *ctx)
{
    chat_job_t *job = (chat_job_t *)ctx;

    if (!job || !event || event[0] == '\0') {
        return true;
    }

    xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
    if (job->state == CHAT_JOB_EMPTY) {
        xSemaphoreGive(s_jobs_mutex);
        return false;
    }

    uint32_t seq = ++job->status_next_seq;
    chat_status_event_t *slot = &job->status_events[(seq - 1U) % WEB_UI_STATUS_EVENT_COUNT];
    memset(slot, 0, sizeof(*slot));
    slot->seq = seq;
    strncpy(slot->event, event, sizeof(slot->event) - 1);
    if (message) {
        strncpy(slot->message, message, sizeof(slot->message) - 1);
    }
    xSemaphoreGive(s_jobs_mutex);
    return true;
}

static void web_ui_chat_job_task(void *arg)
{
    chat_job_t *job = (chat_job_t *)arg;
    esp_err_t err;

    if (!job) {
        vTaskDelete(NULL);
        return;
    }

    xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
    job->state = CHAT_JOB_RUNNING;
    xSemaphoreGive(s_jobs_mutex);

    err = agent_process_web_request_streaming(job->prompt,
                                              job->reply,
                                              sizeof(job->reply),
                                              web_ui_agent_status_cb,
                                              job,
                                              web_ui_stream_chunk_cb,
                                              job);

    xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        job->state = CHAT_JOB_DONE;
    } else {
        job->state = CHAT_JOB_FAILED;
        snprintf(job->error, sizeof(job->error), "agent_request_failed");
    }
    xSemaphoreGive(s_jobs_mutex);

    vTaskDelete(NULL);
}

static esp_err_t web_ui_chat_post_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    const cJSON *message = NULL;
    chat_job_t *job = NULL;

    if (s_mode != WEB_UI_MODE_CHAT) {
        return web_ui_send_error(req, 409, "chat_not_available");
    }

    if (web_ui_read_json(req, &json) != ESP_OK || !json) {
        return web_ui_send_error(req, 400, "invalid_json");
    }

    message = cJSON_GetObjectItemCaseSensitive(json, "message");
    if (!cJSON_IsString(message) || message->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return web_ui_send_error(req, 400, "message_required");
    }

    xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
    job = web_ui_allocate_job_locked();
    if (job) {
        strncpy(job->prompt, message->valuestring, sizeof(job->prompt) - 1);
        job->prompt[sizeof(job->prompt) - 1] = '\0';
    }
    xSemaphoreGive(s_jobs_mutex);
    cJSON_Delete(json);

    if (!job) {
        return web_ui_send_error(req, 409, "chat_busy");
    }

    if (xTaskCreate(web_ui_chat_job_task,
                    "web_chat_job",
                    WEB_UI_CHAT_JOB_STACK_SIZE,
                    job,
                    4,
                    NULL) != pdPASS) {
        xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
        memset(job, 0, sizeof(*job));
        xSemaphoreGive(s_jobs_mutex);
        return web_ui_send_error(req, 500, "job_create_failed");
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return ESP_FAIL;
    }
    cJSON_AddNumberToObject(resp, "id", job->id);
    cJSON_AddBoolToObject(resp, "ok", true);
    web_ui_send_json(req, 200, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t web_ui_send_sse_event(httpd_req_t *req, const char *event, const char *data)
{
    char header[64];
    int written = snprintf(header, sizeof(header), "event: %s\n", event ? event : "message");
    if (written < 0) {
        return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, header, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    if (data) {
        if (httpd_resp_send_chunk(req, "data: ", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, data, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, "\n\n", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
    } else if (httpd_resp_send_chunk(req, "\n", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t web_ui_chat_stream_get_handler(httpd_req_t *req)
{
    char query[32] = {0};
    char id_buf[16] = {0};
    int job_id = 0;
    int waited_ms = 0;
    chat_job_t snapshot = {0};
    size_t sent_len = 0;
    uint32_t sent_status_seq = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_buf, sizeof(id_buf)) != ESP_OK) {
        return web_ui_send_error(req, 400, "missing_id");
    }

    job_id = atoi(id_buf);
    if (job_id <= 0) {
        return web_ui_send_error(req, 400, "invalid_id");
    }

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    if (web_ui_send_sse_event(req, "queued", "{}") != ESP_OK ||
        web_ui_send_sse_event(req, "thinking", "{}") != ESP_OK) {
        return ESP_FAIL;
    }

    while (waited_ms < WEB_UI_CHAT_STREAM_WAIT_MS) {
        bool job_terminal = false;

        xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
        chat_job_t *job = web_ui_find_job_by_id_locked(job_id);
        if (job) {
            memcpy(&snapshot, job, sizeof(snapshot));
            job_terminal = (job->state == CHAT_JOB_DONE || job->state == CHAT_JOB_FAILED);
            if (job_terminal) {
                job->state = CHAT_JOB_STREAMING;
            }
        }
        xSemaphoreGive(s_jobs_mutex);

        while (snapshot.id == job_id && sent_status_seq < snapshot.status_next_seq) {
            uint32_t next_seq = sent_status_seq + 1U;
            chat_status_event_t *slot = &snapshot.status_events[(next_seq - 1U) % WEB_UI_STATUS_EVENT_COUNT];
            cJSON *json = NULL;
            char *body = NULL;

            if (slot->seq != next_seq || slot->event[0] == '\0') {
                sent_status_seq = next_seq;
                continue;
            }

            json = cJSON_CreateObject();
            if (!json) {
                return ESP_FAIL;
            }
            cJSON_AddStringToObject(json, "message", slot->message[0] ? slot->message : slot->event);
            body = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (!body) {
                return ESP_FAIL;
            }
            if (web_ui_send_sse_event(req, slot->event, body) != ESP_OK) {
                free(body);
                return ESP_FAIL;
            }
            free(body);
            sent_status_seq = next_seq;
        }

        while (snapshot.id == job_id && snapshot.streamed_len > sent_len) {
            cJSON *json = cJSON_CreateObject();
            char *body = NULL;
            size_t chunk_len = snapshot.streamed_len - sent_len;
            char chunk_buf[256] = {0};
            if (chunk_len >= sizeof(chunk_buf)) {
                chunk_len = sizeof(chunk_buf) - 1;
            }
            memcpy(chunk_buf, snapshot.reply + sent_len, chunk_len);
            chunk_buf[chunk_len] = '\0';
            if (!json) {
                return ESP_FAIL;
            }
            cJSON_AddStringToObject(json, "chunk", chunk_buf);
            body = cJSON_PrintUnformatted(json);
            cJSON_Delete(json);
            if (!body) {
                return ESP_FAIL;
            }
            if (web_ui_send_sse_event(req, "delta", body) != ESP_OK) {
                free(body);
                return ESP_FAIL;
            }
            free(body);
            sent_len += chunk_len;
        }

        if (snapshot.id == job_id && job_terminal && sent_len >= snapshot.streamed_len) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(WEB_UI_CHAT_POLL_MS));
        waited_ms += WEB_UI_CHAT_POLL_MS;
    }

    if (snapshot.id != job_id || (snapshot.state != CHAT_JOB_DONE && snapshot.state != CHAT_JOB_STREAMING && snapshot.state != CHAT_JOB_FAILED)) {
        web_ui_send_sse_event(req, "failure", "{\"error\":\"timeout\"}");
    } else if (snapshot.state == CHAT_JOB_FAILED) {
        cJSON *json = cJSON_CreateObject();
        char *body = NULL;
        if (!json) {
            return ESP_FAIL;
        }
        cJSON_AddStringToObject(json, "error", snapshot.error[0] ? snapshot.error : "request_failed");
        body = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        if (!body) {
            return ESP_FAIL;
        }
        web_ui_send_sse_event(req, "failure", body);
        free(body);
    } else {
        cJSON *json = cJSON_CreateObject();
        char *body = NULL;
        if (!json) {
            return ESP_FAIL;
        }
        cJSON_AddStringToObject(json, "reply", snapshot.reply);
        body = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        if (!body) {
            return ESP_FAIL;
        }
        web_ui_send_sse_event(req, "final", body);
        free(body);
    }

    httpd_resp_send_chunk(req, NULL, 0);

    xSemaphoreTake(s_jobs_mutex, portMAX_DELAY);
    chat_job_t *job = web_ui_find_job_by_id_locked(job_id);
    if (job) {
        memset(job, 0, sizeof(*job));
    }
    xSemaphoreGive(s_jobs_mutex);

    return ESP_OK;
}

static esp_err_t web_ui_register_handlers(httpd_handle_t server)
{
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_ui_root_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t status = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = web_ui_status_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t qr = {
        .uri = "/api/qr.svg",
        .method = HTTP_GET,
        .handler = web_ui_qr_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t settings_get = {
        .uri = "/api/settings",
        .method = HTTP_GET,
        .handler = web_ui_settings_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t setup = {
        .uri = "/api/setup",
        .method = HTTP_POST,
        .handler = web_ui_setup_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t settings_wifi = {
        .uri = "/api/settings/wifi",
        .method = HTTP_POST,
        .handler = web_ui_wifi_settings_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t settings_llm = {
        .uri = "/api/settings/llm",
        .method = HTTP_POST,
        .handler = web_ui_llm_settings_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t chat = {
        .uri = "/api/chat",
        .method = HTTP_POST,
        .handler = web_ui_chat_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t chat_stream = {
        .uri = "/api/chat/stream",
        .method = HTTP_GET,
        .handler = web_ui_chat_stream_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t setup_mode = {
        .uri = "/api/setup-mode",
        .method = HTTP_POST,
        .handler = web_ui_setup_mode_post_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t captive_generate_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = web_ui_captive_portal_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t captive_hotspot_detect = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = web_ui_captive_portal_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t captive_connect_test = {
        .uri = "/connecttest.txt",
        .method = HTTP_GET,
        .handler = web_ui_captive_portal_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t captive_success = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = web_ui_captive_portal_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t captive_fwlink = {
        .uri = "/fwlink",
        .method = HTTP_GET,
        .handler = web_ui_captive_portal_get_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), TAG, "root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &status), TAG, "status handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &qr), TAG, "qr handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_get), TAG, "settings get handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &setup), TAG, "setup handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_wifi), TAG, "settings wifi handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &settings_llm), TAG, "settings llm handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &setup_mode), TAG, "setup-mode handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &chat), TAG, "chat handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &chat_stream), TAG, "chat stream handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_generate_204), TAG, "generate_204 handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_hotspot_detect), TAG, "hotspot-detect handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_connect_test), TAG, "connecttest handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_success), TAG, "ncsi handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &captive_fwlink), TAG, "fwlink handler");
    return ESP_OK;
}

static esp_err_t web_ui_start_httpd(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 16;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");
    ESP_RETURN_ON_ERROR(web_ui_register_handlers(s_server), TAG, "register handlers");
    s_httpd_started = true;
    return ESP_OK;
}

static esp_err_t web_ui_start_softap(void)
{
    esp_err_t err;
    char ssid[32] = {0};
    wifi_config_t wifi_config = {0};

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    if (!s_wifi_ap_netif_ready) {
        esp_netif_create_default_wifi_ap();
        s_wifi_ap_netif_ready = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    device_identity_get_setup_ssid(ssid, sizeof(ssid));

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, WEB_UI_AP_PASS, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);
    wifi_config.ap.max_connection = WEB_UI_AP_MAX_CONN;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.channel = 6;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP");

    s_ap_started = true;
    ESP_LOGI(TAG, "Provisioning AP ready: SSID=%s password=%s ip=192.168.4.1", ssid, WEB_UI_AP_PASS);
    return ESP_OK;
}

esp_err_t web_ui_start(web_ui_mode_t mode)
{
    if (s_httpd_started) {
        return ESP_OK;
    }

    s_mode = mode;
    if (!s_jobs_mutex) {
        s_jobs_mutex = xSemaphoreCreateMutex();
        if (!s_jobs_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (mode == WEB_UI_MODE_PROVISIONING) {
        ESP_RETURN_ON_ERROR(web_ui_start_softap(), TAG, "softap");
    }

    ESP_RETURN_ON_ERROR(web_ui_start_httpd(), TAG, "httpd");
    ESP_LOGI(TAG, "Web UI started in %s mode", mode == WEB_UI_MODE_PROVISIONING ? "provisioning" : "chat");
    return ESP_OK;
}

bool web_ui_is_running(void)
{
    return s_httpd_started;
}

