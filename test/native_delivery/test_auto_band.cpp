// Main-loop cadence: ownership poll every pass, engine every 2 s. `floor`
// and `ceil` model a unit that will not hold an Auto setpoint outside them
// (raw SETTINGS byte: 166 = 19.0, 184 = 28.0).
static void drive(CN105Controller& c,Pump& p,uint32_t ms,uint8_t floor=0,uint8_t ceil=255){
 for(uint32_t t=0;t<ms;t+=10){
  // Match main.cpp: range/reconnect revalidation, ownership handover, then
  // throttled thermostat steering. The CN105 task runs independently.
  SetpointControl::loop();AutoBand::loop(c);
  if(now_ms%2000==0)run_auto_sync(c);
  c.loop();
  if(p.target<floor)p.target=floor;
  if(p.target>ceil)p.target=ceil;
  now_ms+=10;
 }
}
static void untilSent(CN105Controller& c,Pump& p,uint8_t floor){int s=p.sets;while(p.sets==s)drive(c,p,10,floor);}
#define CASE(name) if (!strcmp(argv[1], name))
int main(int, char** argv) {
 Pump pump;CN105Controller c;s_ctrl=&c;now_ms=5000;c.begin(&pump);ticks(c);CHECK(c.isHealthy());SetpointControl::begin(c);
 sl2_cmd_pkt cmd={};sl2_hvac_state_t state={};sl2_caps_pkt caps={};
 CASE("advertise_and_echo_actual_auto_band") {
  h_get_caps(nullptr,&caps);CHECK(caps.modes&(1u<<SL2_MODE_HEAT_COOL));CHECK(!(caps.modes&(1u<<SL2_MODE_AUTO)));
  CHECK(caps.band_min_gap_dc==20);CHECK(caps.set_step_dc==5);
  h_get_state(nullptr,&state);CHECK(state.mode==SL2_MODE_HEAT_COOL);CHECK(state.set_low_dc==200&&state.set_high_dc==250);
  settings.get().modeMask &= ~MODE_CAP_AUTO;h_get_caps(nullptr,&caps);CHECK(!(caps.modes&(1u<<SL2_MODE_HEAT_COOL)));
 }
 CASE("extended_auto_band_advertises_and_accepts_endpoints") {
  auto cfg=settings.setpointSettings();
  for(auto& mode:cfg.preferences.mode) mode={SetpointRange::Selection::Manual,{20,62}};
  CHECK(SetpointControl::applySettings(cfg)==SetpointRange::Result::Ok);
  CHECK(h_get_caps(nullptr,&caps));
  CHECK(caps.set_min_dc==100&&caps.set_max_dc==310&&caps.ftab_id==0);
  cmd.set_low_dc=100;
  cmd.set_high_dc=120;
  CHECK(h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.get().heatingThreshold==10&&settings.get().coolingThreshold==12);
  cmd.set_low_dc=290;
  cmd.set_high_dc=310;
  CHECK(h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.get().heatingThreshold==29&&settings.get().coolingThreshold==31);
 }
 CASE("invalid_auto_band_rejects_mode_power_and_ownership") {
  auto cfg=settings.setpointSettings();
  cfg.preferences.mode[2]={SetpointRange::Selection::Manual,{40,48}};
  cfg.autoOwnerUnit=true;
  CHECK(SetpointControl::applySettings(cfg)==SetpointRange::Result::Ok);
  pump.mode=CN105_MODE_HEAT;
  pump.power=false;
  pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);
  c.loop();
  const auto saves=settings.saves;
  cmd.mode=SL2_MODE_HEAT_COOL;
  cmd.set_low_dc=190;
  cmd.set_high_dc=240;
  CHECK(!h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP_BAND,&cmd));
  CHECK(!c.getEffectiveState().power&&c.getEffectiveState().mode==CN105_MODE_HEAT);
  CHECK(!c._sendRequested&&settings.saves==saves);
  CHECK(settings.get().autoOwnerUnit);
  CHECK(settings.get().heatingThreshold==20&&settings.get().coolingThreshold==24);
 }
 CASE("band_edit_survives_periodic_auto_engine") {
  cmd.set_low_dc=270;cmd.set_high_dc=290;CHECK(h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.get().heatingThreshold==27&&settings.get().coolingThreshold==29);CHECK(settings.saves==1);
  h_get_state(nullptr,&state);CHECK(state.set_low_dc==270&&state.set_high_dc==290);
  run_auto_sync(c);c.loop();CHECK(c.getEffectiveState().targetTemp==27);
  now_ms+=2000;run_auto_sync(c);CHECK(c.getEffectiveState().targetTemp==27);
 }
 CASE("combined_mode_band_and_gap") {
  c.setMode(CN105_MODE_HEAT);c.sendPendingChanges();c.loop();
  cmd.mode=SL2_MODE_HEAT_COOL;cmd.set_low_dc=300;cmd.set_high_dc=305;
  CHECK(h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.get().heatingThreshold==28.5f&&settings.get().coolingThreshold==30.5f);
  h_get_state(nullptr,&state);CHECK(state.mode==SL2_MODE_HEAT_COOL);CHECK(state.set_low_dc==285&&state.set_high_dc==305);
 }
 CASE("band_rejects_stale_bounds_and_snaps_valid_inputs") {
  // Range-aware controls reject stale bounds; they never report a clamped write as success.
  cmd.set_low_dc=100;cmd.set_high_dc=162;CHECK(!h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.get().heatingThreshold==20&&settings.get().coolingThreshold==25);
  cmd.set_low_dc=201;cmd.set_high_dc=249;CHECK(h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.get().heatingThreshold==20&&settings.get().coolingThreshold==25);
 }
 CASE("invalid_or_disabled_band_cannot_change_thresholds") {
  cmd.set_low_dc=SL2_DC_NA;cmd.set_high_dc=290;CHECK(!h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  CHECK(settings.saves==0);CHECK(settings.get().heatingThreshold==20&&settings.get().coolingThreshold==25);
  settings.get().modeMask &= ~MODE_CAP_AUTO;cmd.mode=SL2_MODE_HEAT_COOL;cmd.set_low_dc=270;
  h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP_BAND,&cmd);CHECK(settings.saves==0);
 }
 CASE("single_setpoint_preserved_when_leaving_auto") {
  cmd.mode=SL2_MODE_HEAT;cmd.set_dc=290;CHECK(h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP,&cmd));
  CHECK(c.getEffectiveState().targetTemp==29);CHECK(c.getEffectiveState().mode==CN105_MODE_HEAT);
  CHECK(settings.get().heatingThreshold==20&&settings.get().coolingThreshold==25);
  h_get_state(nullptr,&state);CHECK(state.set_dc==290);CHECK(state.set_low_dc==SL2_DC_NA&&state.set_high_dc==SL2_DC_NA);
 }
 CASE("legacy_auto_single_target_is_not_temporarily_accepted") {
  cmd.set_dc=290;CHECK(!h_apply(nullptr,SL2_CM_TEMP,&cmd));CHECK(c.getEffectiveState().targetTemp!=29);
 }
 CASE("legacy_unit_half_degree_threshold_sends_once") {
  pump.legacy=true;pump.roomRaw=164;settings.get().heatingThreshold=20.5f;
  ticks(c,300,10);CHECK(c.getEffectiveState().roomTemp==18);
  int sets0=pump.sets,sets1=-1,infos1=0;
  for(int t=0;t<60000;t+=10){
   if(t%2000==0){run_auto_sync(c);if(sets1<0&&pump.sets>sets0){sets1=pump.sets;infos1=pump.infos;}}
   c.loop();now_ms+=10;
  }
  CHECK(sets1==sets0+1);CHECK(pump.sets==sets1);CHECK(pump.infos>infos1);CHECK(pump.legacyC==20);
 }
 CASE("legacy_unit_confirms_truncated_target") {
  pump.legacy=true;pump.roomRaw=164;settings.get().heatingThreshold=20.5f;
  ticks(c,300,10);run_auto_sync(c);ticks(c,300,10);
  CHECK(pump.legacyC==20);CHECK(c.getEffectiveState().targetTemp==20);CHECK(!c._wanted.hasTemp);
 }
 CASE("enhanced_unit_half_degree_unchanged") {
  pump.roomRaw=164;settings.get().heatingThreshold=20.5f;CHECK(c.getState().encoding==SetpointRange::Encoding::Enhanced);
  ticks(c,300,10);int sets0=pump.sets;
  for(int t=0;t<60000;t+=10){if(t%2000==0)run_auto_sync(c);c.loop();now_ms+=10;}
  CHECK(pump.sets==sets0+1);CHECK(pump.target==(uint8_t)(20.5f*2+128));CHECK(c.getEffectiveState().targetTemp==20.5f);
 }
 // Ownership: the engine steers only while this device last wrote the Auto
 // operating point. Fixture: pump in Auto at 22.0 (172), room 21.0 (170),
 // band 20–25; roomRaw 164 = 18.0 puts the room below the band.
 CASE("engine_silent_while_unit_owns") {
  settings.get().autoOwnerUnit=true;pump.roomRaw=164;ticks(c,300,10);int sets0=pump.sets;
  run_auto_sync(c);ticks(c,300,10);CHECK(pump.sets==sets0);CHECK(c.getEffectiveState().targetTemp==22);
 }
 CASE("wall_setpoint_hands_over_and_imports_band") {
  AutoBand::loop(c);CHECK(AutoBand::deviceOwns());
  pump.target=166;ticks(c,300,10);AutoBand::loop(c);
  CHECK(settings.get().autoOwnerUnit);CHECK(settings.get().heatingThreshold==18&&settings.get().coolingThreshold==20);
  CHECK(settings.saves==1);int sets0=pump.sets;run_auto_sync(c);ticks(c,300,10);
  CHECK(pump.sets==sets0);CHECK(c.getEffectiveState().targetTemp==19);
 }
 CASE("wall_entered_auto_imports_without_steering") {
  c.setMode(CN105_MODE_HEAT);c.sendPendingChanges();ticks(c,300,10);AutoBand::loop(c);CHECK(AutoBand::deviceOwns());
  pump.mode=CN105_MODE_AUTO;pump.roomRaw=164;ticks(c,300,10);AutoBand::loop(c);
  CHECK(!AutoBand::deviceOwns());CHECK(settings.get().heatingThreshold==21&&settings.get().coolingThreshold==23);
  int sets0=pump.sets;run_auto_sync(c);ticks(c,300,10);CHECK(pump.sets==sets0);
 }
 CASE("engine_sends_keep_device_ownership") {
  pump.roomRaw=164;ticks(c,300,10);run_auto_sync(c);ticks(c,300,10);
  CHECK(c.getEffectiveState().targetTemp==20);AutoBand::loop(c);CHECK(AutoBand::deviceOwns());CHECK(settings.saves==0);
 }
 CASE("legacy_engine_sends_keep_device_ownership") {
  pump.legacy=true;pump.roomRaw=164;settings.get().heatingThreshold=20.5f;
  ticks(c,300,10);for(int t=0;t<20000;t+=10){if(t%2000==0){run_auto_sync(c);AutoBand::loop(c);}c.loop();now_ms+=10;}
  CHECK(pump.legacyC==20);CHECK(AutoBand::deviceOwns());
 }
 CASE("import_clamps_at_range_edges") {
  pump.target=189;ticks(c,300,10);AutoBand::loop(c);   // 30.5
  CHECK(settings.get().heatingThreshold==28.5f&&settings.get().coolingThreshold==30.5f);
  pump.target=160;ticks(c,300,10);AutoBand::loop(c);   // 16.0
  CHECK(settings.get().heatingThreshold==16&&settings.get().coolingThreshold==18);
 }
 CASE("dial_band_edit_reclaims_and_steering_resumes") {
  pump.target=166;ticks(c,300,10);AutoBand::loop(c);CHECK(!AutoBand::deviceOwns());
  cmd.set_low_dc=230;cmd.set_high_dc=250;CHECK(h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));CHECK(AutoBand::deviceOwns());
  run_auto_sync(c);ticks(c,300,10);CHECK(c.getEffectiveState().targetTemp==23);
 }
 CASE("dial_auto_selection_reclaims") {
  settings.get().autoOwnerUnit=true;cmd.mode=SL2_MODE_HEAT_COOL;CHECK(h_apply(nullptr,SL2_CM_MODE,&cmd));
  CHECK(AutoBand::deviceOwns());
 }
 // Refusal: band 16–18 with the room at 20.5 resolves to 18.0; the unit's
 // Auto floor is 19.0. Fixture starts at 22.0, so the unit visibly clamps.
 CASE("refused_auto_target_sent_once") {
  settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;pump.roomRaw=169;int sets0=pump.sets;
  drive(c,pump,180000,166);
  CHECK(pump.sets==sets0+1);CHECK(pump.target==166);CHECK(!c._wanted.hasTemp);
  CHECK(c.getEffectiveState().targetTemp==19);CHECK(AutoBand::deviceOwns());
 }
 CASE("refused_auto_target_at_floor_retried_once") {
  pump.target=166;ticks(c,300,10);AutoBand::loop(c);CHECK(!AutoBand::deviceOwns());   // unit already at 19.0
  settings.get().autoOwnerUnit=false;settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;
  pump.roomRaw=169;int sets0=pump.sets;drive(c,pump,180000,166);
  CHECK(pump.sets==sets0+2);CHECK(c.getEffectiveState().targetTemp==19);
 }
 CASE("wall_change_while_refused_set_in_flight_kept") {
  settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;pump.roomRaw=169;int sets0=pump.sets;
  untilSent(c,pump,166);drive(c,pump,5000,166);pump.target=171;drive(c,pump,120000,166);
  CHECK(pump.target==171);CHECK(pump.sets==sets0+1);
 }
 CASE("wall_change_after_refusal_hands_over") {
  settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;pump.roomRaw=169;
  drive(c,pump,30000,166);int sets0=pump.sets;pump.target=171;drive(c,pump,10000,166);
  CHECK(!AutoBand::deviceOwns());CHECK(settings.get().heatingThreshold==20.5f&&settings.get().coolingThreshold==22.5f);
  drive(c,pump,60000,166);CHECK(pump.target==171);CHECK(pump.sets==sets0);
 }
 CASE("lost_auto_set_retried") {
  pump.roomRaw=164;pump.dropTemp=1;int sets0=pump.sets;drive(c,pump,60000);
  CHECK(pump.target==168);CHECK(pump.sets==sets0+2);CHECK(c.getEffectiveState().targetTemp==20);
 }
 CASE("refusal_released_when_target_changes") {
  settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;pump.roomRaw=169;
  drive(c,pump,30000,166);int sets0=pump.sets;
  settings.get().heatingThreshold=22;settings.get().coolingThreshold=24;drive(c,pump,30000,166);
  CHECK(pump.target==172);CHECK(pump.sets==sets0+1);
 }
 CASE("refusal_forgotten_on_reconnect") {
  settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;pump.roomRaw=169;
  drive(c,pump,30000,166);int sets0=pump.sets;
  pump.responding=false;now_ms+=15000;ticks(c);CHECK(!c.isHealthy());pump.responding=true;now_ms+=4000;
  now_ms-=now_ms%10;drive(c,pump,120000,166);
  CHECK(c.isHealthy());CHECK(pump.sets==sets0+2);CHECK(pump.target==166);
 }
 CASE("heat_mode_refusal_does_not_hold_auto") {
  c.setMode(CN105_MODE_HEAT);c.sendPendingChanges();drive(c,pump,6000);
  c.setTargetTemp(30);c.sendPendingChanges();drive(c,pump,30000,0,184);CHECK(pump.target==184);   // Heat max 28.0
  c.setMode(CN105_MODE_AUTO);c.sendPendingChanges();settings.get().heatingThreshold=28;settings.get().coolingThreshold=30;
  pump.roomRaw=190;drive(c,pump,30000);CHECK(pump.target==188);   // Auto takes 30.0
 }
 CASE("legacy_unit_refused_target_sent_once") {
  pump.legacy=true;settings.get().heatingThreshold=16;settings.get().coolingThreshold=18;pump.roomRaw=169;
  ticks(c,300,10);int sets0=pump.sets;
  for(uint32_t t=0;t<180000;t+=10){if(now_ms%2000==0)run_auto_sync(c);AutoBand::loop(c);c.loop();if(pump.legacyC<19)pump.legacyC=19;now_ms+=10;}
  CHECK(pump.sets==sets0+1);CHECK(pump.legacyC==19);CHECK(AutoBand::deviceOwns());
 }
 CASE("accepting_unit_engine_cadence_unchanged") {
  // HomeKit-only user, unit takes everything: the send count after each room
  // step must match 0.2.8/beta.2 exactly.
  const float rooms[]={18,19,20,21,22,23,24,25,26,27,26,24,22,20,18};const int want[]={1,1,1,1,2,2,3,3,4,4,4,4,5,6,6};
  int sets0=pump.sets;for(int i=0;i<15;i++){pump.roomRaw=(uint8_t)(rooms[i]*2+128);drive(c,pump,20000);CHECK(pump.sets-sets0==want[i]);}
 }
 CASE("offline_main_loop_enforces_auto_band_before_homekit_starts") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  CHECK(!homekit_is_started());
  for(int i=0;i<100;i++){run_main_control_tick(c);c.loop();now_ms+=100;}
  CHECK(pump.target==182);CHECK(c.getState().targetTemp==27);
 }
 CASE("offline_main_loop_respects_unit_owned_auto_band") {
  settings.get().autoOwnerUnit=true;settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  CHECK(!homekit_is_started());int sets0=pump.sets;
  for(int i=0;i<100;i++){run_main_control_tick(c);c.loop();now_ms+=100;}
  CHECK(pump.sets==sets0);CHECK(c.getState().targetTemp==22);
 }
 CASE("safe_mode_does_not_run_auto_engine") {
  safeMode=true;settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  int sets0=pump.sets;
  for(int i=0;i<100;i++){run_main_control_tick(c);c.loop();now_ms+=100;}
  CHECK(pump.sets==sets0);CHECK(c.getState().targetTemp==22);
 }
 CASE("dial_invalid_target_cannot_power_on") {
  pump.power=false;pump.mode=CN105_MODE_HEAT;ticks(c,300);
  auto cfg=settings.setpointSettings();cfg.preferences.mode[0]={SetpointRange::Selection::Manual,{36,48}};
  CHECK(SetpointControl::applySettings(cfg)==SetpointRange::Result::Ok);
  cmd.mode=SL2_MODE_HEAT;cmd.set_dc=290;
  CHECK(!h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP,&cmd));
  CHECK(!c.getEffectiveState().power);CHECK(!c._sendRequested);
 }
 CASE("auto_external_violation_corrected_before_handover") {
  auto cfg=settings.setpointSettings();cfg.preferences.mode[2]={SetpointRange::Selection::Manual,{40,48}};
  CHECK(SetpointControl::applySettings(cfg)==SetpointRange::Result::Ok);
  pump.target=188;ticks(c,300);SetpointControl::loop();AutoBand::loop(c);
  CHECK(settings.get().heatingThreshold==20&&settings.get().coolingThreshold==24);
  CHECK(!AutoBand::deviceOwns());
  ticks(c,300);SetpointControl::loop();AutoBand::loop(c);
  CHECK(settings.get().autoOwnerUnit);CHECK(settings.get().heatingThreshold==22&&settings.get().coolingThreshold==24);
 }

 CASE("accepted_band_edit_cancels_deferred_external_handover") {
  auto cfg=settings.setpointSettings();cfg.preferences.mode[2]={SetpointRange::Selection::Manual,{40,56}};
  CHECK(SetpointControl::applySettings(cfg)==SetpointRange::Result::Ok);
  pump.target=188;ticks(c,300);SetpointControl::loop();AutoBand::loop(c);CHECK(!AutoBand::deviceOwns());
  cmd.set_low_dc=220;cmd.set_high_dc=240;CHECK(h_apply(nullptr,SL2_CM_TEMP_BAND,&cmd));
  AutoBand::loop(c);CHECK(AutoBand::deviceOwns());ticks(c,300);SetpointControl::loop();AutoBand::loop(c);
  CHECK(AutoBand::deviceOwns());CHECK(settings.get().heatingThreshold==22&&settings.get().coolingThreshold==24);
 }

 CASE("stale_auto_tick_cannot_overwrite_new_heat_target") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  beforeAutoSteering=[&](){SetpointControl::Command user;user.unit.mode=CN105_MODE_HEAT;user.unit.targetTemp=20;CHECK(SetpointControl::submit(user)==SetpointRange::Result::Ok);};
  run_auto_sync(c);CHECK(!beforeAutoSteering);CHECK(c.getEffectiveState().mode==CN105_MODE_HEAT);CHECK(c.getEffectiveState().targetTemp==20);
 }
 CASE("stale_auto_tick_uses_newly_accepted_band") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  beforeAutoSteering=[&](){SetpointControl::Command user;user.heatingThreshold=23;user.coolingThreshold=25;CHECK(SetpointControl::submit(user)==SetpointRange::Result::Ok);};
  run_auto_sync(c);CHECK(!beforeAutoSteering);CHECK(c.getEffectiveState().mode==CN105_MODE_AUTO);CHECK(c.getEffectiveState().targetTemp==23);
  CHECK(settings.get().heatingThreshold==23&&settings.get().coolingThreshold==25);
 }
 CASE("stale_auto_tick_yields_to_new_external_observation") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  beforeAutoSteering=[&](){pump.target=166;ticks(c,300);};
  run_auto_sync(c);CHECK(!beforeAutoSteering);CHECK(c.getEffectiveState().targetTemp==19);CHECK(!c._wanted.hasTemp);CHECK(!AutoBand::deviceOwns());
  AutoBand::loop(c);CHECK(settings.get().autoOwnerUnit);
 }

 CASE("auto_queue_discards_decision_after_new_unit_observation") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  beforeAutoQueue=[&](){pump.target=166;ticks(c,300);};
  run_auto_sync(c);CHECK(!beforeAutoQueue);CHECK(c.getEffectiveState().targetTemp==19);CHECK(!c._wanted.hasTemp);CHECK(!c._sendRequested);
  AutoBand::loop(c);CHECK(settings.get().autoOwnerUnit);
 }
 CASE("stale_auto_tick_cannot_overwrite_new_cool_target") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  beforeAutoSteering=[&](){SetpointControl::Command user;user.unit.mode=CN105_MODE_COOL;user.unit.targetTemp=20;CHECK(SetpointControl::submit(user)==SetpointRange::Result::Ok);};
  run_auto_sync(c);CHECK(!beforeAutoSteering);CHECK(c.getEffectiveState().mode==CN105_MODE_COOL);CHECK(c.getEffectiveState().targetTemp==20);
 }

 CASE("auto_steering_waits_for_reconnect_settings_baseline") {
  settings.get().heatingThreshold=27;settings.get().coolingThreshold=29;
  c.restartHandshake();pump.responding=false;pump.packet(CN105_PKT_CONNECT_OK,0);c.loop();
  CHECK(c.isHealthy());CHECK(c.getState().encoding==SetpointRange::Encoding::Unknown);
  run_auto_sync(c);CHECK(!c._sendRequested);CHECK(!c._wanted.hasTemp);
 }

}
