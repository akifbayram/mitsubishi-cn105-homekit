using namespace SetpointRange;
#define CASE(name) if (!strcmp(argv[1], name))
static void manual(int mode,int low,int high){auto s=settings.setpointSettings();s.preferences.mode[mode]={Selection::Manual,{(int16_t)(low*2),(int16_t)(high*2)}};CHECK(SetpointControl::applySettings(s)==Result::Ok);}
int main(int,char**argv){
 settings.begin();Pump pump; pump.mode=CN105_MODE_HEAT;CN105Controller c;now_ms=5000;c.begin(&pump);ticks(c);SetpointControl::begin(c);g_homekitCtrl=&c;
 CASE("batch_invalid_target_leaves_mailbox_unchanged") {manual(0,18,24);SetpointControl::Command cmd;cmd.unit.power=true;cmd.unit.targetTemp=29;CHECK(SetpointControl::submit(cmd)==Result::OutsideRange);CHECK(!c._staged.flags1);CHECK(!c._wanted.hasPower);}
 CASE("mode_and_target_order_independent") {manual(0,10,16);manual(1,20,30);SetpointControl::Command cmd;cmd.unit.targetTemp=26;cmd.unit.mode=CN105_MODE_COOL;CHECK(SetpointControl::submit(cmd)==Result::Ok);CHECK(c.getEffectiveState().targetTemp==26);CHECK(c.getEffectiveState().mode==CN105_MODE_COOL);}
 CASE("mode_only_normalizes_retained_target") {manual(1,24,30);SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_COOL;CHECK(SetpointControl::submit(cmd)==Result::Ok);CHECK(c.getEffectiveState().targetTemp==24);}
 CASE("fan_service_power_on_checks_range") {manual(0,24,30);SetpointControl::Command cmd;cmd.unit.power=true;CHECK(SetpointControl::submit(cmd)==Result::Ok);CHECK(c.getEffectiveState().targetTemp==24);}
 CASE("dry_switch_uses_cool_range") {manual(1,24,30);SetpointControl::Command cmd;cmd.unit.power=true;cmd.unit.mode=CN105_MODE_DRY;CHECK(SetpointControl::submit(cmd)==Result::Ok);CHECK(c.getEffectiveState().targetTemp==24);}
 CASE("wire_enhanced_endpoints") {manual(0,10,31);for(float t:{10.f,31.f}){SetpointControl::Command cmd;cmd.unit.targetTemp=t;CHECK(SetpointControl::submit(cmd)==Result::Ok);ticks(c,300);CHECK(pump.target==(t==10?0x94:0xBE));}}
 CASE("wire_legacy_endpoints") {pump.legacy=true;ticks(c,300);manual(0,16,31);for(float t:{16.f,31.f}){SetpointControl::Command cmd;cmd.unit.targetTemp=t;CHECK(SetpointControl::submit(cmd)==Result::Ok);ticks(c,300);CHECK(pump.legacyC==t);bool found=false;for(auto&p:pump.sent)if(p[1]==CN105_PKT_SET&&(p[6]&CN105_FLAG_TEMP)&&p[10]==(t==16?0x0F:0x00)&&p[19]==0)found=true;CHECK(found);}}
 CASE("save_replaces_earlier_queued_target") {SetpointControl::Command cmd;cmd.unit.targetTemp=30;CHECK(SetpointControl::submit(cmd)==Result::Ok);manual(0,18,24);CHECK(c.getEffectiveState().targetTemp<=24);ticks(c,300);CHECK(pump.target<=176);}
 CASE("storage_failure_leaves_command_and_ownership_unchanged") {auto s=settings.setpointSettings();s.autoOwnerUnit=true;CHECK(settings.saveSetpointSettings(s));hkNvs.failWriteKey="rangeCfg";SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_AUTO;cmd.unit.power=true;cmd.heatingThreshold=23;cmd.coolingThreshold=25;CHECK(SetpointControl::submit(cmd)==Result::StorageFailure);CHECK(settings.get().autoOwnerUnit);CHECK(c._staged.flags1==0);CHECK(c.getEffectiveState().mode==CN105_MODE_HEAT);}
 CASE("external_violation_retry_and_refusal_bounded") {
  manual(0,18,24);pump.target=188;
  for(int i=0;i<18000;++i){c.loop();SetpointControl::loop();if(pump.target<188)pump.target=188;now_ms+=10;}
  CHECK(pump.sets==2);CHECK(SetpointControl::status().condition==SetpointControl::Condition::Refused);
  const int n=pump.sets;auto cfg=settings.setpointSettings();CHECK(SetpointControl::applySettings(cfg)==Result::Ok);
  for(int i=0;i<4000;++i){c.loop();SetpointControl::loop();now_ms+=10;}CHECK(pump.sets==n);
 }
 CASE("external_low_and_physical_off") {
  manual(0,18,24);pump.target=160;ticks(c,300);SetpointControl::loop();CHECK(SetpointControl::status().condition==SetpointControl::Condition::Pending);
  ticks(c,300);SetpointControl::loop();CHECK(pump.target==164);CHECK(SetpointControl::status().condition==SetpointControl::Condition::Satisfied);
  pump.power=false;pump.target=188;ticks(c,300);int n=pump.sets;SetpointControl::loop();ticks(c,300);CHECK(!pump.power);CHECK(pump.sets==n);
 }
 CASE("failed_save_resumes_previous_queued_target") {
  SetpointControl::Command cmd;cmd.unit.targetTemp=29;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  hkNvs.writeBeforeErrorKey="rangeCfg";hkNvs.writeBeforeErrorCount=1;auto cfg=settings.setpointSettings();cfg.preferences.mode[0]={Selection::Manual,{36,48}};
  CHECK(SetpointControl::applySettings(cfg)==Result::StorageFailure);ticks(c,300);CHECK(pump.target==186);CHECK(settings.setpointSettings().preferences.mode[0].selection!=Selection::Manual);
 }
 CASE("held_power_on_can_be_cancelled_by_off") {
  manual(0,10,14);SetpointControl::Command cmd;cmd.unit.power=true;cmd.unit.targetTemp=12;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  c.restartHandshake();pump.legacy=true;now_ms+=4000;ticks(c,300);SetpointControl::loop();CHECK(c.commandsHeld());
  cmd={};cmd.unit.power=false;CHECK(SetpointControl::submit(cmd)==Result::Ok);ticks(c,300);CHECK(!pump.power);CHECK(pump.legacyC==22);
 }
 CASE("reconnect_revalidates_before_encoding_change_send") {
  manual(0,10,14);SetpointControl::Command cmd;cmd.unit.targetTemp=12;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  const int n=pump.sets;c.restartHandshake();pump.legacy=true;now_ms+=4000;ticks(c,300);SetpointControl::loop();ticks(c,300);
  CHECK(pump.sets==n);CHECK(SetpointControl::status().condition==SetpointControl::Condition::Incompatible);
  CHECK(settings.setpointSettings().preferences.mode[0].manual.min2==20);
 }
 CASE("disconnected_save_waits_for_baseline") {
  c.restartHandshake();manual(0,10,14);CHECK(pump.sets==0);now_ms+=4000;ticks(c,300);SetpointControl::loop();ticks(c,300);CHECK(pump.target==156);
 }
 CASE("unavailable_storage_rejects_even_without_generation_change") {
  const auto gen=settings.generation();settings._setpointAvailable=false;SetpointControl::Command cmd;cmd.unit.power=true;CHECK(SetpointControl::submit(cmd)==Result::Unavailable);CHECK(settings.generation()==gen);CHECK(SetpointControl::status().condition==SetpointControl::Condition::Incompatible);
 }

 CASE("hap_fan_active_rejects_incompatible_power_on") {
  manual(0,10,14);c.restartHandshake();pump.legacy=true;pump.power=false;now_ms+=4000;ticks(c,300);SetpointControl::loop();
  hap_char_t active{HAP_CHAR_UUID_ACTIVE};int status=-1;hap_write_data_t w{&active,{.u=1},&status};
  CHECK(fan_write_cb(&w,1,nullptr,nullptr)==HAP_FAIL);CHECK(status==HAP_STATUS_VAL_INVALID);CHECK(active.updates==0);CHECK(!c.getEffectiveState().power);
 }
 CASE("hap_dry_switch_normalizes_cool_range") {
  manual(1,24,30);hap_char_t on{HAP_CHAR_UUID_ON};int status=-1;hap_write_data_t w{&on,{.b=true},&status};
  CHECK(dry_mode_write_cb(&w,1,nullptr,nullptr)==HAP_SUCCESS);CHECK(c.getEffectiveState().targetTemp==24);CHECK(c.getEffectiveState().mode==CN105_MODE_DRY);
 }
 CASE("hap_batch_invalid_target_has_no_cached_success") {
  manual(0,18,24);hap_char_t mode{HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE},target{HAP_CHAR_UUID_TARGET_TEMPERATURE};int st[2]={-1,-1};
  hap_write_data_t w[2]={{&mode,{.u=1},&st[0]},{&target,{.f=29},&st[1]}};
  CHECK(thermostat_write_cb(w,2,nullptr,nullptr)==HAP_FAIL);CHECK(!mode.updates&&!target.updates);CHECK(!c._sendRequested);
 }

 CASE("web_malformed_target_does_not_partly_power_on") {
  pump.power=false;ticks(c,300);applyWeb(R"({"power":true,"target":"bad"})");CHECK(webError);CHECK(!c._sendRequested);CHECK(!c.getEffectiveState().power);
 }
 CASE("web_complete_batch_rejects_and_uses_resulting_mode") {
  manual(0,18,24);manual(1,24,30);applyWeb(R"({"power":true,"target":29,"mode":"heat"})");CHECK(webError);CHECK(!c._sendRequested);
  applyWeb(R"({"target":29,"mode":"cool","power":true})");CHECK(!webError);CHECK(c.getEffectiveState().targetTemp==29);CHECK(c.getEffectiveState().mode==CN105_MODE_COOL);
 }
 CASE("disconnected_last_known_legacy_validates_edits") {
  pump.legacy=true;
  ticks(c,300);
  SetpointControl::loop();
  CHECK(settings.setpointSettings().lastEncoding==Encoding::Legacy);
  c.restartHandshake();
  const auto old=settings.setpointSettings();
  const int sets=pump.sets;
  for (const Range bounds : {Range{20,28},Range{33,49}}) {
   auto next=old;
   next.lastEncoding=Encoding::Enhanced; // Caller cannot override encoding evidence.
   next.preferences.mode[0]={Selection::Manual,bounds};
   CHECK(SetpointControl::applySettings(next)==Result::Invalid);
   CHECK(settings.setpointSettings().preferences.mode[0].selection==old.preferences.mode[0].selection);
   CHECK(pump.sets==sets);
  }
 }
 CASE("disconnected_last_known_enhanced_accepts_deferred") {
  SetpointControl::loop();
  c.restartHandshake();
  auto next=settings.setpointSettings();
  next.preferences.mode[0]={Selection::Manual,{20,28}};
  const int sets=pump.sets;
  CHECK(SetpointControl::applySettings(next)==Result::Ok);
  CHECK(settings.setpointSettings().preferences.mode[0].manual.min2==20);
  CHECK(pump.sets==sets);
  CHECK(SetpointControl::status().condition==SetpointControl::Condition::Pending);
 }
 CASE("no_encoding_history_accepts_firmware_envelope_deferred") {
  Pump silent;
  silent.responding=false;
  CN105Controller unconnected;
  unconnected.begin(&silent);
  auto next=settings.setpointSettings();
  next.lastEncoding=Encoding::Unknown;
  CHECK(settings.saveSetpointSettings(next));
  SetpointControl::begin(unconnected);
  next.preferences.mode[0]={Selection::Manual,{20,62}};
  CHECK(SetpointControl::applySettings(next)==Result::Ok);
  CHECK(silent.sets==0);
  CHECK(SetpointControl::snapshot().mode[0].availability==Availability::AwaitingBaseline);
 }
 CASE("settings_view_keeps_concurrent_policy_captures_complete") {
  auto initial=settings.setpointSettings();
  initial.preferences.mode[0]={Selection::Manual,{20,28}};
  CHECK(SetpointControl::applySettings(initial)==Result::Ok);
  std::thread writer([&] {
   for (int i=0;i<200;++i) {
    auto next=initial;
    next.preferences.mode[0].manual=i%2 ? Range{20,28} : Range{48,60};
    CHECK(SetpointControl::applySettings(next)==Result::Ok);
   }
  });
  for (int i=0;i<200;++i) {
   const auto view=SetpointControl::settingsView();
   CHECK(view.saved.preferences.mode[0].selection==Selection::Manual);
   CHECK(view.saved.preferences.mode[0].manual.min2==view.ranges.mode[0].range.min2);
   CHECK(view.saved.preferences.mode[0].manual.max2==view.ranges.mode[0].range.max2);
   CHECK(view.ranges.mode[0].source==Source::Manual);
  }
  writer.join();
 }
 CASE("snapshot_retains_confirmed_encoding_after_disconnect") {
  SetpointControl::loop();CHECK(settings.setpointSettings().lastEncoding==Encoding::Enhanced);
  c.restartHandshake();CHECK(SetpointControl::snapshot().encoding==Encoding::Unknown);CHECK(settings.setpointSettings().lastEncoding==Encoding::Enhanced);
 }
 CASE("held_mailbox_does_not_block_polling_or_remote_feed") {
  SetpointControl::Command cmd;cmd.unit.targetTemp=26;CHECK(SetpointControl::submit(cmd)==Result::Ok);c.holdCommands();int infos=pump.infos,n=pump.sets;
  c.sendRemoteTemperature(21);ticks(c,1000);CHECK(pump.remoteSets==1);CHECK(pump.infos>infos);CHECK(pump.sets==n+1);CHECK(pump.target==172);
 }
 CASE("save_during_inflight_target_invalidates_old_intent") {
  SetpointControl::Command cmd;cmd.unit.targetTemp=30;CHECK(SetpointControl::submit(cmd)==Result::Ok);c.loop();CHECK(pump.target==188);
  manual(0,18,24);CHECK(c.getEffectiveState().targetTemp==24);ticks(c,300);CHECK(pump.target==176);
 }
 CASE("incompatible_heat_still_allows_fan_and_off") {
  manual(0,10,14);c.restartHandshake();pump.legacy=true;now_ms+=4000;ticks(c,300);SetpointControl::loop();
  SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_FAN;cmd.unit.power=true;CHECK(SetpointControl::submit(cmd)==Result::Ok);ticks(c,300);CHECK(pump.mode==CN105_MODE_FAN);
 }

 CASE("incompatible_saved_profile_does_not_block_other_mode_edit") {
  manual(0,10,14);c.restartHandshake();pump.legacy=true;now_ms+=4000;ticks(c,300);SetpointControl::loop();
  auto cfg=settings.setpointSettings();const int writes=hkNvs.writes;CHECK(SetpointControl::applySettings(cfg)==Result::Ok);CHECK(hkNvs.writes==writes);
  cfg.preferences.mode[1]={Selection::Manual,{40,56}};CHECK(SetpointControl::applySettings(cfg)==Result::Ok);
  CHECK(settings.setpointSettings().preferences.mode[0].manual.min2==20);CHECK(settings.setpointSettings().preferences.mode[1].manual.min2==40);
 }
 CASE("untouched_unavailable_auto_band_survives_other_edit") {
  auto cfg=settings.setpointSettings();cfg.preferences.mode[2]={Selection::Manual,{20,28}};cfg.band={20,24};CHECK(SetpointControl::applySettings(cfg)==Result::Ok);
  c.restartHandshake();pump.legacy=true;now_ms+=4000;ticks(c,300);SetpointControl::loop();cfg=settings.setpointSettings();cfg.preferences.mode[1]={Selection::Manual,{40,56}};
  CHECK(SetpointControl::applySettings(cfg)==Result::Ok);CHECK(settings.setpointSettings().band.heat2==20);CHECK(settings.setpointSettings().band.cool2==24);
 }
 CASE("lost_manual_correction_retried_once") {
  manual(0,18,24);pump.target=188;pump.dropTemp=1;
  for(int i=0;i<8000;++i){c.loop();SetpointControl::loop();now_ms+=10;}
  CHECK(pump.sets==2);CHECK(pump.target==176);CHECK(SetpointControl::status().condition==SetpointControl::Condition::Satisfied);
 }
 CASE("discovery_caches_pairs_without_changing_manual_preferences") {
  manual(0,10,14);const auto old=SetpointControl::snapshot();
  c._rangeCapabilities.detected={{{36,56},{38,58},{40,56}},7,7};++c._rangeCapabilities.revision;SetpointControl::loop();
  const auto fresh=SetpointControl::snapshot();CHECK(fresh.mode[0].source==Source::Manual);CHECK(fresh.mode[0].range.min2==20);
  CHECK(fresh.mode[1].source==Source::Fresh);CHECK(settings.detectedRanges().mode[1].min2==38);CHECK(fresh.generation>old.generation);
  SetpointControl::loop();CHECK(SetpointControl::snapshot().generation==fresh.generation);
  c.restartHandshake();CHECK(SetpointControl::snapshot().mode[1].source==Source::Cached);
 }
 CASE("range_status_does_not_clear_equipment_error") {
  manual(0,18,24);c._state.hasError=true;c._state.errorCode=0x12;CHECK(SetpointControl::status().condition==SetpointControl::Condition::Satisfied);
  CHECK(c.getState().hasError);CHECK(c.getState().errorCode==0x12);
 }
 CASE("fan_zero_speed_cancels_held_power_on") {
  manual(0,10,14);SetpointControl::Command cmd;cmd.unit.power=true;cmd.unit.targetTemp=12;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  c.restartHandshake();pump.legacy=true;now_ms+=4000;ticks(c,300);SetpointControl::loop();
  hap_char_t active{HAP_CHAR_UUID_ACTIVE},speed{HAP_CHAR_UUID_ROTATION_SPEED};int st[2]={-1,-1};
  hap_write_data_t w[2]={{&speed,{.f=0},&st[0]},{&active,{.u=1},&st[1]}};
  CHECK(fan_write_cb(w,2,nullptr,nullptr)==HAP_SUCCESS);ticks(c,300);CHECK(!pump.power);CHECK(pump.legacyC==22);
 }

 CASE("reconnect_during_persistence_keeps_accepted_batch_pending") {
  hkNvs.onCommit=[&](){hkNvs.onCommit=nullptr;c.restartHandshake();now_ms+=4000;ticks(c,300);};
  SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_AUTO;cmd.unit.power=true;cmd.heatingThreshold=23;cmd.coolingThreshold=26;
  CHECK(SetpointControl::submit(cmd)==Result::Ok);CHECK(settings.get().heatingThreshold==23);CHECK(c.commandsHeld());
  SetpointControl::loop();CHECK(!c.commandsHeld());ticks(c,300);CHECK(pump.mode==CN105_MODE_AUTO);
 }

 CASE("off_remains_deliverable_when_storage_is_unavailable") {
  settings._setpointAvailable=false;SetpointControl::Command cmd;cmd.unit.power=false;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  SetpointControl::loop();ticks(c,300);CHECK(!pump.power);
 }
 CASE("superseded_own_auto_confirmation_is_not_external") {
  pump.mode=CN105_MODE_AUTO;ticks(c,300);SetpointControl::updateAutoOwnership();
  SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_AUTO;cmd.unit.targetTemp=30;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  const uint32_t external=c.externalAutoChanges();c.loop();CHECK(pump.target==188);
  manual(2,20,24);pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();CHECK(c.externalAutoChanges()==external);
  ticks(c,300);SetpointControl::loop();SetpointControl::updateAutoOwnership();CHECK(SetpointControl::autoDeviceOwns());CHECK(pump.target==176);
 }
 CASE("automatic_ranges_do_not_enforce_external_targets") {
  pump.target=188;ticks(c,300);int n=pump.sets;SetpointControl::loop();ticks(c,300);CHECK(pump.sets==n);CHECK(pump.target==188);
 }

 CASE("hap_threshold_edit_publishes_complete_projected_pair") {
  hap_char_t heat{HAP_CHAR_UUID_HEATING_THRESHOLD_TEMPERATURE},cool{HAP_CHAR_UUID_COOLING_THRESHOLD_TEMPERATURE};cool.value.f=25;s_heatThresh=&heat;s_coolThresh=&cool;
  int st=-1;hap_write_data_t w{&heat,{.f=25},&st};CHECK(thermostat_write_cb(&w,1,nullptr,nullptr)==HAP_SUCCESS);
  CHECK(heat.value.f==24);CHECK(cool.value.f==26);CHECK(settings.get().coolingThreshold==26);
 }
 CASE("hap_mode_target_order_is_independent") {
  manual(0,18,24);manual(1,24,30);
  hap_char_t mode{HAP_CHAR_UUID_TARGET_HEATING_COOLING_STATE},target{HAP_CHAR_UUID_TARGET_TEMPERATURE};int st[2]={-1,-1};
  hap_write_data_t w[2]={{&target,{.f=29},&st[0]},{&mode,{.u=2},&st[1]}};
  CHECK(thermostat_write_cb(w,2,nullptr,nullptr)==HAP_SUCCESS);CHECK(c.getEffectiveState().targetTemp==29);CHECK(c.getEffectiveState().mode==CN105_MODE_COOL);
 }

 CASE("target_uses_resulting_mode_of_held_batch") {
  manual(0,10,14);manual(1,24,30);SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_COOL;cmd.unit.targetTemp=26;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  c.restartHandshake();now_ms+=4000;ticks(c,300);CHECK(c.commandsHeld());cmd={};cmd.unit.targetTemp=12;
  CHECK(SetpointControl::submit(cmd)==Result::OutsideRange);CHECK(c.heldCommand().targetTemp==26);CHECK(c.heldCommand().mode==CN105_MODE_COOL);
 }

 CASE("atomic_transport_batch_keeps_all_fields") {
  manual(0,10,31);SetpointControl::Command cmd;cmd.unit.power=true;cmd.unit.mode=CN105_MODE_HEAT;cmd.unit.targetTemp=10;cmd.unit.fanSpeed=CN105_FAN_2;cmd.unit.vane=CN105_VANE_3;cmd.unit.wideVane=CN105_WVANE_SWING;
  CHECK(SetpointControl::submit(cmd)==Result::Ok);const int n=pump.sets;c.loop();CHECK(pump.sets==n+1);const auto& packet=pump.sent.back();
  CHECK(packet[6]==(CN105_FLAG_POWER|CN105_FLAG_MODE|CN105_FLAG_TEMP|CN105_FLAG_FAN|CN105_FLAG_VANE));CHECK(packet[7]==CN105_FLAG2_WVANE);
  CHECK(packet[8]==1&&packet[9]==CN105_MODE_HEAT&&packet[19]==0x94);CHECK(packet[11]==CN105_FAN_2&&packet[12]==CN105_VANE_3&&packet[18]==CN105_WVANE_SWING);
 }

 CASE("fan_cancels_unsent_target_across_reconnect") {
  SetpointControl::Command cmd;cmd.unit.targetTemp=29;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  cmd={};cmd.unit.mode=CN105_MODE_FAN;cmd.unit.power=true;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  CHECK(!c._wanted.hasTemp);CHECK(!c.heldCommand().targetTemp);ticks(c,300);CHECK(pump.mode==CN105_MODE_FAN);CHECK(pump.target==172);
  c.restartHandshake();now_ms+=4000;ticks(c,300);SetpointControl::loop();CHECK(!c.commandsHeld());CHECK(!c.heldCommand().targetTemp);
  cmd={};cmd.unit.fanSpeed=CN105_FAN_3;CHECK(SetpointControl::submit(cmd)==Result::Ok);const int sent=pump.sets;ticks(c,300);
  CHECK(pump.sets==sent+1);bool found=false;for(auto& packet:pump.sent)if(packet[1]==CN105_PKT_SET&&(packet[6]&CN105_FLAG_FAN)&&packet[11]==CN105_FAN_3)found=true;CHECK(found);
 }
 CASE("fan_cancels_inflight_temp_but_keeps_own_confirmation_attribution") {
  pump.mode=CN105_MODE_AUTO;ticks(c,300);SetpointControl::updateAutoOwnership();
  SetpointControl::Command cmd;cmd.unit.mode=CN105_MODE_AUTO;cmd.unit.targetTemp=29;CHECK(SetpointControl::submit(cmd)==Result::Ok);c.loop();CHECK(pump.target==186);
  const auto external=c.externalAutoChanges();cmd={};cmd.unit.mode=CN105_MODE_FAN;CHECK(SetpointControl::submit(cmd)==Result::Ok);
  CHECK(!c._wanted.hasTemp);pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();CHECK(c.externalAutoChanges()==external);
  ticks(c,300);c.restartHandshake();now_ms+=4000;ticks(c,300);SetpointControl::loop();CHECK(!c.commandsHeld());CHECK(!c.heldCommand().targetTemp);
 }

}
