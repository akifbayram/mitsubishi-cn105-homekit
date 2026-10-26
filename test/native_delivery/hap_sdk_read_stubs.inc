// JSON/allocator and private struct boundaries for pinned SDK read/serialization
// functions compiled intact below. Test output records only value and status.
using __hap_char_t=hap_char_t;using __hap_serv_t=hap_serv_t;
struct hap_linked_serv_t {hap_serv_t*hs;hap_linked_serv_t*next;};
constexpr int HAP_CHAR_PERM_SPECIAL_READ=8,HAP_CHAR_PERM_WR=16;
struct json_gen_str_t {std::map<int,float> values;std::map<int,int> statuses;int iid=0;};
static void json_gen_start_object(json_gen_str_t*){}
static void json_gen_end_object(json_gen_str_t*){}
static void json_gen_push_array(json_gen_str_t*,const char*){}
static void json_gen_pop_array(json_gen_str_t*){}
static void json_gen_arr_set_int(json_gen_str_t*,int){}
static void json_gen_obj_set_int(json_gen_str_t*j,const char*k,int n){if(!strcmp(k,"iid"))j->iid=n;if(!strcmp(k,"status"))j->statuses[j->iid]=n;}
static void json_gen_obj_set_string(json_gen_str_t*,const char*,const char*){}
static void json_gen_obj_set_bool(json_gen_str_t*,const char*,const char*){}
static void json_gen_obj_set_null(json_gen_str_t*,const char*){}
static void hap_add_char_val_json(int format,const char*,hap_val_t*v,json_gen_str_t*j){if(format==1)j->values[j->iid]=v->f;}
static void hap_add_char_type(hap_char_t*,json_gen_str_t*){}
static void hap_add_char_perms(hap_char_t*,json_gen_str_t*){}
static void hap_add_char_ev(hap_char_t*,json_gen_str_t*,int){}
static void hap_add_char_meta(hap_char_t*,json_gen_str_t*){}
static void hap_add_char_valid_vals(hap_char_t*,json_gen_str_t*){}
static hap_serv_t* hap_char_get_parent(hap_char_t*){return thermostat;}
static hap_char_t* hap_serv_get_first_char(hap_serv_t*s){return s->chars.front();}
static hap_char_t* hap_char_get_next(hap_char_t*c){auto&v=thermostat->chars;for(size_t i=0;i<v.size();++i)if(v[i]==c)return i+1<v.size()?v[i+1]:nullptr;return nullptr;}
static void hap_char_set_owner_ctrl(hap_char_t*c,int n){c->owner_ctrl=n;}
// SDK C uses implicit void* allocation conversion. A conversion proxy keeps
// its original allocation statements intact in this C++ boundary harness.
struct Allocation {void*p;template<class T> operator T*() const{return static_cast<T*>(p);}};
static Allocation hap_platform_memory_calloc(size_t n,size_t s){return {calloc(n,s)};}
static void hap_platform_memory_free(void*p){free(p);}
