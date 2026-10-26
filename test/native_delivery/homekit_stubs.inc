#include <map>
#include <functional>
#include <string>
#include "branding.h"
#include "mode_caps.h"
enum LogLevel : uint8_t {LOG_LEVEL_ERROR, LOG_LEVEL_WARN, LOG_LEVEL_INFO, LOG_LEVEL_DEBUG_TEST};
constexpr float CN105_TEMP_MIN=16.0f, CN105_TEMP_MAX=30.5f;
using esp_err_t=int;using nvs_handle_t=uint32_t;
constexpr int ESP_OK=0,ESP_FAIL=-1,ESP_ERR_NVS_NOT_FOUND=1;
constexpr int NVS_READONLY=0,NVS_READWRITE=1,ESP_ERR_NVS_NOT_INITIALIZED=2;
enum nvs_type_t {NVS_TYPE_U8,NVS_TYPE_U16,NVS_TYPE_U32,NVS_TYPE_STR,NVS_TYPE_BLOB};
struct FakeSettingsNvs {
 std::map<std::string,std::map<std::string,std::vector<uint8_t>>> committed,pending;
 std::vector<std::string> handles;
 bool init=true;int commits=0,writes=0,erases=0,inits=0,deinits=0;
 std::function<void()> onErase,onInit,onWrite,onCommit;
 std::string failWriteKey,writeBeforeErrorKey;int writeBeforeErrorCount=0;
 bool failCommit=false,immediateWrites=false;
} hkNvs;
static const char* esp_err_to_name(int){return "host error";}
static int nvs_open(const char* ns,int,nvs_handle_t* h){if(!hkNvs.init)return ESP_ERR_NVS_NOT_INITIALIZED;hkNvs.handles.push_back(ns);*h=hkNvs.handles.size();return ESP_OK;}
static void nvs_close(nvs_handle_t h){hkNvs.pending.erase(hkNvs.handles[h-1]);}
static int nvs_get_blob(nvs_handle_t h,const char* key,void* out,size_t* len){
 auto& m=hkNvs.committed[hkNvs.handles[h-1]];auto i=m.find(key);if(i==m.end())return ESP_ERR_NVS_NOT_FOUND;
 if(!out){*len=i->second.size();return ESP_OK;}
 if(*len<i->second.size())return ESP_FAIL;memcpy(out,i->second.data(),i->second.size());*len=i->second.size();return ESP_OK;}
static int nvs_set_blob(nvs_handle_t h,const char* key,const void* in,size_t len){
 ++hkNvs.writes;if(hkNvs.onWrite){auto callback=hkNvs.onWrite;callback();}
 if(!hkNvs.init)return ESP_ERR_NVS_NOT_INITIALIZED;
 if(hkNvs.failWriteKey==key)return ESP_FAIL;
 auto b=(const uint8_t*)in;
 (hkNvs.immediateWrites?hkNvs.committed:hkNvs.pending)[hkNvs.handles[h-1]][key].assign(b,b+len);
 if(hkNvs.writeBeforeErrorKey==key&&hkNvs.writeBeforeErrorCount>0){--hkNvs.writeBeforeErrorCount;return ESP_FAIL;}
 return ESP_OK;}
template<class T>static int getNvs(nvs_handle_t h,const char* k,T* v){size_t n=sizeof(*v);return nvs_get_blob(h,k,v,&n);}
template<class T>static int setNvs(nvs_handle_t h,const char* k,T v){return nvs_set_blob(h,k,&v,sizeof(v));}
#define NVS_NUM(type,suffix) static int nvs_get_##suffix(nvs_handle_t h,const char*k,type*v){return getNvs(h,k,v);} static int nvs_set_##suffix(nvs_handle_t h,const char*k,type v){return setNvs(h,k,v);}
NVS_NUM(uint8_t,u8) NVS_NUM(uint16_t,u16) NVS_NUM(uint32_t,u32) NVS_NUM(uint64_t,u64) NVS_NUM(int16_t,i16)
static int nvs_get_str(nvs_handle_t h,const char*k,char*v,size_t*n){return nvs_get_blob(h,k,v,n);}
static int nvs_set_str(nvs_handle_t h,const char*k,const char*v){return nvs_set_blob(h,k,v,strlen(v)+1);}
static int nvs_find_key(nvs_handle_t h,const char* k,nvs_type_t* t){if(!hkNvs.committed[hkNvs.handles[h-1]].count(k))return ESP_ERR_NVS_NOT_FOUND;*t=NVS_TYPE_BLOB;return ESP_OK;}
static int nvs_get_used_entry_count(nvs_handle_t h,size_t* n){*n=hkNvs.committed[hkNvs.handles[h-1]].size();return ESP_OK;}
static int nvs_erase_key(nvs_handle_t h,const char*k){++hkNvs.writes;if(!hkNvs.init)return ESP_ERR_NVS_NOT_INITIALIZED;hkNvs.committed[hkNvs.handles[h-1]].erase(k);hkNvs.pending[hkNvs.handles[h-1]].erase(k);return ESP_OK;}
static int nvs_commit(nvs_handle_t){if(hkNvs.onCommit){auto callback=hkNvs.onCommit;callback();}if(hkNvs.failCommit)return ESP_FAIL;if(!hkNvs.init)return ESP_ERR_NVS_NOT_INITIALIZED;for(auto& ns:hkNvs.pending)for(auto& kv:ns.second)hkNvs.committed[ns.first][kv.first]=kv.second;hkNvs.pending.clear();++hkNvs.commits;return ESP_OK;}
static int nvs_erase_all(nvs_handle_t h){++hkNvs.writes;if(!hkNvs.init)return ESP_ERR_NVS_NOT_INITIALIZED;hkNvs.committed[hkNvs.handles[h-1]].clear();return ESP_OK;}
static int nvs_flash_erase(){if(hkNvs.onErase)hkNvs.onErase();hkNvs.committed.clear();hkNvs.pending.clear();hkNvs.init=false;++hkNvs.erases;return ESP_OK;}
static int nvs_flash_init(){hkNvs.init=true;++hkNvs.inits;if(hkNvs.onInit)hkNvs.onInit();return ESP_OK;}
static int nvs_flash_deinit(){hkNvs.init=false;++hkNvs.deinits;return ESP_OK;}
using esp_event_base_t=int;constexpr int HAP_EVENT=0,ESP_EVENT_ANY_ID=0;
enum {HAP_EVENT_PAIRING_STARTED,HAP_EVENT_PAIRING_ABORTED,HAP_EVENT_CTRL_PAIRED,HAP_EVENT_CTRL_UNPAIRED,HAP_EVENT_CTRL_CONNECTED,HAP_EVENT_CTRL_DISCONNECTED,HAP_EVENT_ACC_REBOOTING,HAP_EVENT_PAIRING_MODE_TIMED_OUT};
#define HAP_REBOOT_REASON_RESET_PAIRINGS "reset_pairings"
static int controllers=0,resetResult=0,startResult=0,resetRequests=0;
constexpr int HAP_SUCCESS=0,HAP_TRANSPORT_WIFI=0,UNIQUE_NONE=0,ESP_MAC_WIFI_STA=0;
using hap_cid_t=int;constexpr int HAP_CID_BRIDGE=2,HAP_CID_AIR_CONDITIONER=21;
struct hap_acc_t{};
struct hap_cfg_t {int unique_param,max_event_notif_chars;};
struct hap_acc_cfg_t {char*name;char*model;char*manufacturer;char*serial_num;char*fw_rev;char*hw_rev;char*pv;int cid;int(*identify_routine)(hap_acc_t*);};
static int hap_get_paired_controller_count(){return controllers;}
static int hap_reset_pairings(){++resetRequests;return resetResult;}
static void hap_get_config(hap_cfg_t*cfg){*cfg={};}static void hap_set_config(hap_cfg_t*){}
static int hapInitCalls=0,hapStartCalls=0;
static int hap_init(int){++hapInitCalls;return HAP_SUCCESS;}static int hap_start(){++hapStartCalls;return startResult;}
static void hap_set_setup_id(const char*){}static void hap_set_setup_code(const char*){}
static int configCalls=0,configResult=HAP_SUCCESS;
static int hap_update_config_number(){++configCalls;return configResult;}
static hap_acc_t* hap_acc_create(hap_acc_cfg_t*){static hap_acc_t a;return &a;}
static void hap_acc_add_product_data(hap_acc_t*,uint8_t*,size_t){}
static void hap_acc_add_wifi_transport_service(hap_acc_t*,int){}
static void hap_add_accessory(hap_acc_t*){}static void hap_add_bridged_accessory(hap_acc_t*,int){}
static int hap_get_unique_aid(const char*){return 2;}static void homekit_services_create_all(hap_acc_t*){}
static void status_led_identify(){}static void mdns_hostname_set(const char*){}
static void esp_read_mac(uint8_t*mac,int){memset(mac,1,6);}
static char* esp_hap_get_setup_payload(const char*,char*,bool,int){return strdup("X-HM://test");}
static int esp_event_handler_register(int,int,void(*)(void*,int,int32_t,void*),void*){return ESP_OK;}
