using hap_status_t=int;
constexpr int HAP_FAIL=-1,HAP_STATUS_SUCCESS=0,HAP_STATUS_VAL_INVALID=-70410,HAP_STATUS_RES_BUSY=-70403,HAP_STATUS_RES_ABSENT=-70409,HAP_STATUS_COMM_ERR=-70402;
constexpr int HAP_CHAR_PERM_PR=1,HAP_CHAR_PERM_PW=2,HAP_CHAR_PERM_EV=4;
union hap_val_t {uint32_t u;float f;bool b;char* s;};
struct hap_char_t {const char* uuid;union {hap_val_t value{};hap_val_t val;};int iid=0,permission=HAP_CHAR_PERM_PR,format=0,owner_ctrl=0;bool update_called=false;float min=-INFINITY,max=INFINITY,step=0;bool floating=false;int updates=0;};
struct hap_write_data_t {hap_char_t* hc;hap_val_t val;hap_status_t* status;};
struct hap_read_data_t {hap_char_t* hc;hap_status_t* status;};
struct hap_linked_serv_t;
struct hap_serv_t {int iid=1;const char* type_uuid="thermostat";bool hidden=false,primary=true;hap_linked_serv_t* linked_servs=nullptr;void*priv=nullptr;int(*bulk_read)(hap_read_data_t[],int,void*,void*)=nullptr;std::vector<hap_char_t*> chars;int(*read_cb)(hap_char_t*,hap_status_t*,void*,void*)=nullptr;int(*write_cb)(hap_write_data_t[],int,void*,void*)=nullptr;};
static hap_serv_t* thermostat;
static bool inHttp=false,requireHttp=false;
static const char* hap_char_get_type_uuid(hap_char_t*c){return c->uuid;}
static const hap_val_t* hap_char_get_val(hap_char_t*c){return &c->value;}
static int hap_char_update_val(hap_char_t*c,hap_val_t*v){if(requireHttp)CHECK(inHttp);if(c->floating&&(v->f<c->min||v->f>c->max))return HAP_FAIL;c->value=*v;++c->updates;return HAP_SUCCESS;}
static void hap_char_float_set_constraints(hap_char_t*c,float lo,float hi,float step){if(requireHttp)CHECK(inHttp);c->min=lo;c->max=hi;c->step=step;}
static hap_char_t* float_char(const char*uuid,float v,float lo,float hi,float step){auto*c=new hap_char_t{uuid};c->floating=true;c->format=1;c->value.f=v;hap_char_float_set_constraints(c,lo,hi,step);return c;}
static hap_char_t* uint_char(const char*uuid,uint32_t v){auto*c=new hap_char_t{uuid};c->value.u=v;return c;}
static void hap_serv_add_char(hap_serv_t*s,hap_char_t*c){s->chars.push_back(c);}
static hap_char_t* hap_serv_get_char_by_uuid(hap_serv_t*s,const char*u){for(auto*c:s->chars)if(!strcmp(c->uuid,u))return c;return nullptr;}
static void hap_char_add_valid_vals(hap_char_t*,uint8_t*,uint8_t){}
static hap_char_t* hap_char_status_fault_create(int v){return uint_char("fault",v);}
static hap_char_t* hap_char_status_active_create(bool v){auto*c=uint_char("active",0);c->value.b=v;return c;}
static hap_char_t* hap_char_heating_threshold_temperature_create(float v){return float_char(HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE,v,0,25,0.1);}
static hap_char_t* hap_char_cooling_threshold_temperature_create(float v){return float_char(HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE,v,10,35,0.1);}
static hap_char_t* hap_char_name_create(char*v){auto*c=new hap_char_t{"name"};c->value.s=v;return c;}
static hap_char_t* hap_char_string_create(char*u,int,char*v){auto*c=new hap_char_t{u};c->value.s=v;return c;}
static void hap_serv_mark_primary(hap_serv_t*){}
static void hap_acc_add_serv(hap_acc_t*,hap_serv_t*){}
static void hap_serv_set_write_cb(hap_serv_t*s,decltype(s->write_cb) cb){s->write_cb=cb;}
static void hap_serv_set_read_cb(hap_serv_t*s,decltype(s->read_cb) cb){s->read_cb=cb;}
static hap_serv_t* hap_serv_thermostat_create(int current,int target,float room,float temp,int units){
 auto*s=new hap_serv_t;thermostat=s;
 hap_serv_add_char(s,uint_char(HAP_CHAR_UUID_CURRENT_HEATING_COOLING_STATE,current));
 hap_serv_add_char(s,uint_char(HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE,target));
 hap_serv_add_char(s,float_char(HAP_CHAR_UUID_CURRENT_TEMPERATURE,room,0,100,0.1));
 hap_serv_add_char(s,float_char(HAP_CHAR_UUID_TARGET_TEMPERATURE,temp,10,38,0.1));
 hap_serv_add_char(s,uint_char(HAP_CHAR_UUID_TEMPERATURE_DISPLAY_UNITS,units));return s;}
using httpd_handle_t=void*;
static httpd_handle_t serverHandle=reinterpret_cast<void*>(0x1234);
static httpd_handle_t* hap_platform_httpd_get_handle(){return &serverHandle;}
static bool queueFails=false;static int queueCalls=0;
static std::deque<std::pair<void(*)(void*),void*>> httpQueue;
static int httpd_queue_work(httpd_handle_t h,void(*cb)(void*),void*arg){CHECK(h==serverHandle);++queueCalls;if(queueFails)return ESP_FAIL;httpQueue.push_back({cb,arg});return ESP_OK;}
static void drainHttp(){inHttp=true;while(!httpQueue.empty()){auto item=httpQueue.front();httpQueue.pop_front();item.first(item.second);}inHttp=false;}
namespace RoomAvg {struct Status{bool feeding;float effective;};static Status status(){return {false,NAN};}}
CN105Controller* g_homekitCtrl=nullptr;
