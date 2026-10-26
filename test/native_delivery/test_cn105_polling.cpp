#define CASE(name) if (!strcmp(argv[1], name))
int main(int, char** argv) {
 Pump pump; CN105Controller c;
 CASE("poll_after_rollover") {
  now_ms=UINT32_MAX-1000;c.begin(&pump);ticks(c);
  CHECK(pump.infos==5);int before=pump.infos;
  now_ms=1000;ticks(c,640,100);
  CHECK(pump.infos>=before+100);CHECK(pump.connects==1);CHECK(c.isHealthy());
 }
 CASE("receive_time_does_not_restart_poll_early") {
  now_ms=5000;pump.readCostMs=1;c.begin(&pump);ticks(c);
  CHECK(pump.infos==5);ticks(c,20,10);CHECK(pump.infos==5);
 }
 CASE("set_deferral_across_rollover") {
  now_ms=UINT32_MAX-3000;c.begin(&pump);ticks(c);
  now_ms=UINT32_MAX-500;  // Poll due; SET wins, then requires a quiet period.
  c.setTargetTemp(24);c.sendPendingChanges();c.loop();int before=pump.infos;
  now_ms+=CN105_DEFER_DELAY-1;c.loop();CHECK(pump.infos==before);
  ++now_ms;c.loop();CHECK(pump.infos==before+1);ticks(c);CHECK(pump.infos==before+5);
 }
 CASE("remote_deferral_across_rollover") {
  now_ms=UINT32_MAX-3000;c.begin(&pump);ticks(c);
  now_ms=UINT32_MAX-500;
  c.sendRemoteTemperature(19);c.loop();int before=pump.infos;
  now_ms+=CN105_DEFER_DELAY-1;c.loop();CHECK(pump.infos==before);
  ++now_ms;c.loop();CHECK(pump.infos==before+1);
 }
 CASE("reconnect_polls_immediately") {
  now_ms=UINT32_MAX-1000;c.begin(&pump);ticks(c);
  pump.responding=false;now_ms+=15000;ticks(c);CHECK(!c.isHealthy());
  pump.responding=true;now_ms+=4000;int before=pump.infos;ticks(c);
  CHECK(c.isHealthy());CHECK(pump.infos>=before+5);
 }
 CASE("remote_written_since_boot_latches_on_first_send") {
  // main.cpp persists "this device has fed the pump" off this flag; a bare
  // boot, polling included, must not trip it.
  now_ms=5000;c.begin(&pump);ticks(c,100,100);CHECK(!c.remoteWrittenSinceBoot());
  c.sendRemoteTemperature(21);CHECK(c.remoteWrittenSinceBoot());
  ticks(c,100,100);CHECK(c.remoteWrittenSinceBoot());
 }
 CASE("first_connect_no_clear_unless_enabled") {
  // Default: a device that was never told it fed the pump leaves the
  // register alone — it may belong to a wall controller, not to us.
  now_ms=5000;c.begin(&pump);ticks(c,100,100);
  CHECK(c.isHealthy());CHECK(pump.remoteClears==0);CHECK(pump.remoteSets==0);
 }
 CASE("first_connect_clears_when_enabled_and_no_source_feeds") {
  now_ms=5000;c.enableBootClear(true);c.begin(&pump);ticks(c);
  CHECK(pump.infos==5);            // the first poll still runs straight after the handshake
  CHECK(pump.remoteClears==1);CHECK(pump.remoteSets==0);
  ticks(c,100,100);CHECK(pump.remoteClears==1);
  // Drain the reply already queued before simulating a 15 s silent link.
  pump.responding=false;ticks(c);now_ms+=15000;ticks(c);CHECK(!c.isHealthy());
  pump.responding=true;now_ms+=4000;ticks(c,100,100);
  CHECK(c.isHealthy());CHECK(pump.remoteClears==1);   // first connect after boot only
 }
 CASE("first_connect_no_clear_when_source_feeds") {
  now_ms=5000;c.enableBootClear(true);c.sendRemoteTemperature(21);c.begin(&pump);ticks(c,100,100);
  CHECK(pump.remoteSets==1);CHECK(pump.remoteClears==0);
 }
 CASE("first_connect_no_clear_when_source_starts_during_first_poll") {
  now_ms=5000;c.enableBootClear(true);c.begin(&pump);c.loop();now_ms+=10;c.loop();CHECK(c.isConnected());
  c.sendRemoteTemperature(21);ticks(c,100,100);
  CHECK(pump.remoteSets==1);CHECK(pump.remoteClears==0);
 }
 CASE("enhanced_to_legacy_reconnect_changes_wire_encoding") {
  now_ms=5000;c.begin(&pump);ticks(c);CHECK(c.quantizeSetpoint(22.5f)==22.5f);
  pump.legacy=true;now_ms+=4000;c.begin(&pump);ticks(c);
  CHECK(c.quantizeSetpoint(22.5f)==22);
  c.setTargetTemp(23.5f);c.sendPendingChanges();ticks(c);
  CHECK(pump.legacyC==23);CHECK(pump.sent.back()[19]==0);
 }
 // External Auto changes: a wall controller or IR remote writing the Auto
 // setpoint, or putting the unit into Auto, is a SETTINGS change this
 // controller did not command. Pump SETTINGS: target 172 = 22.0, 166 = 19.0.
 CASE("external_setpoint_change_counted_after_baseline") {
  now_ms=5000;c.begin(&pump);ticks(c);CHECK(c.externalAutoChanges()==0);
  pump.target=166;ticks(c,300,10);CHECK(c.externalAutoChanges()==1);
  ticks(c,300,10);CHECK(c.externalAutoChanges()==1);
 }
 CASE("own_set_not_counted") {
  now_ms=5000;c.begin(&pump);ticks(c);
  c.setTargetTemp(24);c.sendPendingChanges();ticks(c,300,10);
  CHECK(pump.target==176);CHECK(c.getState().targetTemp==24);CHECK(c.externalAutoChanges()==0);
 }
 CASE("own_set_answered_with_other_value_not_counted") {
  now_ms=5000;c.begin(&pump);ticks(c);
  c.setTargetTemp(24);c.sendPendingChanges();c.loop();CHECK(pump.sets==1);
  pump.target=170;   // the unit clamps our 24 to 21 while our SET is in flight
  ticks(c,300,10);CHECK(c.getState().targetTemp==21);CHECK(c.externalAutoChanges()==0);
 }
 CASE("change_before_our_send_goes_out_is_external") {
  now_ms=5000;c.begin(&pump);ticks(c);
  pump.target=166;now_ms+=CN105_UPDATE_INTERVAL;c.loop();   // SETTINGS requested, answer queued
  c.setTargetTemp(24);c.sendPendingChanges();ticks(c,300,10); // staged, not yet sent, when it lands
  CHECK(c.externalAutoChanges()==1);CHECK(pump.target==176);
 }
 CASE("external_auto_entry_counted") {
  pump.mode=CN105_MODE_HEAT;now_ms=5000;c.begin(&pump);ticks(c);CHECK(c.externalAutoChanges()==0);
  pump.mode=CN105_MODE_AUTO;ticks(c,300,10);CHECK(c.externalAutoChanges()==1);
 }
 CASE("own_auto_entry_not_counted") {
  pump.mode=CN105_MODE_HEAT;now_ms=5000;c.begin(&pump);ticks(c);
  c.setMode(CN105_MODE_AUTO);c.sendPendingChanges();ticks(c,300,10);
  CHECK(c.getState().mode==CN105_MODE_AUTO);CHECK(c.externalAutoChanges()==0);
 }
 CASE("external_power_on_in_auto_counted") {
  pump.power=false;now_ms=5000;c.begin(&pump);ticks(c);CHECK(c.externalAutoChanges()==0);
  pump.power=true;ticks(c,300,10);CHECK(c.externalAutoChanges()==1);
 }
 CASE("first_response_after_reconnect_is_baseline") {
  now_ms=5000;c.begin(&pump);ticks(c);
  pump.responding=false;now_ms+=15000;ticks(c);CHECK(!c.isHealthy());
  pump.target=166;pump.responding=true;now_ms+=4000;ticks(c,300,10);
  CHECK(c.isHealthy());CHECK(c.getState().targetTemp==19);CHECK(c.externalAutoChanges()==0);
 }
 // A fan command staged while the confirmation of our own setpoint or Auto
 // entry is on the wire must not make that change look like somebody else's.
 CASE("own_set_with_fan_staged_mid_poll_not_counted") {
  now_ms=5000;c.begin(&pump);ticks(c);
  c.setTargetTemp(24);c.sendPendingChanges();c.loop();CHECK(pump.sets==1);
  int infos=pump.infos;while(pump.infos==infos){now_ms+=10;c.loop();}   // SETTINGS requested, answer queued
  c.setFanSpeed(CN105_FAN_2);c.sendPendingChanges();ticks(c,300,10);
  CHECK(pump.target==176);CHECK(pump.sets==2);CHECK(c.externalAutoChanges()==0);
 }
 CASE("own_auto_entry_with_fan_staged_mid_poll_not_counted") {
  pump.mode=CN105_MODE_HEAT;now_ms=5000;c.begin(&pump);ticks(c);
  c.setMode(CN105_MODE_AUTO);c.sendPendingChanges();c.loop();CHECK(pump.sets==1);
  int infos=pump.infos;while(pump.infos==infos){now_ms+=10;c.loop();}
  c.setFanSpeed(CN105_FAN_2);c.sendPendingChanges();ticks(c,300,10);
  CHECK(pump.mode==CN105_MODE_AUTO);CHECK(pump.sets==2);CHECK(c.externalAutoChanges()==0);
 }
 // The unit reporting our own setpoint confirms it even with another field
 // staged — so a matching answer can never later read as a refusal.
 CASE("own_set_confirmed_with_fan_staged_mid_poll") {
  now_ms=5000;c.begin(&pump);ticks(c);
  c.setTargetTemp(24);c.sendPendingChanges();c.loop();
  int infos=pump.infos;while(pump.infos==infos){now_ms+=10;c.loop();}
  c.setFanSpeed(CN105_FAN_2);c.loop();   // reads the SETTINGS answer; the fan is still staged
  CHECK(c.getState().targetTemp==24);CHECK(!c._wanted.hasTemp);CHECK(!c.refusedSetpoint(24));
 }
 CASE("unconfirmed_setpoint_not_resurrected_by_later_command") {
  // Heat mode, the unit holds 28.0 when asked for 30.0. Once the grace window
  // has passed, a later fan command must not bring the refused 30.0 back.
  pump.mode=CN105_MODE_HEAT;now_ms=5000;c.begin(&pump);ticks(c);
  c.setTargetTemp(30);c.sendPendingChanges();
  for(int i=0;i<1500;i++){c.loop();if(pump.target>184)pump.target=184;now_ms+=10;}
  CHECK(c.getEffectiveState().targetTemp==28);
  c.setFanSpeed(CN105_FAN_2);c.sendPendingChanges();
  for(int i=0;i<1000;i++){c.loop();CHECK(c.getEffectiveState().targetTemp==28);now_ms+=10;}
 }
 CASE("first_answer_after_reconnect_never_latches") {
  now_ms=5000;c.begin(&pump);ticks(c);
  c.setTargetTemp(18);c.sendPendingChanges();ticks(c,5,10);pump.target=166;   // unit holds 19.0
  pump.responding=false;now_ms+=15000;ticks(c);CHECK(!c.isHealthy());pump.responding=true;now_ms+=4000;ticks(c,300,10);
  CHECK(c.isHealthy());CHECK(!c._wanted.hasTemp);CHECK(!c.refusedSetpoint(18));
 }
 CASE("external_keepalive_does_not_starve_long_interval_polls") {
  now_ms=5000;c.setUpdateInterval(30000);c.begin(&pump);ticks(c);
  int before=pump.infos;CHECK(c.getState().roomTemp==21);
  for(int i=0;i<1200;i++){
   if(i%200==0)c.sendRemoteTemperature(27);
   c.loop();now_ms+=100;
  }
  CHECK(pump.infos>=before+15);CHECK(c.getState().roomTemp==27);CHECK(c.isHealthy());
 }
 CASE("frequent_set_commands_do_not_starve_long_interval_polls") {
  now_ms=5000;c.setUpdateInterval(30000);c.begin(&pump);ticks(c);
  int before=pump.infos;
  for(int i=0;i<1200;i++){
   if(i%200==0){c.setTargetTemp(24);c.sendPendingChanges();}
   c.loop();now_ms+=100;
  }
  CHECK(pump.infos>=before+15);CHECK(c.getState().targetTemp==24);CHECK(c.isHealthy());
 }
}
