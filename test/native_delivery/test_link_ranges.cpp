using namespace SetpointRange;
#define CASE(name) if (!strcmp(argv[1], name))

static void manual(int mode, Range range) {
 auto cfg=settings.setpointSettings();
 cfg.preferences.mode[mode]={Selection::Manual,range};
 CHECK(SetpointControl::applySettings(cfg)==Result::Ok);
}
static void automatic() {
 auto cfg=settings.setpointSettings();
 for(auto& mode:cfg.preferences.mode) mode.selection=Selection::Automatic;
 CHECK(SetpointControl::applySettings(cfg)==Result::Ok);
}
static sl2_caps_pkt caps() {
 sl2_caps_pkt packet{};
 CHECK(h_get_caps(nullptr,&packet));
 return packet;
}
static void checkCaps(int low,int high,int step,int table) {
 const auto packet=caps();
 CHECK(packet.set_min_dc==low);
 CHECK(packet.set_max_dc==high);
 CHECK(packet.set_step_dc==step);
 CHECK(packet.ftab_id==table);
 CHECK(packet.band_min_gap_dc==20);
}
static void linkTick() {
 now_ms+=50;
 EspnowLink link;
 link.loop();
}
static void announceInitial() {
 s_capsGen=settings.generation();
 caps_announce_if_changed();
 linkTick();
}
static uint8_t persistentSequence() {
 const auto& value=hkNvs.committed["serinlink"][SL2_KV_CAPS_SEQ];
 CHECK(value.size()==1);
 return value[0];
}
static std::vector<uint8_t> discovery(Range heat,Range cool,Range autoRange) {
 std::vector<uint8_t> response={0xFC,0x7B,1,0x30,0x10,0xC9,3,0,0x20,0,0x14,7,0x75,0x8C,0x25,0,0,0,0,0,0,0};
 response[15]=cool.min2+128;
 response[16]=cool.max2+128;
 response[17]=heat.min2+128;
 response[18]=heat.max2+128;
 response[19]=autoRange.min2+128;
 response[20]=autoRange.max2+128;
 response.back()=CN105Controller::calcChecksum(response.data(),response.size()-1);
 return response;
}
static void receiveDiscovery(CN105Controller& controller,Pump& pump,const std::vector<uint8_t>& response) {
 // The boot fixture has sent C9 and is still inside its response window.
 for(uint8_t byte:response) pump.rx.push_back(byte);
 controller.loop();
 CHECK(controller.rangeCapabilities().detected.validMask!=0);
}

int main(int,char** argv) {
 hkNvs.committed["ac-settings"]["deviceName"]={'O','l','d',0};
 settings.begin();
 Pump pump;
 CN105Controller controller;
 pump.mode=CN105_MODE_HEAT;
 now_ms=5000;
 controller.begin(&pump);
 ticks(controller);
 SetpointControl::begin(controller);
 s_ctrl=&controller;
 sl2_port_t port{};
 port.kv_set=p_kv_set;
 s_link.port=&port;

 CASE("enhanced_manual_envelope_and_linear_table") {
  for(int mode=0;mode<3;++mode) manual(mode,{20,62});
  checkCaps(100,310,5,0);
 }
 CASE("legacy_opted_in_precision_and_endpoints") {
  pump.legacy=true;
  ticks(controller,300);
  for(int mode=0;mode<3;++mode) manual(mode,{32,62});
  checkCaps(160,310,10,0);
  sl2_cmd_pkt command{};
  command.set_dc=305;
  CHECK(h_apply(nullptr,SL2_CM_TEMP,&command));
  ticks(controller,300);
  CHECK(pump.legacyC==31);
 }
 CASE("migrated_compatibility_keeps_native_table_including_legacy") {
  checkCaps(160,305,5,1);
  pump.legacy=true;
  ticks(controller,300);
  checkCaps(160,305,5,1);
  sl2_cmd_pkt command{};
  command.set_dc=305;
  CHECK(h_apply(nullptr,SL2_CM_TEMP,&command));
  ticks(controller,300);
  CHECK(pump.legacyC==30);
 }
 CASE("unknown_compatibility_keeps_prior_caps") {
  controller.restartHandshake();
  checkCaps(160,305,5,1);
 }
 CASE("discovery_refreshes_caps_without_settings_save") {
  automatic();
  announceInitial();
  const auto generation=settings.generation();
  const auto sequence=persistentSequence();
  receiveDiscovery(controller,pump,discovery({20,62},{32,62},{32,62}));
  linkTick();
  CHECK(settings.generation()==generation);
  CHECK(persistentSequence()==uint8_t(sequence+1));
  checkCaps(100,310,5,0);
 }
 CASE("sparse_discovery_preserves_undetected_compatibility_and_refreshes") {
  auto cfg=settings.setpointSettings();
  cfg.preferences.mode[0].selection=Selection::Automatic;
  CHECK(SetpointControl::applySettings(cfg)==Result::Ok);
  announceInitial();
  const auto sequence=persistentSequence();
  receiveDiscovery(controller,pump,discovery({20,62},{0,0},{0,0}));
  linkTick();
  CHECK(persistentSequence()==uint8_t(sequence+1));
  checkCaps(100,310,5,0);
  CHECK(SetpointControl::snapshot().mode[1].source==Source::Compatibility);
  CHECK(SetpointControl::snapshot().mode[2].source==Source::Compatibility);
 }
 CASE("provenance_mode_and_identical_saves_do_not_persist_sequence") {
  automatic();
  announceInitial();
  const auto sequence=persistentSequence();
  const auto writes=hkNvs.writes;
  const auto rangeGeneration=SetpointControl::snapshot().generation;
  // Fresh C9 reports the same fallback bounds: metadata is byte-for-byte identical.
  receiveDiscovery(controller,pump,discovery({32,61},{32,61},{32,61}));
  linkTick();
  CHECK(SetpointControl::snapshot().generation>rangeGeneration);
  CHECK(persistentSequence()==sequence);
  CHECK(hkNvs.writes==writes);
  auto packet=caps();
  pump.mode=CN105_MODE_COOL;
  pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);
  controller.loop();
  linkTick();
  auto after=caps();
  CHECK(!memcmp(&packet,&after,sizeof packet));
  CHECK(persistentSequence()==sequence);
  CHECK(hkNvs.writes==writes);
  settings.save();
  const auto afterSaveWrites=hkNvs.writes;
  linkTick();
  linkTick();
  CHECK(persistentSequence()==sequence);
  CHECK(hkNvs.writes==afterSaveWrites);
 }
 CASE("settings_refresh_persists_changed_packet_once") {
  announceInitial();
  const auto sequence=persistentSequence();
  settings.get().vaneConfig=0;
  settings.save();
  linkTick();
  CHECK(persistentSequence()==uint8_t(sequence+1));
  CHECK(caps().vane_v==0&&caps().vane_h==0);
  const auto writes=hkNvs.writes;
  linkTick();
  linkTick();
  CHECK(hkNvs.writes==writes);
 }
 CASE("single_boot_announces_changed_caps_once") {
  // Bonded dials learn the new mode from CAPS; no dial firmware change.
  announceInitial();
  const auto sequence=persistentSequence();
  hkNvs.committed["ac-settings"]["autoSingle"]={1};
  settings=SettingsStore{};
  settings.begin();
  SetpointControl::begin(controller);
  caps_announce_if_changed();
  linkTick();
  CHECK(persistentSequence()==uint8_t(sequence+1));
  CHECK(caps().modes&(1u<<SL2_MODE_AUTO));
  caps_announce_if_changed();
  linkTick();
  CHECK(persistentSequence()==uint8_t(sequence+1));
 }
 CASE("per_mode_bounds_reject_entire_batch_inside_common_envelope") {
  manual(0,{36,48});
  manual(1,{20,62});
  pump.power=false;
  pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);
  controller.loop();
  checkCaps(100,310,5,0);
  const auto generation=settings.generation();
  sl2_cmd_pkt command{};
  command.mode=SL2_MODE_HEAT;
  command.set_dc=290;
  command.fan=100;
  command.use_f=1;
  CHECK(!h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP|SL2_CM_FAN|SL2_CM_UNITS,&command));
  CHECK(!controller.getEffectiveState().power);
  CHECK(!controller._sendRequested);
  CHECK(!settings.get().useFahrenheit);
  CHECK(settings.generation()==generation);
  command.mode=SL2_MODE_COOL;
  CHECK(h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP,&command));
  CHECK(controller.getEffectiveState().mode==CN105_MODE_COOL);
  CHECK(controller.getEffectiveState().targetTemp==29);
 }
 CASE("compatible_per_mode_upgrade_keeps_native_table") {
  manual(0,{36,48});
  checkCaps(160,305,5,1);
  pump.legacy=true;
  ticks(controller,300);
  sl2_cmd_pkt command{};
  command.set_dc=235;
  CHECK(h_apply(nullptr,SL2_CM_TEMP,&command));
  ticks(controller,300);
  CHECK(pump.legacyC==24);
  checkCaps(160,305,5,1);
 }
 CASE("factory_reset_blocks_commands_and_link_storage") {
  announceInitial();
  const auto sequence=persistentSequence();
  SettingsStore::beginFactoryReset();
  sl2_cmd_pkt command{};
  command.mode=SL2_MODE_COOL;
  command.set_dc=240;
  command.use_f=1;
  CHECK(!h_apply(nullptr,SL2_CM_MODE|SL2_CM_TEMP|SL2_CM_UNITS,&command));
  CHECK(controller.getEffectiveState().mode==CN105_MODE_HEAT);
  CHECK(!controller._sendRequested);
  CHECK(!settings.get().useFahrenheit);
  const auto writes=hkNvs.writes;
  settings.get().vaneConfig=0;
  settings.save();
  linkTick();
  CHECK(persistentSequence()==sequence);
  CHECK(hkNvs.writes==writes);
 }
 CASE("room_measurement_is_not_clamped_to_setpoint_range") {
  manual(0,{40,48});
  pump.roomRaw=138;
  pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_ROOMTEMP);
  controller.loop();
  sl2_hvac_state_t state{};
  CHECK(h_get_state(nullptr,&state));
  CHECK(state.room_dc==50);
 }
 CASE("discovery_and_encoding_use_effective_auto_band") {
  // Fresh discovery changes the usable band without rewriting saved intent.
  auto cfg = settings.setpointSettings();
  cfg.preferences.mode[2].selection = Selection::Automatic;
  CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
  controller._state.mode = CN105_MODE_AUTO;
  controller._state.power = true;
  uint8_t data[16] = {};
  data[0] = 0xC9;
  data[8] = 4;
  data[14] = 176;
  data[15] = 184; // Auto 24..28 C.
  controller.handleIdentifyResponse(data, sizeof data);
  SetpointControl::loop();
  float heat = 24, cool = 26;
  sl2_hvac_state_t state{};
  CHECK(h_get_state(nullptr, &state));
  CHECK(state.set_low_dc == heat * 10);
  CHECK(state.set_high_dc == cool * 10);
  CHECK(settings.setpointSettings().band.heat2 == 40);
  CHECK(settings.setpointSettings().band.cool2 == 50);
  controller._state.roomTemp = 28;
  SetpointControl::steerAuto();
  CHECK(controller.getEffectiveState().targetTemp == cool);
 }
 CASE("legacy_refresh_uses_effective_auto_band") {
  // A half-degree saved band survives reconnect; operational thresholds use whole C.
  auto cfg = settings.setpointSettings();
  cfg.preferences.mode[2].selection = Selection::Automatic;
  cfg.band = {45, 49};
  CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
  pump.mode = CN105_MODE_AUTO;
  controller.restartHandshake();
  pump.legacy = true;
  now_ms += 4000;
  ticks(controller, 300);
  SetpointControl::loop();
  float heat = 22, cool = 24;
  sl2_hvac_state_t state{};
  CHECK(h_get_state(nullptr, &state));
  CHECK(state.set_low_dc == heat * 10);
  CHECK(state.set_high_dc == cool * 10);
  CHECK(settings.setpointSettings().band.heat2 == 45);
  CHECK(settings.setpointSettings().band.cool2 == 49);
  controller._state.roomTemp = 20;
  SetpointControl::steerAuto();
  CHECK(controller.getEffectiveState().targetTemp == heat);
 }

}
