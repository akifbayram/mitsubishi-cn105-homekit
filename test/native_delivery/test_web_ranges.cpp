
using namespace SetpointRange;
#define CASE(name) if (!strcmp(argv[1], name))
static char reply[256];
static Result apply(const char* text){return webApplyRangeConfig(text,reply,sizeof(reply));}
static SetpointSettings autoPolicy(bool high) {
 auto next=settings.setpointSettings();
 next.preferences.mode[2]={Selection::Manual,high ? Range{44,60} : Range{20,36}};
 next.band=high ? Band{48,56} : Band{24,32};
 next.autoOwnerUnit=high;
 return next;
}
static void checkAutoPolicy(const cJSON* root,bool high) {
 const auto* ranges=cJSON_GetObjectItemCaseSensitive(root,"setpointRanges");
 const auto* profile=cJSON_GetObjectItemCaseSensitive(ranges,"auto");
 const auto* heat=cJSON_GetObjectItemCaseSensitive(root,"heatThresh");
 const auto* cool=cJSON_GetObjectItemCaseSensitive(root,"coolThresh");
 CHECK(cJSON_IsObject(profile));
 CHECK(cJSON_IsNumber(heat));
 CHECK(cJSON_IsNumber(cool));
 CHECK(cJSON_GetObjectItemCaseSensitive(profile,"min")->valuedouble==(high ? 22 : 10));
 CHECK(cJSON_GetObjectItemCaseSensitive(profile,"max")->valuedouble==(high ? 30 : 18));
 CHECK(cJSON_GetObjectItemCaseSensitive(profile,"manualMin")->valuedouble==(high ? 22 : 10));
 CHECK(cJSON_GetObjectItemCaseSensitive(profile,"manualMax")->valuedouble==(high ? 30 : 18));
 CHECK(heat->valuedouble==(high ? 24 : 12));
 CHECK(cool->valuedouble==(high ? 28 : 16));
 const auto* owner=cJSON_GetObjectItemCaseSensitive(root,"autoUnit");
 CHECK(cJSON_IsBool(owner));
 CHECK(cJSON_IsTrue(owner)==high);
}
// A boot that finds Single in NVS, through the real settings load.
static void bootSingle(CN105Controller& c) {
 hkNvs.committed["ac-settings"]["autoSingle"]={1};
 settings=SettingsStore{};
 settings.begin();
 SetpointControl::begin(c);
}
static bool stateBool(const std::string& state,const char* key) {
 auto root=webParseObject(state.c_str());
 CHECK(root);
 const auto* value=cJSON_GetObjectItemCaseSensitive(root.get(),key);
 CHECK(cJSON_IsBool(value));
 return cJSON_IsTrue(value);
}
int main(int,char**argv){
 settings.begin();
 Pump pump;
 pump.mode=CN105_MODE_HEAT;
 CN105Controller c;
 now_ms=5000;
 c.begin(&pump);
 ticks(c);
 SetpointControl::begin(c);
 CASE("reversed_bounds") {CHECK(apply(R"({"cmd":"config","rangeHeatSource":"manual","rangeHeatMin":25,"rangeHeatMax":18})")==Result::Invalid);}
 CASE("incomplete_pair") {CHECK(apply(R"({"cmd":"config","rangeHeatMin":18})")==Result::Invalid);}
 CASE("strict_types_and_tokens") {
  for(const char* text:{R"({"rangeHeatMin":null,"rangeHeatMax":24})",R"({"rangeHeatMin":"18","rangeHeatMax":24})",R"({"heatThresh":1e999})",R"({"heatThresh":20oops})",R"({"heatThresh":NaN})",R"({"heatThresh":20}garbage)",R"({"rangeHeatSource":5})"})CHECK(apply(text)==Result::Invalid);
 }
 CASE("duplicate_keys") {for(const char* text:{R"({"heatThresh":20,"heatThresh":21})",R"({"deviceName":"a","deviceName":"b"})",R"({"nested":{"min":1,"min":2}})"})CHECK(apply(text)==Result::Invalid);}
 CASE("malformed_other_config_field_blocks_range_side_effects") {
  CHECK(apply(R"({"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31,"deviceName":null})")==Result::Invalid);
  CHECK(settings.setpointSettings().preferences.mode[0].selection!=Selection::Manual);
  CHECK(apply(R"({"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31,"trustedHost":7})")==Result::Invalid);
  CHECK(settings.setpointSettings().preferences.mode[0].selection!=Selection::Manual);
 }
 CASE("strict_json_number_grammar") {
  for (const char* text : {R"({"heatThresh":020})",R"({"heatThresh":20.})",R"({"heatThresh":2e1.0})"}) {
   CHECK(apply(text)==Result::Invalid);
  }
 }
 CASE("heat_only_preserves_other_modes") {
  auto initial=settings.setpointSettings();
  for(auto& p:initial.preferences.mode) p.selection=Selection::Compatibility;
  CHECK(settings.saveSetpointSettings(initial));
  CHECK(apply(R"({"cmd":"config","requestId":7,"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31})")==Result::Ok);
  const auto saved=settings.setpointSettings();
  CHECK(saved.preferences.mode[0].manual.min2==20);
  CHECK(saved.preferences.mode[0].selection==Selection::Manual);
  CHECK(saved.preferences.mode[1].selection==Selection::Compatibility);
  CHECK(saved.preferences.mode[2].selection==Selection::Compatibility);
  CHECK(strstr(reply,"\"requestId\":7"));
  CHECK(strstr(reply,"\"ok\":true"));
 }
 CASE("failed_persistence_keeps_range_and_band") {
  const auto old=settings.setpointSettings();
  hkNvs.failWriteKey="rangeCfg";
  CHECK(apply(R"({"requestId":8,"rangeAutoSource":"manual","rangeAutoMin":18,"rangeAutoMax":26,"heatThresh":19,"coolThresh":23})")==Result::StorageFailure);
  CHECK(settings.setpointSettings().preferences.mode[2].selection==old.preferences.mode[2].selection);
  CHECK(settings.get().heatingThreshold==20);
  CHECK(strstr(reply,"storage_failed"));
  CHECK(strstr(reply,"\"ok\":false"));
 }
 CASE("malformed_band_never_saves_profile") {
  CHECK(apply(R"({"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31,"heatThresh":"bad","coolThresh":25})")==Result::Invalid);
  CHECK(settings.setpointSettings().preferences.mode[0].selection!=Selection::Manual);
  CHECK(settings.get().heatingThreshold==20);
  CHECK(!c._staged.flags1);
 }
 CASE("range_and_band_one_commit") {
  const int commits=hkNvs.commits;
  CHECK(apply(R"({"rangeAutoSource":"manual","rangeAutoMin":10,"rangeAutoMax":31,"heatThresh":11,"coolThresh":15})")==Result::Ok);
  CHECK(settings.get().heatingThreshold==11);
  CHECK(settings.get().coolingThreshold==15);
  CHECK(hkNvs.commits==commits+1);
 }
 CASE("commit_failure_keeps_range_and_band") {
  hkNvs.failCommit=true;
  CHECK(apply(R"({"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31,"heatThresh":19})")==Result::StorageFailure);
  CHECK(settings.setpointSettings().preferences.mode[0].selection!=Selection::Manual);
  CHECK(settings.get().heatingThreshold==20);
  CHECK(strstr(reply,"storage_failed"));
 }
 CASE("escaped_strings_do_not_confuse_numeric_validation") {
  CHECK(apply(R"({"deviceName":"A \"room\" 020", "heatThresh":20})")==Result::Ok);
 }
 CASE("request_id_type_rejected") {CHECK(apply(R"({"requestId":"7","heatThresh":19})")==Result::Invalid);CHECK(settings.get().heatingThreshold==20);}
 CASE("serialized_ranges_are_complete_saved_preferences") {
  CHECK(apply(R"({"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31})")==Result::Ok);
  auto state=webRangeState(8192);
  auto root=webParseObject(state.c_str());
  CHECK(root);
  const auto* ranges=cJSON_GetObjectItemCaseSensitive(root.get(),"setpointRanges");
  CHECK(cJSON_IsObject(ranges));
  const auto* heat=cJSON_GetObjectItemCaseSensitive(ranges,"heat");
  CHECK(cJSON_GetObjectItemCaseSensitive(heat,"manualMin")->valuedouble==10);
  CHECK(!strcmp(cJSON_GetObjectItemCaseSensitive(heat,"selection")->valuestring,"manual"));
  CHECK(cJSON_GetObjectItemCaseSensitive(ranges,"cool"));
  CHECK(cJSON_GetObjectItemCaseSensitive(ranges,"auto"));
  CHECK(cJSON_GetObjectItemCaseSensitive(ranges,"ftabId")->valuedouble==0);
 }
 CASE("range_section_rolls_back_atomically") {
  auto state=webRangeState(200);
  auto root=webParseObject(state.c_str());
  CHECK(root);
  CHECK(!cJSON_HasObjectItem(root.get(),"setpointRanges"));
  CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"trunc")));
 }
 CASE("auto_policy_export_fields_survive_interleaved_save") {
  const auto before=autoPolicy(false);
  const auto after=autoPolicy(true);
  CHECK(SetpointControl::applySettings(before)==Result::Ok);
  afterWebRangeSection=[&] {
   std::thread writer([&] {CHECK(SetpointControl::applySettings(after)==Result::Ok);});
   writer.join();
  };
  const auto first=webRangeState(8192);
  auto oldJson=webParseObject(first.c_str());
  CHECK(oldJson);
  checkAutoPolicy(oldJson.get(),false);

  const auto second=webRangeState(8192);
  auto newJson=webParseObject(second.c_str());
  CHECK(newJson);
  checkAutoPolicy(newJson.get(),true);
 }
 CASE("concurrent_auto_saves_emit_complete_profile_band_and_owner") {
  const auto low=autoPolicy(false);
  const auto high=autoPolicy(true);
  CHECK(SetpointControl::applySettings(low)==Result::Ok);
  std::thread writer([&] {
   for(int i=0;i<200;++i)
    CHECK(SetpointControl::applySettings(i%2 ? low : high)==Result::Ok);
  });
  for(int i=0;i<200;++i) {
   const auto state=webRangeState(8192);
   auto root=webParseObject(state.c_str());
   CHECK(root);
   const auto* ranges=cJSON_GetObjectItemCaseSensitive(root.get(),"setpointRanges");
   const auto* profile=cJSON_GetObjectItemCaseSensitive(ranges,"auto");
   const auto* min=cJSON_GetObjectItemCaseSensitive(profile,"min");
   CHECK(cJSON_IsNumber(min));
   checkAutoPolicy(root.get(),min->valuedouble==22);
  }
  writer.join();
 }
 CASE("rollback_keeps_profile_band_and_ownership_together") {
  CHECK(SetpointControl::applySettings(autoPolicy(true))==Result::Ok);
  const auto full=webRangeState(8192);
  for(const size_t cap : {size_t{200},full.size()-20,full.size()+16}) {
   const auto state=webRangeState(cap);
   auto root=webParseObject(state.c_str());
   CHECK(root);
   const bool hasProfile=cJSON_HasObjectItem(root.get(),"setpointRanges");
   CHECK(cJSON_HasObjectItem(root.get(),"heatThresh")==hasProfile);
   CHECK(cJSON_HasObjectItem(root.get(),"coolThresh")==hasProfile);
   CHECK(cJSON_HasObjectItem(root.get(),"autoUnit")==hasProfile);
   CHECK(cJSON_HasObjectItem(root.get(),"rangeCondition")==hasProfile);
   if(hasProfile) checkAutoPolicy(root.get(),true);
   else CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root.get(),"trunc")));
  }
 }
 CASE("status_preserves_actual_outside_advertised_range") {
  CHECK(apply(R"({"rangeHeatSource":"automatic"})")==Result::Ok);
  pump.target=148;
  ticks(c,300);
  auto state=webRangeState(8192);
  auto root=webParseObject(state.c_str());
  CHECK(root);
  const auto* actual=cJSON_GetObjectItemCaseSensitive(root.get(),"actualTarget");
  CHECK(cJSON_IsNumber(actual));
  CHECK(actual->valuedouble==10);
  CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root.get(),"targetReadable")));
 }
 CASE("auto_single_defaults_in_state") {
  const auto state=webRangeState(8192);
  CHECK(!stateBool(state,"autoSingle"));
  CHECK(!stateBool(state,"autoSingleBoot"));
 }
 CASE("auto_single_saves_and_waits_for_restart") {
  CHECK(apply(R"({"autoSingle":true})")==Result::Ok);
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{1});
  const auto state=webRangeState(8192);
  CHECK(stateBool(state,"autoSingle"));
  CHECK(!stateBool(state,"autoSingleBoot"));
 }
 CASE("auto_single_non_boolean_rejected") {
  for(const char* text:{R"({"autoSingle":1,"rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31})",
                        R"({"autoSingle":"single","rangeHeatSource":"manual","rangeHeatMin":10,"rangeHeatMax":31})"}) {
   CHECK(apply(text)==Result::Invalid);
   CHECK(settings.setpointSettings().preferences.mode[0].selection!=Selection::Manual);
   CHECK(!settings.get().autoSingleSetpoint);
  }
 }
 CASE("older_file_leaves_auto_single_alone") {
  bootSingle(c);
  CHECK(apply(R"({"deviceName":"Old file","rangeHeatSource":"automatic"})")==Result::Ok);
  CHECK(settings.get().autoSingleSetpoint);
  CHECK(settings.autoSingleBoot());
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{1});
 }
 CASE("single_import_keeps_band_and_owner") {
  bootSingle(c);
  auto owned=settings.setpointSettings();
  owned.autoOwnerUnit=true;
  CHECK(settings.saveSetpointSettings(owned));
  CHECK(apply(R"({"heatThresh":19,"coolThresh":23,"rangeAutoSource":"manual","rangeAutoMin":16,"rangeAutoMax":28})")==Result::Ok);
  const auto saved=settings.setpointSettings();
  CHECK(saved.band.heat2==40&&saved.band.cool2==50);
  CHECK(saved.autoOwnerUnit);
  CHECK(saved.preferences.mode[2].selection==Selection::Manual);
 }
 CASE("saving_range_in_auto_hands_over") {
  bootSingle(c);
  pump.mode=CN105_MODE_AUTO;
  ticks(c,300);
  CHECK(apply(R"({"autoSingle":false})")==Result::Ok);
  const auto saved=settings.setpointSettings();
  CHECK(saved.band.heat2==42&&saved.band.cool2==46);
  CHECK(saved.autoOwnerUnit);
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{0});
  CHECK(settings.autoSingleBoot());
  settings=SettingsStore{};
  settings.begin();
  CHECK(!settings.autoSingleBoot());
  CHECK(settings.setpointSettings().autoOwnerUnit);
 }
 CASE("saving_range_outside_auto_hands_over") {
  bootSingle(c);
  CHECK(apply(R"({"autoSingle":false})")==Result::Ok);
  const auto saved=settings.setpointSettings();
  CHECK(saved.band.heat2==40&&saved.band.cool2==50);
  CHECK(saved.autoOwnerUnit);
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{0});
 }
 CASE("failed_handover_stays_single") {
  bootSingle(c);
  hkNvs.failWriteKey="rangeCfg";
  CHECK(apply(R"({"autoSingle":false})")==Result::StorageFailure);
  CHECK(settings.get().autoSingleSetpoint);
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{1});
  CHECK(strstr(reply,"storage_failed"));
 }
 CASE("failed_mode_write_stays_single") {
  // The handover succeeds, then the mode itself cannot be stored.
  bootSingle(c);
  pump.mode=CN105_MODE_AUTO;
  ticks(c,300);
  hkNvs.failWriteKey="autoSingle";
  CHECK(apply(R"({"autoSingle":false})")==Result::StorageFailure);
  CHECK(strstr(reply,"storage_failed"));
  CHECK(settings.get().autoSingleSetpoint);
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{1});
  const auto kept=settings.setpointSettings();
  CHECK(kept.band.heat2==40&&kept.band.cool2==50);
  CHECK(!kept.autoOwnerUnit);
  // Storage recovers: the same Save now works rather than reading as no change.
  hkNvs.failWriteKey.clear();
  CHECK(apply(R"({"autoSingle":false})")==Result::Ok);
  CHECK(hkNvs.committed["ac-settings"]["autoSingle"]==std::vector<uint8_t>{0});
  CHECK(settings.setpointSettings().autoOwnerUnit);
 }
 CASE("failed_commit_stays_range") {
  hkNvs.failCommit=true;
  CHECK(apply(R"({"autoSingle":true})")==Result::StorageFailure);
  CHECK(!settings.get().autoSingleSetpoint);
  CHECK(!stateBool(webRangeState(8192),"autoSingle"));
 }
 CASE("unavailable_policy_refuses_range") {
  bootSingle(c);
  settings._setpointAvailable=false;
  CHECK(apply(R"({"autoSingle":false})")==Result::Unavailable);
  CHECK(settings.get().autoSingleSetpoint);
 }
 CASE("rollback_keeps_auto_single_with_profile") {
  const auto full=webRangeState(8192);
  for(const size_t cap : {size_t{200},full.size()-20,full.size()+16}) {
   const auto state=webRangeState(cap);
   auto root=webParseObject(state.c_str());
   CHECK(root);
   const bool hasProfile=cJSON_HasObjectItem(root.get(),"setpointRanges");
   CHECK(cJSON_HasObjectItem(root.get(),"autoSingle")==hasProfile);
   CHECK(cJSON_HasObjectItem(root.get(),"autoSingleBoot")==hasProfile);
  }
 }
 puts("PASS");
 CASE("discovery_and_encoding_use_effective_auto_band") {
  // Fresh discovery changes the usable band without rewriting saved intent.
  auto cfg = settings.setpointSettings();
  cfg.preferences.mode[2].selection = Selection::Automatic;
  CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
  c._state.mode = CN105_MODE_AUTO;
  c._state.power = true;
  uint8_t data[16] = {};
  data[0] = 0xC9;
  data[8] = 4;
  data[14] = 176;
  data[15] = 184; // Auto 24..28 C.
  c.handleIdentifyResponse(data, sizeof data);
  SetpointControl::loop();
  float heat = 24, cool = 26;
  auto root = webParseObject(webRangeState(8192).c_str());
  CHECK(root);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "heatThresh")->valuedouble == heat);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "coolThresh")->valuedouble == cool);
  CHECK(settings.setpointSettings().band.heat2 == 40);
  CHECK(settings.setpointSettings().band.cool2 == 50);
  c._state.roomTemp = 28;
  SetpointControl::steerAuto();
  CHECK(c.getEffectiveState().targetTemp == cool);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "savedHeatThresh")->valuedouble == 20);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "savedCoolThresh")->valuedouble == 25);
  CHECK(apply(R"({"heatThresh":26})") == Result::Ok);
  CHECK(settings.setpointSettings().band.heat2 == 50);
  CHECK(settings.setpointSettings().band.cool2 == 54);
 }
 CASE("legacy_refresh_uses_effective_auto_band") {
  // A half-degree saved band survives reconnect; operational thresholds use whole C.
  auto cfg = settings.setpointSettings();
  cfg.preferences.mode[2].selection = Selection::Automatic;
  cfg.band = {45, 49};
  CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
  pump.mode = CN105_MODE_AUTO;
  c.restartHandshake();
  pump.legacy = true;
  now_ms += 4000;
  ticks(c, 300);
  SetpointControl::loop();
  float heat = 22, cool = 24;
  auto root = webParseObject(webRangeState(8192).c_str());
  CHECK(root);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "heatThresh")->valuedouble == heat);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "coolThresh")->valuedouble == cool);
  CHECK(settings.setpointSettings().band.heat2 == 45);
  CHECK(settings.setpointSettings().band.cool2 == 49);
  c._state.roomTemp = 20;
  SetpointControl::steerAuto();
  CHECK(c.getEffectiveState().targetTemp == heat);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "savedHeatThresh")->valuedouble == 22.5);
  CHECK(cJSON_GetObjectItemCaseSensitive(root.get(), "savedCoolThresh")->valuedouble == 24.5);
  // A Heat-only edit does not silently save the projected Auto band.
  CHECK(apply(R"({"rangeHeatSource":"manual","rangeHeatMin":18,"rangeHeatMax":28})") == Result::Ok);
  CHECK(settings.setpointSettings().band.heat2 == 45);
  CHECK(settings.setpointSettings().band.cool2 == 49);
 }

}
