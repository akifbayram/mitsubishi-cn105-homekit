#!/usr/bin/env python3
"""Exercise production HTTP startup and the main-loop retry boundary."""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
server = (ROOT / 'main/web_server.cpp').read_text()
main = (ROOT / 'main/main.cpp').read_text()
begin = re.search(r'^(void|bool) WebUI::begin\([^)]*\)\s*\{.*?^\}', server, re.M | re.S)
assert begin
startup = main[main.index('        // ── Web server deferred init'):main.index('        // ── WebSocket state push')]
# The sensor loops and OTA validation gate on network readiness, never on
# the web server having started.
for gate in ('if (networkReady && now - lastBleLoop', 'if (networkReady && now - lastLinkSensorLoop',
             '!firmwareValidated && networkReady'):
    assert gate in main, gate

prefix = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
using esp_err_t=int;
using httpd_handle_t=void*;
constexpr int ESP_OK=0,ESP_FAIL=-1,HTTP_GET=0,HTTP_POST=1;
struct httpd_req_t{};
struct CN105Controller{};
using Handler=esp_err_t(*)(httpd_req_t*);
struct httpd_uri_t {
 const char* uri;int method;Handler handler;void* user_ctx;
 bool is_websocket,handle_ws_control_frames;const char* supported_subprotocol;
 Handler ws_pre_handshake_cb=nullptr;
};
struct httpd_config_t {
 int server_port,ctrl_port,stack_size,max_uri_handlers,max_open_sockets;
 bool lru_purge_enable;
 esp_err_t(*open_fn)(httpd_handle_t,int);
 void(*close_fn)(httpd_handle_t,int);
 void* global_user_ctx;void(*global_user_ctx_free_fn)(void*);
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
static int startCalls=0,stopCalls=0,registrations=0,workerStarts=0,workerStops=0,releases=0;
static bool failServer=false,failWorker=false,failStop=false;
static int failRegistration=0;
static esp_err_t httpd_start(httpd_handle_t* s,const httpd_config_t*) {
 ++startCalls; if(failServer){*s=nullptr;return ESP_FAIL;}
 *s=reinterpret_cast<void*>(1);return ESP_OK;
}
static esp_err_t httpd_stop(httpd_handle_t) {++stopCalls;return failStop?ESP_FAIL:ESP_OK;}
static esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*) {
 ++registrations;return registrations==failRegistration?ESP_FAIL:ESP_OK;
}
static esp_err_t setTcpNoDelay(httpd_handle_t,int){return ESP_OK;}
struct WebWsTransport {
 static constexpr int MAX_CLIENTS=7;
 bool start(httpd_handle_t){++workerStarts;return !failWorker;}
 void stop(){++workerStops;}
 void release(){++releases;}
};
class WebUI {public:
 CN105Controller* _ctrl=nullptr;httpd_handle_t _server=nullptr;
 WebWsTransport _ws;std::atomic<bool> _wsReady{false};
 RETURN_TYPE begin(CN105Controller*);
 void applyCaptivePortalHandler(){}
 static void closeClient(httpd_handle_t,int){}
 HANDLERS
};
'''
handlers = sorted(set(re.findall(r'\.handler\s*=\s*(\w+)', begin[0])) | {'wsPreHandshake'})
prefix = prefix.replace('RETURN_TYPE', begin[1]).replace('HANDLERS', '\n'.join(
    f'static esp_err_t {name}(httpd_req_t*){{return ESP_OK;}}' for name in handlers))
server_test = prefix + begin[0] + r'''
int main(int argc,char** argv) {
 assert(argc==2);WebUI ui;CN105Controller c;
 if(!strcmp(argv[1],"server")) {
  failServer=true;ui.begin(&c);assert(!ui._wsReady.load());
  failServer=false;ui.begin(&c);assert(ui._wsReady.load());assert(startCalls==2);
 } else if(!strcmp(argv[1],"worker")) {
  failWorker=true;ui.begin(&c);assert(!ui._wsReady.load());assert(!ui._server);
  failWorker=false;ui.begin(&c);assert(ui._wsReady.load());assert(stopCalls==1);
 } else if(!strcmp(argv[1],"stop")) {
  failWorker=true;failStop=true;ui.begin(&c);assert(!ui._wsReady.load());assert(ui._server);
  failWorker=false;failStop=false;ui.begin(&c);assert(ui._wsReady.load());assert(startCalls==2);
 } else if(!strcmp(argv[1],"registration")) {
  failRegistration=3;ui.begin(&c);assert(!ui._wsReady.load());assert(!ui._server);
  assert(workerStops==1);assert(releases==1);
  failRegistration=0;ui.begin(&c);assert(ui._wsReady.load());
 } else {assert(false);}
 int starts=startCalls,workers=workerStarts;ui.begin(&c);
 assert(startCalls==starts&&workerStarts==workers);
 puts("startup resources recover and successful initialization remains one-shot");
}
'''
main_test = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
static uint32_t now=0;
static uint32_t uptime_ms(){return now;}
namespace WifiManager {static bool connected=false;bool isConnected(){return connected;}}
struct Recovery {bool active=false;bool isAPActive(){return active;}} wifiRecovery;
struct WebUI {int attempts=0;bool succeed=false;bool begin(void*){++attempts;return succeed;}}webUI;
namespace BleSensor {static int begins=0;void begin(){++begins;}}
static bool webUIStarted=false,networkReady=false,safeMode=false;
static uint32_t webUIStartTime=0;
static int cn105;
#define BLE_ENABLE 1
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
static void tick(){
''' + startup + r'''
}
int main() {
 tick();assert(webUI.attempts==0);
 WifiManager::connected=true;now=0xffffff00U;tick();
 // The network is up after the first attempt whether or not HTTPD started:
 // sensors and OTA validation must not depend on the web server.
 assert(webUI.attempts==1);assert(!webUIStarted);assert(networkReady);
 assert(BleSensor::begins==1);assert(webUIStartTime==now);
 for(int i=0;i<10;++i){now+=100;tick();}
 assert(webUI.attempts==1); // backoff survives clock wrap
 const uint32_t firstAttempt=webUIStartTime;
 now+=10000;webUI.succeed=true;tick();
 assert(webUI.attempts==2);assert(webUIStarted);assert(webUIStartTime==firstAttempt);
 assert(BleSensor::begins==1);
 for(int i=0;i<10;++i){now+=10000;tick();}
 assert(webUI.attempts==2);assert(BleSensor::begins==1);
 puts("main retries failed startup with backoff and starts sensors on the first attempt");
}
'''

with tempfile.TemporaryDirectory(prefix='cn105-web-startup-') as folder:
    build = Path(folder)
    failed = False
    for name, body, cases in [('server', server_test, ['server', 'worker', 'stop', 'registration']),
                              ('main', main_test, [''])]:
        source = build / (name + '.cpp')
        source.write_text(body)
        exe = build / name
        subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', str(source), '-o', str(exe)], check=True)
        for case in cases:
            result = subprocess.run([str(exe), *([case] if case else [])])
            print(f'web_startup/{name}/{case}: {"PASS" if result.returncode == 0 else "FAIL"}', flush=True)
            failed |= result.returncode != 0
    raise SystemExit(1 if failed else 0)
