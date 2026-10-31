#define CASE(name) if (!strcmp(argv[1], name))
static std::vector<uint8_t> bytes(size_t n,uint8_t seed){std::vector<uint8_t> v(n);for(size_t i=0;i<n;i++)v[i]=(uint8_t)(seed+i*7);return v;}
int main(int, char** argv) {
 hkNvs.committed["ac-settings"]["autoSingle"]={0};
 settings.begin();
 CHECK(nvs_open("ac-events",NVS_READWRITE,&Events::s_nvs)==ESP_OK);
 s_started=true;
 const auto id=bytes(SL2_ED25519_PRIV+SL2_ED25519_PUB,3);
 hkNvs.committed["serinlink"][SL2_KV_IDENTITY]=id;
 hkNvs.committed["serinlink"][SL2_KV_BONDS]=bytes(64,9);
 hkNvs.committed["ac-settings"]["hkPaired"]={1};
 hkNvs.committed["settings"]["deviceName"]=bytes(8,1);
 hkNvs.committed["nvs.net80211"]["sta.ssid"]=bytes(32,2);
 CASE("settings_save_is_blocked_before_erase") {
  hkNvs.onErase=[](){int writes=hkNvs.writes,commits=hkNvs.commits;uint32_t gen=settings.generation();
   settings.save();CHECK(hkNvs.writes==writes);CHECK(hkNvs.commits==commits);CHECK(settings.generation()==gen);};
  CHECK(espnow_factory_erase_nvs());
 }
 CASE("main_loop_writers_are_blocked_during_identity_restore") {
  hkNvs.onInit=[](){
   int writes=hkNvs.writes,commits=hkNvs.commits;uint32_t gen=settings.generation();
   settings.get().homekitPaired=true;settings.save();CHECK(hkNvs.writes==writes);CHECK(settings.generation()==gen);
   uint8_t bonds=42;CHECK(!p_kv_set(nullptr,SL2_KV_BONDS,&bonds,1));CHECK(hkNvs.writes==writes);
   CHECK(!p_kv_set(nullptr,SL2_KV_IDENTITY,&bonds,1));CHECK(hkNvs.writes==writes);
   Events::persistLocked();CHECK(hkNvs.writes==writes);
   WifiManager::saveCredentials("late network","late password");CHECK(hkNvs.writes==writes);
   WifiManager::eraseCredentials();CHECK(hkNvs.writes==writes);
   homekit_reconcile_service_shape();CHECK(hkNvs.writes==writes);
   homekit_record_service_shape();CHECK(hkNvs.writes==writes);CHECK(hkNvs.commits==commits);
  };
  CHECK(espnow_factory_erase_nvs());
  CHECK(hkNvs.committed.size()==1);CHECK(hkNvs.committed["serinlink"].size()==1);
  CHECK(hkNvs.committed["serinlink"][SL2_KV_IDENTITY]==id);
  // The latch must remain set even if storage becomes writable again before restart.
  hkNvs.onInit=nullptr;CHECK(nvs_flash_init()==ESP_OK);int writes=hkNvs.writes;
  settings.save();CHECK(hkNvs.writes==writes);
 }
 CASE("normal_settings_save_persists") {
  strcpy(settings.get().deviceName,"Saved before reset");settings.get().homekitPaired=true;
  uint32_t gen=settings.generation();settings.save();CHECK(settings.generation()==gen+1);
  CHECK(hkNvs.committed["ac-settings"]["hkPaired"]==std::vector<uint8_t>{1});
  CHECK(!strcmp((const char*)hkNvs.committed["ac-settings"]["deviceName"].data(),"Saved before reset"));
 }
 CASE("normal_link_save_persists") {
  const auto bonds=bytes(64,19);CHECK(p_kv_set(nullptr,SL2_KV_BONDS,bonds.data(),bonds.size()));
  CHECK(hkNvs.committed["serinlink"][SL2_KV_BONDS]==bonds);
 }
 CASE("normal_event_log_persists") {
  Events::s_blob.count=1;Events::persistLocked();
  CHECK(hkNvs.committed["ac-events"]["ring"].size()==sizeof(EventBlob));
 }
 CASE("normal_wifi_credentials_save_and_erase") {
  WifiManager::saveCredentials("network","password");CHECK(s_haveCreds==1);
  CHECK(!strcmp((const char*)hkNvs.committed["wifi-creds"]["ssid"].data(),"network"));
  WifiManager::eraseCredentials();CHECK(s_haveCreds==0);CHECK(hkNvs.committed["wifi-creds"].empty());
 }
 CASE("normal_homekit_service_shape_persists") {
  homekit_reconcile_service_shape();CHECK(hkNvs.committed["hk-meta"]["svcShape"]==std::vector<uint8_t>{MODE_CAP_ALL});
 }
 CASE("identity_survives_factory_erase") {
  CHECK(espnow_factory_erase_nvs());
  CHECK(hkNvs.erases==1);
  CHECK(hkNvs.committed.size()==1);CHECK(hkNvs.committed.count("serinlink")==1);
  CHECK(hkNvs.committed["serinlink"].size()==1);                      // bonds are gone
  CHECK(hkNvs.committed["serinlink"][SL2_KV_IDENTITY]==id);           // byte for byte
  CHECK(!hkNvs.init);   // closed again: nothing else lands before the restart
 }
 CASE("factory_reset_defaults_to_single") {
  CHECK(!settings.autoSingleBoot());
  CHECK(espnow_factory_erase_nvs());CHECK(nvs_flash_init()==ESP_OK);
  s_factoryResetting.store(false);settings=SettingsStore{};settings.begin();
  CHECK(settings.get().autoSingleSetpoint);CHECK(settings.autoSingleBoot());
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{1});
  settings=SettingsStore{};settings.begin();CHECK(settings.autoSingleBoot());
 }
 CASE("no_identity_erases_everything") {
  hkNvs.committed["serinlink"].erase(SL2_KV_IDENTITY);
  CHECK(espnow_factory_erase_nvs());CHECK(hkNvs.erases==1);CHECK(hkNvs.committed.empty());CHECK(hkNvs.inits==0);
 }
 CASE("malformed_identity_is_not_restored") {
  hkNvs.committed["serinlink"][SL2_KV_IDENTITY]=bytes(40,5);
  CHECK(espnow_factory_erase_nvs());CHECK(hkNvs.erases==1);CHECK(hkNvs.committed.empty());
 }
}
