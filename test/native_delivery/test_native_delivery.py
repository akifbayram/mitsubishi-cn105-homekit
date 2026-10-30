#!/usr/bin/env python3
"""Compile production control paths with host clock/UART/radio boundaries.

ESP-IDF headers are removed only for the host build. The adapter and BLE
functions are extracted intact: assertions execute their compiled bodies.
Each invocation owns its build directory, including concurrent repo runs.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import shlex

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"


def clean(name):
    return re.sub(r"^#(?:include[^\n]*|pragma once)\n", "", (MAIN / name).read_text(), flags=re.M)


def block(src, start):
    opening = src.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        if src[end] == "{":
            depth += 1
        elif src[end] == "}":
            depth -= 1
        end += 1
    return src[start:end]


def function(src, name):
    match = re.search(r"^(?:(?:static|inline) )*(?:bool|void|uint8_t|uint16_t|uint32_t|int|float|AutoTarget) "
                      + re.escape(name) + r"\(", src, re.M)
    assert match, name
    return block(src, match.start())


HOST = r'''
#include <algorithm>
#include <climits>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <deque>
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>
#include <functional>
#include "sl2_proto.h"
#include "cn105_baud.h"
#include "uart_interface.h"
// Tests inspect mailbox/protocol internals; keep private below system includes.
#define private public
using uart_port_t=int;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr,"line %d: %s\n",__LINE__,#cond); exit(1); } } while(0)
static int currentLogLevel=0;
static constexpr int LOG_LEVEL_DEBUG=3;
static uint32_t now_ms=1000;
static uint32_t uptime_ms() {return now_ms;}
static const char* modeToLogStr(uint8_t){return "";}
static const char* fanToLogStr(uint8_t){return "";}
static const char* vaneToLogStr(uint8_t){return "";}
static const char* wideVaneToLogStr(uint8_t){return "";}
static const char* subModeToLogStr(uint8_t){return "";}
static const char* stageToLogStr(uint8_t){return "";}
static const char* autoSubModeToLogStr(uint8_t){return "";}
'''
HOST += "\n" + function((MAIN / "esp_utils.h").read_text(), "fnv1a32") + "\n"
CN105 = HOST + clean("setpoint_range.h") + clean("cn105_protocol.h") + clean("cn105_protocol.cpp")
PUMP = r'''
class Pump : public UartInterface {
public:
 std::deque<uint8_t> rx;
 int infos=0,connects=0,sets=0,remoteSets=0,remoteClears=0,dropTemp=0,identifies=0;
 std::vector<std::vector<uint8_t>> sent;
 std::vector<uint8_t> identifyReply;
 bool responding=true;
 uint32_t readCostMs=0;
 uint8_t mode=CN105_MODE_AUTO;bool power=true;
 uint8_t target=172,roomRaw=170;
 bool legacy=false;int legacyC=22;
 int read(uint8_t* b,size_t n) override {size_t i=0;while(i<n&&!rx.empty()){b[i++]=rx.front();rx.pop_front();}if(i)now_ms+=readCostMs;return i;}
 size_t available() override {return rx.size();}
 void flush() override {rx.clear();}
 void packet(uint8_t type,uint8_t subtype) {
  uint8_t p[22]={};CN105Controller::buildHeader(p,type,16);p[5]=subtype;
  if(subtype==CN105_INFO_SETTINGS){p[8]=power?1:0;p[9]=mode;if(legacy)p[10]=31-legacyC;else p[16]=target;}
  if(subtype==CN105_INFO_ROOMTEMP)p[11]=roomRaw;
  if(subtype==CN105_INFO_ERRORCODE)p[9]=0x80;
  p[21]=CN105Controller::calcChecksum(p,21);
  for(uint8_t b:p)rx.push_back(b);
 }
 void write(const uint8_t* b,size_t n) override {
  sent.emplace_back(b,b+n);
  if(b[1]==0x5b){++identifies;if(responding)for(uint8_t v:identifyReply)rx.push_back(v);}
  if(b[1]==0x5a){++connects;if(responding)packet(CN105_PKT_CONNECT_OK,0);}
  if(b[1]==CN105_PKT_INFO_REQ){++infos;if(responding)packet(CN105_PKT_INFO_RESP,b[5]);}
  if(b[1]==CN105_PKT_SET){
   ++sets;
   if(b[5]==1&&(b[6]&CN105_FLAG_TEMP)&&dropTemp>0){--dropTemp;return;}   // lost: not applied, not ACKed
   if(b[5]==0x07){if(b[6]){++remoteSets;roomRaw=b[8];}else ++remoteClears;}
   if(b[5]==1){if(b[6]&CN105_FLAG_POWER)power=b[8];if(b[6]&CN105_FLAG_MODE)mode=b[9];if(b[6]&CN105_FLAG_TEMP){if(b[19])target=b[19];else legacyC=31-b[10];}}
   if(responding)packet(CN105_PKT_SET_ACK,0);
  }
 }
};
static void ticks(CN105Controller& c,int n=12,uint32_t step=10){for(int i=0;i<n;i++){c.loop();now_ms+=step;}}
'''

# The real Average blend shares this translation unit with the Link feed, so
# its file statics are renamed apart from link_sensor.cpp's same-named ones.
room_avg = re.sub(r"\b(TAG|s_mux|s_status|s_feed|s_pendingClear)\b", r"avg_\1",
                  clean("room_avg.cpp"))
LINK = HOST + r'''
#include "room_feed.h"
#include "link_units.h"
#include "sl2_bond.h"
#include "sl2_link.h"
constexpr int ROOM_MAX_BLE_SENSORS=4,ROOM_MEMBER_INTERNAL=0,ROOM_MEMBER_LINK=1,ROOM_MEMBER_BLE0=2,
 ROOM_MEMBER_COUNT=ROOM_MEMBER_BLE0+ROOM_MAX_BLE_SENSORS;
struct DeviceSettings{uint64_t roomSourceId=0;int roomMode=0,roomSingle=1,roomStaleTimeoutS=600;uint8_t roomMembers=0;};
struct Settings{DeviceSettings s;DeviceSettings& get(){return s;}void save(){}}settings;
static int room_offset_dc(const DeviceSettings&,int){return 0;}
static float room_apply_offset(const DeviceSettings&,int,float t){return t;}
class CN105Controller{public:float temp=NAN;int sends=0,clears=0;bool connected=true;
 bool isConnected(){return connected;}void sendRemoteTemperature(float t){temp=t;++sends;}
 void clearRemoteTemperature(){temp=NAN;++clears;}};
''' + clean("link_sensor.h") + clean("room_avg.h") + clean("link_sensor.cpp") + room_avg

ble = (MAIN / "ble_sensor.cpp").read_text()
BLE = HOST + r'''
constexpr int ROOM_MAX_BLE_SENSORS=4,ROOM_MEMBER_LINK=1,ROOM_MEMBER_BLE0=2;
constexpr uint32_t BLE_RESEND_MIN_MS=5000,BLE_KEEPALIVE_MS=20000;
struct DeviceSettings{int roomMode=0,roomSingle=ROOM_MEMBER_BLE0,roomStaleTimeoutS=600;};
struct Settings{DeviceSettings s;DeviceSettings& get(){return s;}void save(){}}settings;
static float room_apply_offset(const DeviceSettings&,int,float t){return t;}
namespace LinkSensor{static bool active=false;bool isActive(){return active;}}
namespace RoomAvg{static bool feeding=false;bool isFeeding(){return feeding;}}
class CN105Controller{public:
 float temp=NAN;int sends=0,clears=0;bool connected=true;
 bool isConnected(){return connected;}
 void sendRemoteTemperature(float t){temp=t;++sends;}
 void clearRemoteTemperature(){temp=NAN;++clears;}
 static float quantizeRemoteTemp(float t);
};
namespace BleSensor{int feedSlot();void loop(CN105Controller&);}
static bool initNimble(){return true;}
static void deinitNimble(){}
static void stopScan(){}
static void startScan(){}
static bool wantScan(){return true;}
static std::atomic<bool> s_discoveryMode{false};
''' + ble[ble.index("struct SlotState"):ble.index("// ── Discovery state")]
BLE += "\n" + function((MAIN / "cn105_protocol.cpp").read_text(), "CN105Controller::quantizeRemoteTemp")
ble_functions = ["BleSensor::feedSlot", "freshAt"]
if re.search(r"^static void drainPendingClear\(", ble, re.M):
    ble_functions.append("drainPendingClear")
ble_functions.append("BleSensor::loop")
BLE += "\n" + "\n".join(function(ble, n) for n in ble_functions)

adapter = (MAIN / "espnow_link.cpp").read_text()

# Exercise the real catalog and selection adapter too. Only the bonded-dial
# store is a fixture; its two MACs stand for already-authenticated peers.
LINK += r'''
#define ROOM_CATALOG_MAX (2 + ROOM_MAX_BLE_SENSORS + 1 + SL2_MAX_DIALS)
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
struct BondedDials {int n=2;uint8_t mac[SL2_MAX_DIALS][6]={{1,2,3,4,5,6},{6,5,4,3,2,1}};};
static BondedDials s_link;
static bool s_dialHasSensor[SL2_MAX_DIALS]={true,true};
static int sl2_link_dial_count(const BondedDials* l) {return l->n;}
static bool sl2_link_dial_mac(const BondedDials* l,int index,uint8_t out[6]) {
 if(index<0||index>=l->n)return false;
 memcpy(out,l->mac[index],6);return true;
}
static bool sl2_link_dial_view(const BondedDials* link,int index,sl2_dial_view_t* out) {
 if(!out||index<0||index>=link->n)return false;
 *out={};sl2_link_dial_mac(link,index,out->mac);out->has_sensor=s_dialHasSensor[index];return true;
}
static bool sl2_link_forget_dial(BondedDials* l,const uint8_t mac[6]) {
 for(int i=0;i<l->n;i++)if(!memcmp(l->mac[i],mac,6)){
  memmove(l->mac[i],l->mac[i+1],(size_t)(l->n-i-1)*6);l->n--;return true;}
 return false;
}
'''
LINK += "\n" + "\n".join(function(adapter, n) for n in
    ["copy_name", "room_catalog_build", "room_catalog_hash", "room_catalog_revision", "h_room_source_set",
     "room_source_reconcile_catalog"])
# The web forgetDial mailbox and its main-task drain, statics included.
LINK += "\n" + adapter[adapter.index("/* Per-dial forget mailbox"):adapter.index("static void drain_forget_dials")]
LINK += "\n" + function(adapter, "drain_forget_dials")
thermo_name = "homekit_thermostat.cpp" if (MAIN / "homekit_thermostat.cpp").exists() else "matter_thermostat.cpp"
thermo = (MAIN / thermo_name).read_text()
strings = (MAIN / "cn105_strings.h").read_text()
AUTO = CN105 + r'''
#include "mode_caps.h"
#include "sl2_link.h"
#include "solar.h"
struct DeviceSettings{
 uint8_t modeMask=MODE_CAP_ALL,vaneConfig=2,nightCeilPct=20;
 float heatingThreshold=20,coolingThreshold=25,latitude=NAN,longitude=NAN;
 int16_t nightDuskOffMin=0,nightDawnOffMin=0;
 bool useFahrenheit=false,autoOwnerUnit=false,autoSingleSetpoint=false;
 char deviceName[32]="Test";
};
struct SetpointSettings {
 SetpointRange::Preferences preferences{};SetpointRange::Band band{40,50};bool autoOwnerUnit=false;
 SetpointRange::Encoding lastEncoding=SetpointRange::Encoding::Unknown;
};
struct SettingsStore {
 DeviceSettings s;int saves=0;SetpointSettings policy;SetpointRange::Detection detected{};bool singleBoot=false;
 DeviceSettings& get(){return s;}void save(){++saves;}bool autoSingleBoot()const{return singleBoot;}
 bool saveAutoSingle(bool v){s.autoSingleSetpoint=v;++saves;return true;}
 SetpointSettings setpointSettings()const{auto p=policy;p.band={(int16_t)std::lround(s.heatingThreshold*2),(int16_t)std::lround(s.coolingThreshold*2)};p.autoOwnerUnit=s.autoOwnerUnit;return p;}
 bool setpointSettingsAvailable()const{return true;}
 bool saveSetpointSettings(const SetpointSettings& p){if(p.band.heat2!=s.heatingThreshold*2||p.band.cool2!=s.coolingThreshold*2||p.autoOwnerUnit!=s.autoOwnerUnit)++saves;policy=p;s.heatingThreshold=p.band.heat2/2.f;s.coolingThreshold=p.band.cool2/2.f;s.autoOwnerUnit=p.autoOwnerUnit;return true;}
 SetpointRange::Detection detectedRanges()const{return detected;}
 bool saveDetectedRanges(const SetpointRange::Detection& p){detected=p;return true;}
 static bool isFactoryResetting(){return false;}
}settings;
namespace WifiManager{bool isConnected(){return true;}bool hasCredentials(){return true;}}
struct Recovery{bool isAPActive(){return false;}bool wifiChangeFailed(){return false;}}wifiRecovery;
static uint32_t time_sync_epoch(){return 0;}
static CN105Controller* s_ctrl;
''' + function(strings, "modeToCapBit") + "\n" + function(strings, "mode_mask_allows")
# A second scheduling boundary models a CN105 response after the coordinator
# has made its steering decision but before the driver's conditional staging.
AUTO += "\nstatic std::function<void()> beforeAutoQueue;\n"
auto_control = clean("setpoint_control.cpp")
queue_call = "controller->queueAutoTarget(target, resolvedState, external);"
assert auto_control.count(queue_call) == 1
auto_control = auto_control.replace(queue_call,
    "if (beforeAutoQueue) { auto hook = beforeAutoQueue; beforeAutoQueue = nullptr; hook(); }\n" + queue_call)
AUTO += "\n" + clean("setpoint_control.h") + auto_control
# The real ownership module: the dial adapter below and the engine both call it.
AUTO += "\n" + clean("auto_band.h") + clean("auto_band.cpp").replace('static const char *TAG', 'static const char *auto_band_TAG')
AUTO += "\n" + adapter[adapter.index("static uint8_t mode_to_sl2"):adapter.index("/* A bonded dial reported")]
AUTO += "\n" + function(adapter, "h_get_caps")
# Run the complete thermostat tick with absent HAP handles, as on an offline
# boot. Any accidental SDK access before HomeKit exists fails the test.
AUTO += r'''
union hap_val_t {uint32_t u;float f;bool b;};
struct hap_char_t {};
static const hap_val_t* hap_char_get_val(hap_char_t*) {CHECK(false);return nullptr;}
static void hap_char_update_val(hap_char_t*,const hap_val_t*) {CHECK(false);}
namespace RoomAvg {
 struct Status {bool feeding;float effective;};
 static Status status(){return {false,NAN};}
}
'''
AUTO += thermo[thermo.index("static hap_char_t"):thermo.index("// ── External controller reference")]
# Deterministic scheduling boundary: run a newer operation immediately before
# the production thermostat delegates steering. No production behavior is mocked.
AUTO += "\nstatic std::function<void()> beforeAutoSteering;\nstatic void homekit_reconcile_temperature_metadata(CN105Controller&) {}\n"
steering = function(thermo, "homekit_sync_thermostat")
steering_call = "SetpointControl::steerAuto();"
assert steering.count(steering_call) == 1
steering = steering.replace(steering_call,
    "if (beforeAutoSteering) { auto hook = beforeAutoSteering; beforeAutoSteering = nullptr; hook(); }\n" + steering_call)
AUTO += "\n" + "\n".join(function(thermo, n) for n in ["cn105ToHKTargetState", "deriveCurrentState"]) + "\n" + steering
AUTO += "\nstatic void run_auto_sync(CN105Controller& cn105){homekit_sync_thermostat(cn105);}\n"
# Execute the production main-loop dispatch, not a duplicate of its gating.
app = (MAIN / "main.cpp").read_text()
AUTO += r'''
static bool safeMode=false,homekitStarted=false;
static bool homekit_is_started(){return homekitStarted;}
static void homekit_sync_fan(CN105Controller&){CHECK(homekitStarted);}
static void homekit_sync_switches(CN105Controller&){CHECK(homekitStarted);}
static void run_main_control_tick(CN105Controller& cn105){
'''
AUTO += app[app.index("        // ── Correct manual-range violations"):app.index("        // ── ESP-NOW remote")]
AUTO += "\n}\n" + PUMP

# H2 compiles SettingsStore and HomeKit setup intact against NVS/HAP boundaries.
# The reset stub queues work: storage completion is delivered separately.
HOMEKIT = HOST + (HERE / "homekit_stubs.inc").read_text()
HOMEKIT += "\n" + clean("setpoint_range.h")
HOMEKIT += "\n" + clean("settings.h") + "\n" + clean("settings.cpp").replace('static const char *TAG', 'static const char *settings_TAG')
HOMEKIT += "\n" + clean("homekit_setup.h") + "\n" + clean("homekit_setup.cpp")
HOMEKIT += '\n#include "web_security.h"\n'

# Factory reset uses the same NVS stub and real settings/HomeKit paths as H2.
# Callbacks model main-loop writes at erase entry and during identity restore.
FACTORY = HOMEKIT + r'''
#include "sl2_link.h"
#include "event_ring.h"
static void sodium_memzero(void* b,size_t n){memset(b,0,n);}
''' + re.search(r'^static const char \*SL2_NVS_NS = "[^"]+";', adapter, re.M).group(0)
FACTORY += "\n" + "\n".join(function(adapter, n) for n in ["p_kv_get", "p_kv_set", "espnow_factory_erase_nvs"])
FACTORY += r'''
namespace Events {
static nvs_handle_t s_nvs=0;
static constexpr const char* KEY_RING="ring";
static EventBlob s_blob{};
''' + function((MAIN / "event_log.cpp").read_text(), "persistLocked") + "\n}\n"
wifi = (MAIN / "wifi_manager.cpp").read_text()
FACTORY += "\nnamespace WifiManager {bool saveCredentials(const char*,const char*);void eraseCredentials();}\n"
FACTORY += wifi[wifi.index("static constexpr const char* NVS_NAMESPACE"):wifi.index("// ── Event group bits")]
FACTORY += "\nstatic int8_t s_haveCreds=-1;\n" + "\n".join(function(wifi, n) for n in
    ["WifiManager::saveCredentials", "WifiManager::eraseCredentials"])

# Compile the shipped HTTP and WS serialization expressions, not copies of
# their security policy. Only their buffer append boundary is a host stub.
HOMEKIT += r'''
#include <cstdarg>
static void stateAppend(char*buf,size_t cap,int*n,int*want,const char*fmt,...){
 va_list ap;va_start(ap,fmt);int wrote=vsnprintf(buf+*n,cap-*n,fmt,ap);va_end(ap);*n+=wrote;*want+=wrote;}
static std::string httpSetup(){
 const char*escSSID="",*escName="Test",*trial="idle";bool connected=false,joined=false;
'''
http = (MAIN / "web_server.cpp").read_text()
start = http.index('    char buf[340];')
HOMEKIT += http[start:http.index('    httpd_resp_set_type', start)] + "\nreturn buf;}\n"
HOMEKIT += r'''
static std::string wsSetup(){
 const auto&cfg=settings.get();char buf[2048]="{";size_t bufSz=sizeof(buf);int n=1,want=1;
'''
ws = (MAIN / "web_ws.cpp").read_text()
start = ws.index('    bool hkReady = homekit_is_started();')
HOMEKIT += ws[start:ws.index('    section();', start)] + "\nreturn buf;}\n"

CONTROL = CN105 + (HERE / "homekit_stubs.inc").read_text().replace("constexpr float CN105_TEMP_MIN=16.0f, CN105_TEMP_MAX=30.5f;", "")
CONTROL += "\n" + clean("settings.h") + clean("settings.cpp").replace('static const char *TAG', 'static const char *settings_TAG')
if (MAIN / "setpoint_control.h").exists():
    CONTROL += "\n" + function(strings, "modeToCapBit") + "\n" + function(strings, "mode_mask_allows")
    CONTROL += "\n" + clean("setpoint_control.h") + clean("setpoint_control.cpp")
CONTROL += r'''
union hap_val_t {uint32_t u;float f;bool b;};
struct hap_char_t {const char* uuid;hap_val_t value{};int updates=0;};
struct hap_write_data_t {hap_char_t* hc;hap_val_t val;int* status;};
static const char* hap_char_get_type_uuid(hap_char_t* c){return c->uuid;}
static void hap_char_update_val(hap_char_t* c,const hap_val_t* v){c->value=*v;++c->updates;}
constexpr int HAP_FAIL=-1,HAP_STATUS_SUCCESS=0,HAP_STATUS_VAL_INVALID=1,HAP_STATUS_RES_BUSY=2,HAP_STATUS_RES_ABSENT=3;
static CN105Controller* g_homekitCtrl;
static hap_char_t *s_heatThresh=nullptr,*s_coolThresh=nullptr;
// The policy-only lane omits HAP service creation. The full ranges lane below
// executes this metadata boundary with production code.
static void install_temperature_metadata(CN105Controller*) {}
'''
for uuid in ["CONFIGURED_NAME", "TEMPERATURE_DISPLAY_UNITS", "TARGET_HEATING_COOLING_STATE", "TARGET_TEMPERATURE", "HEATING_THRESHOLD_TEMPERATURE", "COOLING_THRESHOLD_TEMPERATURE", "ACTIVE", "ROTATION_SPEED", "ON"]:
    CONTROL += f'\n#define HAP_CHAR_UUID_{uuid} "{uuid}"\n'
CONTROL += "\n" + function(thermo, "hkTargetStateToCN105Mode") + "\n" + function(thermo, "thermostat_write_cb")
fan = (MAIN / "homekit_fan.cpp").read_text()
switches = (MAIN / "homekit_switches.cpp").read_text()
CONTROL += "\n" + "\n".join(function(fan, name) for name in ["percentToCN105Fan", "fan_write_cb", "fan_auto_write_cb"])
if "static bool reject_masked_on" in switches:
    CONTROL += "\n" + function(switches, "reject_masked_on")
CONTROL += "\n" + "\n".join(function(switches, name) for name in ["fan_mode_write_cb", "dry_mode_write_cb"])
CONTROL += '\n#include <cstdarg>\n#include <cctype>\n#include <memory>\n#include <cJSON.h>\n' + clean("json_utils.h")
CONTROL += ws[ws.index('// ── range-config:start'):ws.index('// ── range-config:end')]
CONTROL += "\n" + "\n".join(function(strings, name) for name in ["strToMode", "strToFan", "strToVane", "strToWideVane"])
CONTROL += r'''
static bool webError=false;
static int httpd_req_to_sockfd(void*){return 0;}
static void sendWsText(int,const char*){webError=true;}
static void applyWeb(const char*msg){void* req=nullptr;webError=false;auto parsed=webParseObject(msg);if(!parsed){webError=true;return;}
'''
CONTROL += ws[ws.index("        // Decode the complete climate"):ws.index("        // Night gate:")] + "\n}\n"
CONTROL += PUMP

# Exercise CAPS construction, the complete adapter loop refresh gate, and the
# production core's sequence persistence against the shared NVS/UART boundaries.
LINK_RANGES = CONTROL[:CONTROL.index("union hap_val_t")]
LINK_RANGES += r'''
#include "sl2_link.h"
#include "sl2_rxq.h"
#include "solar.h"
class EspnowLink {public:void loop();};
static CN105Controller* s_ctrl;
static sl2_link_t s_link{};
static sl2_rxq_t s_rxq{};
static bool s_started=true;
static bool s_reqForgetAll=false,s_reqPairStart=false,s_reqPairCancel=false,s_reqForgetRestart=false;
#define PAIR_WINDOW_S 60
#define PIN_LED_DATA -1
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int) {CHECK(false);}
static void esp_restart() {CHECK(false);}
static void room_source_reconcile_catalog(uint32_t) {}
static void room_source_drop_link() {}
static void drain_forget_dials() {}
static void snapshot_status() {}
namespace LinkSensor {static void forget(const uint8_t*) {}}
namespace WifiManager {static bool isConnected(){return true;}static bool hasCredentials(){return true;}}
struct Recovery {bool isAPActive(){return false;}bool wifiChangeFailed(){return false;}} wifiRecovery;
static uint32_t time_sync_epoch(){return 0;}
void sl2_link_loop(sl2_link_t*) {}
void sl2_link_pair_start(sl2_link_t*,uint32_t) {CHECK(false);}
void sl2_link_pair_cancel(sl2_link_t*) {CHECK(false);}
bool sl2_link_forget_all(sl2_link_t*) {CHECK(false);return false;}
void sl2_link_on_recv(sl2_link_t*,const uint8_t*,const uint8_t*,const uint8_t*,int) {CHECK(false);}
'''
LINK_RANGES += "\n" + clean("auto_band.h")
LINK_RANGES += adapter[adapter.index("// caps-change watch:"):adapter.index("/* Serin root Ed25519")]
LINK_RANGES += "\n" + adapter[adapter.index("static uint8_t mode_to_sl2"):adapter.index("/* A bonded dial reported")]
LINK_RANGES += "\n" + function(adapter, "h_get_caps")
LINK_RANGES += '\nstatic const char* SL2_NVS_NS="serinlink";\n'
LINK_RANGES += "\n" + "\n".join(function(adapter, n) for n in ["p_kv_get", "p_kv_set"])
core = (MAIN / "sl2_link.c").read_text()
LINK_RANGES += "\n" + "\n".join(function(core, n) for n in ["persist_caps_seq", "mark_all_state_pending", "sl2_link_caps_changed"])
LINK_RANGES += "\n" + adapter[adapter.index('static const char *SL2_KV_CAPS_FP'):adapter.index("/* ── EspnowLink facade")]
LINK_RANGES += "\n" + function(adapter, "EspnowLink::loop") + PUMP

# The real checked web configuration and serializer, backed by real NVS/control.
WEB = CONTROL[:CONTROL.index("union hap_val_t")] + PUMP
WEB += '\n#include <memory>\n#include <cJSON.h>\n'
WEB += ws[ws.index('// ── range-config:start'):ws.index('// ── range-config:end')]

WEB += '\n#include <cstdarg>\n' + clean("json_utils.h")
WEB += "\n" + function(ws, "stateAppend")
WEB += ws[ws.index('// ── range-state:start'):ws.index('// ── range-state:end')]
WEB += r'''
static std::function<void()> afterWebRangeSection;
static std::string webRangeState(size_t cap) {
 char storage[8192]="{\"type\":\"state\"";char*buf=storage;
 size_t bufSz=cap,allocSz=sizeof(storage);int n=strlen(storage);
'''
WEB += ws[ws.index('    int kept = n;'):ws.index('    // Keep the complete profile',ws.index('    int kept = n;'))]
WEB += "\nwebAppendRangeState(buf,bufSz,&n,&want);section();\n"
# A scheduling boundary after capture exposes any later mutable policy reads.
WEB += "\nif(afterWebRangeSection){auto hook=afterWebRangeSection;afterWebRangeSection=nullptr;hook();}\n"
if '    // Dual setpoint thresholds.' in ws:
    WEB += "\nconst auto& cfg=settings.get();\n"
    WEB += ws[ws.index('    // Dual setpoint thresholds.'):ws.index('    // Night gate location.')]

WEB += ws[ws.index('    n = kept;',ws.index('    // Close on the last section')):ws.index('    if (dropped) {',ws.index('    n = kept;',ws.index('    // Close on the last section')))]
WEB += "\nreturn buf;}\n"

# Full production thermostat + setup, with only the HAP/HTTP/NVS boundaries replaced.
RANGES = CONTROL[:CONTROL.index("union hap_val_t")]
for uuid in ["CURRENT_HEATING_COOLING_STATE", "CURRENT_TEMPERATURE", "TEMPERATURE_DISPLAY_UNITS", "TARGET_HEATING_COOLING_STATE", "TARGET_TEMPERATURE", "HEATING_THRESHOLD_TEMPERATURE", "COOLING_THRESHOLD_TEMPERATURE"]:
    RANGES += f'\n#define HAP_CHAR_UUID_{uuid} "{uuid}"\n'
RANGES += "\n" + (HERE / "hap_ranges_stubs.inc").read_text()
RANGES += "\n" + clean("homekit_setup.h") + clean("homekit_setup.cpp").replace('static const char *TAG', 'static const char *setup_TAG')
RANGES += "\n" + clean("homekit_thermostat.cpp").replace('static const char *TAG', 'static const char *thermo_TAG') + PUMP

SDK = MAIN.parent / "components/esp-homekit-sdk/components/homekit/esp_hap_core/src"
RANGES += "\n" + (HERE / "hap_sdk_read_stubs.inc").read_text()
RANGES += "\n" + function((SDK / "esp_hap_serv.c").read_text(), "hap_serv_def_bulk_read_cb")
sdk_ip = (SDK / "esp_hap_ip_services.c").read_text()
RANGES += "\n" + function(sdk_ip, "hap_prepare_char_db")
RANGES += "\n" + function(sdk_ip, "hap_prepare_serv_db")
# Extract the actual GET /characteristics response gates, including error status.
# Request parsing/session authentication are outside this read-status boundary.
start = sdk_ip.index('        if (hc->permission & HAP_CHAR_PERM_SPECIAL_READ)', sdk_ip.index('/* Loop through the characteristics and include their data'))
end = sdk_ip.index('\n', sdk_ip.index('}', sdk_ip.index('json_gen_obj_set_int(&jstr, "status"', start)))
RANGES += r'''
static json_gen_str_t sdk_characteristic(hap_char_t*hc){
 hap_status_t status=HAP_STATUS_SUCCESS;hap_read_data_t read_arr[]={{hc,&status}};
 bool read_err=hap_serv_def_bulk_read_cb(read_arr,1,nullptr,nullptr)!=HAP_SUCCESS;
 bool include_status=false;int i=0;json_gen_str_t jstr;jstr.iid=hc->iid;
''' + sdk_ip[start:end] + "\nreturn jstr;}\n"

SOURCES = {"link_ranges": LINK_RANGES,"web_ranges": WEB,"homekit_ranges": RANGES,"setpoint_control": CONTROL, "cn105_polling": CN105 + PUMP, "cn105_float_setters": CN105 + PUMP,
           "cn105_capabilities": CN105 + PUMP,
           "link_source": LINK, "ble_handoff": BLE, "auto_band": AUTO,
           "factory_reset": FACTORY, "homekit_pairing": HOMEKIT, "setpoint_settings": HOMEKIT}
failed = False
with tempfile.TemporaryDirectory(prefix="cn105-native-delivery-") as directory:
    build = Path(directory)
    for name, source in SOURCES.items():
        if len(sys.argv) > 1 and name not in sys.argv[1:]:
            continue
        path = build / (name + ".cpp")
        path.write_text(source + "\n" + (HERE / ("test_" + name + ".cpp")).read_text())
        exe = build / name
        subprocess.run(["g++", "-std=c++17", "-DUNIT_TEST", "-I" + str(MAIN), str(path), *([str(MAIN / "solar.cpp")] if name in ("auto_band", "link_ranges") else []),
                        *([str(MAIN / "setpoint_range.cpp")] if name.startswith("cn105_") or name in ("link_ranges", "auto_band", "homekit_pairing", "factory_reset", "setpoint_settings", "setpoint_control", "homekit_ranges", "web_ranges") else []), *(shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcjson"], text=True)) if name in ("web_ranges", "setpoint_control") else []), "-o", str(exe)], check=True)
        cases = re.findall(r'CASE\("([^"]+)"\)', (HERE / ("test_" + name + ".cpp")).read_text())
        for case in cases:
            for storage_mode in (["staged", "immediate"] if name == "setpoint_settings" else [None]):
                result = subprocess.run([str(exe), case] + ([storage_mode] if storage_mode else []))
                label = f"/{storage_mode}" if storage_mode else ""
                print(f"{name}/{case}{label}: {'PASS' if result.returncode == 0 else 'FAIL'}", flush=True)
                failed |= result.returncode != 0
sys.exit(1 if failed else 0)
