class RecordingPump : public Pump {
public:
 std::vector<std::vector<uint8_t>> sent;
 void write(const uint8_t* b,size_t n) override {
  if(b[1]==CN105_PKT_SET)sent.emplace_back(b,b+n);
  Pump::write(b,n);
 }
};

// Capture mailbox bytes as well as observable state: an ignored input must
// neither create work nor overwrite a finite command or an explicit clear.
struct Snapshot {
 unsigned char staged[sizeof(CN105Controller::PendingCommand)];
 unsigned char wanted[sizeof(WantedSettings)];
 unsigned char state[sizeof(CN105State)];
 unsigned char remoteTemp[sizeof(float)];
 bool sendRequested,pendingRemote,remoteWritten;
 explicit Snapshot(const CN105Controller& c)
  :sendRequested(c._sendRequested),pendingRemote(c._pendingRemoteTemp),remoteWritten(c._remoteWritten) {
  memcpy(staged,&c._staged,sizeof(staged));
  memcpy(wanted,&c._wanted,sizeof(wanted));
  memcpy(state,&c._state,sizeof(state));
  memcpy(remoteTemp,&c._pendingRemoteTempC,sizeof(remoteTemp));
 }
 void unchanged(const CN105Controller& c) const {
  CHECK(!memcmp(staged,&c._staged,sizeof(staged)));
  CHECK(!memcmp(wanted,&c._wanted,sizeof(wanted)));
  CHECK(!memcmp(state,&c._state,sizeof(state)));
  CHECK(!memcmp(remoteTemp,&c._pendingRemoteTempC,sizeof(remoteTemp)));
  CHECK(sendRequested==c._sendRequested);
  CHECK(pendingRemote==c._pendingRemoteTemp);
  CHECK(remoteWritten==c._remoteWritten);
 }
};

// These cases exercise the boot clear's interplay with bad input, so the
// controller is told it may clear (production: a device that has fed before).
static void connect(CN105Controller& c,RecordingPump& pump) {
 now_ms=5000;c.enableBootClear(true);c.begin(&pump);ticks(c,20);
 CHECK(c.isConnected());CHECK(pump.remoteClears==1);CHECK(!c._cycleRunning);
}

static void target_ignored(float invalid,bool pending) {
 for(bool committed:{false,true}) {
  RecordingPump pump;CN105Controller c;connect(c,pump);
  if(pending) {
   c.setTargetTemp(22.5f);c.setFanSpeed(2);c.setWideVane(3);
   if(committed)c.sendPendingChanges();
  } else {
   c.setTargetTemp(22);c.sendPendingChanges();ticks(c,20);
   CHECK(c._wanted.hasBeenSent);CHECK(!c._sendRequested);
  }
  const float effective=c.getEffectiveState().targetTemp;
  const Snapshot before(c);const size_t sent=pump.sent.size();
  ++now_ms;c.setTargetTemp(invalid);
  before.unchanged(c);CHECK(c.getEffectiveState().targetTemp==effective);
  CHECK(pump.sent.size()==sent);
  c.sendPendingChanges();ticks(c,20);
  CHECK(pump.sent.size()==sent+(pending?1:0));
  if(pending) {
   const auto& pkt=pump.sent.back();
   CHECK(pkt[5]==1);CHECK(pkt[6]==(CN105_FLAG_TEMP|CN105_FLAG_FAN));
   CHECK(pkt[7]==CN105_FLAG2_WVANE);CHECK(pkt[19]==173);
   CHECK(pkt[11]==2);CHECK(pkt[18]==3);
  }
 }
}

static void remote_ignored(float invalid,int pending) {
 RecordingPump pump;CN105Controller c;
 if(pending) {
  connect(c,pump);
  if(pending==1)c.sendRemoteTemperature(21.25f);
  else c.clearRemoteTemperature();
 }
 const Snapshot before(c);const size_t sent=pump.sent.size();
 ++now_ms;c.sendRemoteTemperature(invalid);
 before.unchanged(c);CHECK(pump.sent.size()==sent);
 if(!pending) {
  // Invalid input before connection must not suppress the boot clear.
  connect(c,pump);CHECK(pump.remoteSets==0);
  const Snapshot connected(c);const size_t bootSent=pump.sent.size();
  c.sendRemoteTemperature(invalid);connected.unchanged(c);ticks(c,20);
  CHECK(pump.sent.size()==bootSent);
 } else {
  ticks(c,20);CHECK(pump.sent.size()==sent+1);
  const auto& pkt=pump.sent.back();CHECK(pkt[5]==7);
  if(pending==1) {
   CHECK(pkt[6]==1);CHECK(pkt[7]==26);CHECK(pkt[8]==171);
   CHECK(pump.remoteSets==1);CHECK(pump.remoteClears==1);
  } else {
   CHECK(pkt[6]==0);CHECK(pkt[8]==128);
   CHECK(pump.remoteSets==0);CHECK(pump.remoteClears==2);
  }
 }
}

#define CASE(name) if (!strcmp(argv[1], name))
int main(int,char** argv) {
 CASE("target_nan_clean") {target_ignored(NAN,false);}
 CASE("target_pos_inf_clean") {target_ignored(INFINITY,false);}
 CASE("target_neg_inf_clean") {target_ignored(-INFINITY,false);}
 CASE("target_nan_pending") {target_ignored(NAN,true);}
 CASE("target_pos_inf_pending") {target_ignored(INFINITY,true);}
 CASE("target_neg_inf_pending") {target_ignored(-INFINITY,true);}
 CASE("remote_nan_clean") {remote_ignored(NAN,0);}
 CASE("remote_pos_inf_clean") {remote_ignored(INFINITY,0);}
 CASE("remote_neg_inf_clean") {remote_ignored(-INFINITY,0);}
 CASE("remote_nan_pending") {remote_ignored(NAN,1);}
 CASE("remote_pos_inf_pending") {remote_ignored(INFINITY,1);}
 CASE("remote_neg_inf_pending") {remote_ignored(-INFINITY,1);}
 CASE("remote_nan_preserves_clear") {remote_ignored(NAN,2);}
 CASE("remote_pos_inf_preserves_clear") {remote_ignored(INFINITY,2);}
 CASE("remote_neg_inf_preserves_clear") {remote_ignored(-INFINITY,2);}
 CASE("finite_target_clamp_and_round") {
  const float inputs[]={-1,22.25f,100};
  const uint8_t encoded[]={160,173,189};
  for(int i=0;i<3;++i) {
   RecordingPump pump;CN105Controller c;connect(c,pump);
   const size_t sent=pump.sent.size();c.setTargetTemp(inputs[i]);
   CHECK(c._wanted.hasTemp);CHECK(!c._wanted.hasBeenSent);
   CHECK(pump.sent.size()==sent);c.sendPendingChanges();ticks(c,20);
   CHECK(pump.sent.size()==sent+1);CHECK(pump.sent.back()[19]==encoded[i]);
  }
 }
 CASE("finite_remote_clamp_and_round") {
  const float inputs[]={-1,21.25f,100};
  const uint8_t encoded[]={129,171,255};
  for(int i=0;i<3;++i) {
   RecordingPump pump;CN105Controller c;connect(c,pump);
   const size_t sent=pump.sent.size();c.sendRemoteTemperature(inputs[i]);
   CHECK(c._pendingRemoteTemp);CHECK(c._remoteWritten);
   CHECK(pump.sent.size()==sent);ticks(c,20);
   CHECK(pump.sent.size()==sent+1);CHECK(pump.sent.back()[6]==1);
   CHECK(pump.sent.back()[8]==encoded[i]);
  }
 }
 CASE("legacy_target_keeps_compatibility_clamp_round_truncation") {
  const float inputs[]={-1,22.25f,30.5f,100};const int expected[]={16,22,30,30};
  for(int i=0;i<4;++i) {
   RecordingPump pump;CN105Controller c;pump.legacy=true;connect(c,pump);
   CHECK(c.getState().encoding==SetpointRange::Encoding::Legacy);
   CHECK(c.quantizeSetpoint(inputs[i])==expected[i]);
   c.setTargetTemp(inputs[i]);c.sendPendingChanges();ticks(c,20);
   CHECK(pump.legacyC==expected[i]);CHECK(pump.sent.back()[10]==31-expected[i]);CHECK(pump.sent.back()[19]==0);
  }
 }
}
