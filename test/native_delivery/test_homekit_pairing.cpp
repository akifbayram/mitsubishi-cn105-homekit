#define CASE(name) if (!strcmp(argv[1], name))
static void event(int e,const char* reason=nullptr){hap_event_handler(nullptr,HAP_EVENT,e,(void*)reason);}
static bool start(){return homekit_init("Test","Serin","CN105","serial","1.0",nullptr);}
static bool storedPaired(){auto& ns=hkNvs.committed["ac-settings"];return ns.count("hkPaired")&&ns["hkPaired"]==std::vector<uint8_t>{1};}
static void reboot(){settings=SettingsStore{};hkNvs.pending.clear();settings.begin();controllers=0;s_started=false;s_pairingsReset=false;}
int main(int,char**argv){
 // Existing installations keep the old HomeKit shape. Fresh setup is separate.
 if(strcmp(argv[1],"fresh_single_shape"))hkNvs.committed["ac-settings"]["deviceName"]={'O','l','d',0};
 settings.begin();
 CASE("fresh_single_shape") {
  CHECK(start());CHECK(settings.autoSingleBoot());
  CHECK(hkNvs.committed["hk-meta"]["svcShape"]==std::vector<uint8_t>{0x5F});CHECK(configCalls==0);
  reboot();CHECK(start());CHECK(settings.autoSingleBoot());CHECK(configCalls==0);
 }
 CASE("remote_fed_defaults_false_on_fresh_device") {
  CHECK(!settings.get().remoteEverFed);
  settings.save();reboot();CHECK(!settings.get().remoteEverFed);
 }
 CASE("remote_fed_derived_from_stored_remote_selection") {
  // Upgrading from a build without the key: a stored remote source means this device fed the pump.
  hkNvs.committed["ac-settings"]["roomSingle"]={ROOM_MEMBER_LINK};
  reboot();CHECK(settings.get().roomSingle==ROOM_MEMBER_LINK);CHECK(settings.get().remoteEverFed);
 }
 CASE("remote_fed_derived_from_stored_average_mode") {
  hkNvs.committed["ac-settings"]["roomMode"]={1};
  reboot();CHECK(settings.get().roomMode==1);CHECK(settings.get().remoteEverFed);
 }
 CASE("remote_fed_latch_survives_return_to_internal") {
  settings.get().remoteEverFed=true;settings.save();
  reboot();CHECK(settings.get().roomSingle==ROOM_MEMBER_INTERNAL);CHECK(settings.get().remoteEverFed);
 }
 CASE("auto_owner_defaults_to_device") {
  CHECK(!settings.get().autoOwnerUnit);settings.save();reboot();CHECK(!settings.get().autoOwnerUnit);
 }
 CASE("auto_owner_unit_persists_across_reboot") {
  settings.get().autoOwnerUnit=true;settings.save();reboot();CHECK(settings.get().autoOwnerUnit);
  CHECK(hkNvs.committed["ac-settings"]["autoOwner"]==std::vector<uint8_t>{1});
 }
 CASE("range_shape_unchanged_no_bump") {
  CHECK(start());reboot();CHECK(start());
  CHECK(hkNvs.committed["hk-meta"]["svcShape"]==std::vector<uint8_t>{0x1F});CHECK(configCalls==0);
 }
 CASE("auto_single_upgrade_defaults_range") {
  CHECK(!settings.get().autoSingleSetpoint);CHECK(!settings.autoSingleBoot());settings.save();
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{0});
 }
 CASE("auto_single_absent_key_is_range") {
  // Any device upgrading from older firmware has settings but no key.
  settings.save();hkNvs.committed["ac-settings"].erase("autoSingle");reboot();
  CHECK(!settings.get().autoSingleSetpoint);CHECK(!settings.autoSingleBoot());
 }
 CASE("auto_single_latches_at_boot") {
  settings.get().autoSingleSetpoint=true;settings.save();CHECK(!settings.autoSingleBoot());
  reboot();CHECK(settings.get().autoSingleSetpoint);CHECK(settings.autoSingleBoot());
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{1});
 }
 CASE("single_shape_bumps_once") {
  CHECK(start());settings.get().autoSingleSetpoint=true;settings.save();reboot();CHECK(start());
  CHECK(configCalls==1);CHECK(hkNvs.committed["hk-meta"]["svcShape"]==std::vector<uint8_t>{0x5F});
  reboot();CHECK(start());CHECK(configCalls==1);
 }
 CASE("back_to_range_bumps_again") {
  CHECK(start());settings.get().autoSingleSetpoint=true;settings.save();reboot();CHECK(start());
  settings.get().autoSingleSetpoint=false;settings.save();reboot();CHECK(start());
  CHECK(configCalls==2);CHECK(hkNvs.committed["hk-meta"]["svcShape"]==std::vector<uint8_t>{0x1F});
 }
 CASE("factory_reset_blocks_first_start") {
  settings.beginFactoryReset();
  CHECK(settings.isFactoryResetting());
  bool result=start();
  CHECK(hapInitCalls==0);CHECK(hapStartCalls==0);
  CHECK(!result);CHECK(!homekit_is_started());
 }
 CASE("factory_reset_blocks_failed_start_retry") {
  startResult=ESP_FAIL;CHECK(!start());CHECK(!homekit_is_started());
  CHECK(hapInitCalls==1);CHECK(hapStartCalls==1);
  settings.beginFactoryReset();startResult=HAP_SUCCESS;
  bool result=start();
  CHECK(hapInitCalls==1);CHECK(hapStartCalls==1);
  CHECK(!result);CHECK(!homekit_is_started());
 }
 CASE("initial_setup") {
  CHECK(!storedPaired());CHECK(start());CHECK(!storedPaired());
  CHECK(!strcmp(webHkSetupValue(true,controllers,homekit_get_setup_code()),"746-93-323"));
 }
 CASE("live_count_fallback_hides_credentials") {
  CHECK(start());controllers=1;
  CHECK(httpSetup().find("746-93-323")==std::string::npos);
  CHECK(wsSetup().find("746-93-323")==std::string::npos);
  CHECK(wsSetup().find("X-HM://")==std::string::npos);
 }
 CASE("startup_latches_existing_pairing") {
  controllers=1;CHECK(start());CHECK(storedPaired());
 }
 CASE("pairing_survives_recovery_ap") {
  CHECK(start());controllers=1;event(HAP_EVENT_CTRL_PAIRED);CHECK(storedPaired());
  int commits=hkNvs.commits;event(HAP_EVENT_CTRL_CONNECTED);CHECK(hkNvs.commits==commits);
  controllers=0;event(HAP_EVENT_CTRL_UNPAIRED);CHECK(storedPaired());
  CHECK(httpSetup().find("746-93-323")==std::string::npos);
  CHECK(wsSetup().find("746-93-323")==std::string::npos);
  CHECK(wsSetup().find("X-HM://")==std::string::npos);
  reboot();settings.save();CHECK(storedPaired());
  CHECK(httpSetup().find("746-93-323")==std::string::npos);
  CHECK(wsSetup().find("746-93-323")==std::string::npos);
  CHECK(wsSetup().find("X-HM://")==std::string::npos);
  CHECK(wsSetup().find("\"hkPaired\":true")!=std::string::npos);
 }
 CASE("reset_failure_preserves_pairing") {
  controllers=1;CHECK(start());CHECK(storedPaired());resetResult=ESP_FAIL;
  CHECK(!homekit_reset_pairings());CHECK(storedPaired());reboot();settings.save();CHECK(storedPaired());
 }
 CASE("queued_reset_clears_on_completion_without_relatching") {
  controllers=2;CHECK(start());CHECK(storedPaired());CHECK(homekit_reset_pairings());CHECK(storedPaired());
  event(HAP_EVENT_CTRL_DISCONNECTED);CHECK(storedPaired());
  event(HAP_EVENT_ACC_REBOOTING,HAP_REBOOT_REASON_RESET_PAIRINGS);CHECK(!storedPaired());
  // The SDK erases storage but leaves its in-memory controller entries valid.
  CHECK(homekit_get_controller_count()==2);event(HAP_EVENT_CTRL_CONNECTED);settings.save();CHECK(!storedPaired());
  reboot();settings.save();CHECK(!storedPaired());CHECK(start());
  CHECK(httpSetup().find("746-93-323")!=std::string::npos);
  CHECK(wsSetup().find("X-HM://test")!=std::string::npos);
  controllers=1;event(HAP_EVENT_CTRL_PAIRED);CHECK(storedPaired());
 }
 CASE("ordinary_reboot_preserves_pairing") {
  controllers=1;CHECK(start());CHECK(storedPaired());event(HAP_EVENT_ACC_REBOOTING,"reboot_acc");
  reboot();settings.save();CHECK(storedPaired());
 }
}
