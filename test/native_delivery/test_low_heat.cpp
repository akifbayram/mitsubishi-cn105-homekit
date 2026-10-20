// Low heat: below the unit's 16 C floor the unit is held at 16 C and fed a
// room temperature raised by the difference, while every reader sees the real
// room and the real target.
class LowHeatPump : public Pump {
public:
 int lastRemote=-1;        // enhanced byte of the last remote temp, 0x80 = cleared
 int lastSetTarget=-1;     // enhanced byte of the last SET setpoint
 void write(const uint8_t* b,size_t n) override {
  if(b[1]==CN105_PKT_SET&&b[5]==0x07)lastRemote=b[8];
  if(b[1]==CN105_PKT_SET&&b[5]==1&&(b[6]&CN105_FLAG_TEMP))lastSetTarget=b[19];
  Pump::write(b,n);
 }
};
static int enc(float c){return (int)(c*2.0f+128.0f);}
static void run(CN105Controller& c,uint32_t ms){ticks(c,(int)(ms/10));}

// Connected, enhanced encoding learned, unit in HEAT at 16 C, room 20 C.
static void heating(CN105Controller& c,LowHeatPump& pump,bool enabled=true) {
 pump.mode=CN105_MODE_HEAT;pump.target=enc(16);pump.roomRaw=enc(20);
 c.setLowHeatEnabled(enabled);
 now_ms=5000;c.begin(&pump);run(c,3000);
 CHECK(c.isConnected());CHECK(c._tempMode);
}

#define CASE(name) if (!strcmp(argv[1], name))
int main(int,char** argv) {
 CASE("disabled_keeps_floor") {
  LowHeatPump pump;CN105Controller c;heating(c,pump,false);
  c.sendRemoteTemperature(12.0f);run(c,1000);
  CHECK(pump.lastRemote==enc(12));
  c.setTargetTemp(12.0f);c.sendPendingChanges();run(c,3000);
  CHECK(pump.lastSetTarget==enc(16));CHECK(std::isnan(c.lowHeatTarget()));
  CHECK(pump.lastRemote==enc(12));   // no shift
  CHECK(c.getEffectiveState().targetTemp==16.0f);
  CHECK(c.minTargetTemp()==CN105_TEMP_MIN);
 }
 CASE("shifts_feed_reports_real") {
  LowHeatPump pump;CN105Controller c;heating(c,pump);
  c.sendRemoteTemperature(12.3f);run(c,1000);
  CHECK(pump.lastRemote==enc(12.5f));
  c.setTargetTemp(12.8f);c.sendPendingChanges();run(c,1000);
  CHECK(pump.lastSetTarget==enc(16));       // the unit is held at its floor
  CHECK(c.lowHeatTarget()==13.0f);          // 0.5 C grid
  CHECK(pump.lastRemote==enc(15.5f));       // 12.3 + 3.0, resent without a keepalive
  run(c,5000);                              // past the grace window: the unit's own reports
  const CN105State e=c.getEffectiveState(),r=c.getState();
  CHECK(e.targetTemp==13.0f);CHECK(r.targetTemp==13.0f);
  CHECK(e.roomTemp==12.5f);CHECK(r.roomTemp==12.5f);
  // A new reading rides the same shift.
  c.sendRemoteTemperature(11.0f);run(c,1000);CHECK(pump.lastRemote==enc(14.0f));
  CHECK(c.getEffectiveState().roomTemp==11.0f);
  // Back above the floor: target and feed are both plain again.
  c.setTargetTemp(18.0f);c.sendPendingChanges();run(c,1000);
  CHECK(std::isnan(c.lowHeatTarget()));CHECK(pump.lastSetTarget==enc(18));
  CHECK(pump.lastRemote==enc(11.0f));
 }
 CASE("unfed_reports_floor") {
  LowHeatPump pump;CN105Controller c;heating(c,pump);
  c.setTargetTemp(10.0f);c.sendPendingChanges();run(c,5000);
  CHECK(c.lowHeatTarget()==10.0f);
  CHECK(c.getEffectiveState().targetTemp==16.0f);   // what the unit really holds
  CHECK(c.getEffectiveState().roomTemp==20.0f);     // its own thermistor
  // A source arriving later picks the target up.
  c.sendRemoteTemperature(9.0f);run(c,1000);
  CHECK(pump.lastRemote==enc(15.0f));
  CHECK(c.getEffectiveState().targetTemp==10.0f);
  // The source going away hands the unit back unshifted.
  c.clearRemoteTemperature();run(c,1000);
  CHECK(pump.lastRemote==0x80);CHECK(c.getEffectiveState().targetTemp==16.0f);
 }
 CASE("floor_clamp") {
  LowHeatPump pump;CN105Controller c;heating(c,pump);
  c.sendRemoteTemperature(8.0f);
  c.setTargetTemp(4.0f);c.sendPendingChanges();run(c,1000);
  CHECK(c.lowHeatTarget()==CN105_LOW_HEAT_MIN);CHECK(pump.lastRemote==enc(14.0f));
  c.setTargetTemp(15.9f);c.sendPendingChanges();run(c,1000);   // rounds to the floor itself
  CHECK(std::isnan(c.lowHeatTarget()));CHECK(pump.lastRemote==enc(8.0f));
 }
 CASE("mode_change_drops_shift") {
  LowHeatPump pump;CN105Controller c;heating(c,pump);
  c.sendRemoteTemperature(12.0f);c.setTargetTemp(13.0f);c.sendPendingChanges();run(c,1000);
  CHECK(pump.lastRemote==enc(15.0f));
  c.setMode(CN105_MODE_COOL);c.sendPendingChanges();run(c,1000);
  CHECK(pump.lastRemote==enc(12.0f));                    // no shift outside HEAT
  run(c,5000);
  CHECK(c.getEffectiveState().targetTemp==16.0f);        // and no low target shown
  CHECK(c.lowHeatTarget()==13.0f);                       // but kept for HEAT
  c.setMode(CN105_MODE_HEAT);c.sendPendingChanges();run(c,1000);
  CHECK(pump.lastRemote==enc(15.0f));CHECK(c.getEffectiveState().targetTemp==13.0f);
 }
 CASE("remote_override_drops_target") {
  LowHeatPump pump;CN105Controller c;heating(c,pump);
  c.sendRemoteTemperature(12.0f);c.setTargetTemp(13.0f);c.sendPendingChanges();run(c,5000);
  CHECK(c.lowHeatTarget()==13.0f);
  pump.target=enc(20);run(c,3000);                       // the IR remote set 20 C
  CHECK(std::isnan(c.lowHeatTarget()));
  CHECK(pump.lastRemote==enc(12.0f));
  CHECK(c.getEffectiveState().targetTemp==20.0f);
 }
 CASE("pending_set_keeps_target") {
  // A SETTINGS report from before our SET lands must not drop the target.
  LowHeatPump pump;pump.target=enc(20);CN105Controller c;
  pump.mode=CN105_MODE_HEAT;pump.roomRaw=enc(20);c.setLowHeatEnabled(true);
  now_ms=5000;c.begin(&pump);run(c,3000);
  c.setTargetTemp(13.0f);c.sendPendingChanges();
  c._wanted.hasBeenSent=true;   // as if transmitted, not yet applied by the unit
  const uint8_t settingsResp[12]={CN105_INFO_SETTINGS,0,0,1,CN105_MODE_HEAT,0,0,0,0,0,0,(uint8_t)enc(20)};
  c.handleInfoResponse(settingsResp,sizeof settingsResp);
  CHECK(c.lowHeatTarget()==13.0f);
  now_ms+=CN105_GRACE_SAFETY_MS;   // our SET never landed
  c.handleInfoResponse(settingsResp,sizeof settingsResp);
  CHECK(std::isnan(c.lowHeatTarget()));
 }
 CASE("disable_clears") {
  LowHeatPump pump;CN105Controller c;heating(c,pump);
  c.sendRemoteTemperature(12.0f);c.setTargetTemp(13.0f);c.sendPendingChanges();run(c,1000);
  c.setLowHeatEnabled(false);run(c,1000);
  CHECK(std::isnan(c.lowHeatTarget()));CHECK(pump.lastRemote==enc(12.0f));
  CHECK(c.minTargetTemp()==CN105_TEMP_MIN);
 }
 CASE("restore") {
  LowHeatPump pump;CN105Controller c;
  c.restoreLowHeatTarget(13.0f);CHECK(std::isnan(c.lowHeatTarget()));   // off: ignored
  c.setLowHeatEnabled(true);
  c.restoreLowHeatTarget(17.0f);CHECK(std::isnan(c.lowHeatTarget()));   // not a low target
  c.restoreLowHeatTarget(NAN);CHECK(std::isnan(c.lowHeatTarget()));
  c.restoreLowHeatTarget(13.0f);CHECK(c.lowHeatTarget()==13.0f);
  pump.mode=CN105_MODE_HEAT;pump.target=enc(16);pump.roomRaw=enc(20);
  now_ms=5000;c.begin(&pump);run(c,3000);
  c.sendRemoteTemperature(12.0f);run(c,1000);
  CHECK(pump.lastRemote==enc(15.0f));CHECK(c.getEffectiveState().targetTemp==13.0f);
 }
 return 0;
}
