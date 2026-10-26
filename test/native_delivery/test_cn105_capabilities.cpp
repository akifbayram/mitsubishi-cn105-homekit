// Published examples: https://muart-group.github.io/developer/it-protocol/0x7B-identify-response/0xC9-base-capabilities
static const std::vector<uint8_t> GS12NA = {
 0xFC,0x7B,0x01,0x30,0x10,0xC9,0x03,0x00,0x20,0x00,0x14,0x07,0x75,0x8C,0x25,0xA0,0xBE,0x94,0xBE,0xA0,0xBE,0x09};
static const std::vector<uint8_t> SVZ = {
 0xFC,0x7B,0x01,0x30,0x10,0xC9,0x03,0x00,0x20,0x00,0x0A,0x07,0x05,0xE4,0x25,0xA6,0xBC,0x94,0xB8,0xA6,0xB8,0x2D};
static void inject(Pump& pump,std::vector<uint8_t> p) {
 p.back()=CN105Controller::calcChecksum(p.data(),p.size()-1);
 for(uint8_t b:p)pump.rx.push_back(b);
}
static void boot(CN105Controller& c,Pump& pump) {c.begin(&pump);ticks(c);}
static void hold(CN105Controller& c,Pump& pump,uint8_t target=170) {
 for(int i=0;i<1500;++i){c.loop();pump.target=target;now_ms+=10;}
}
static void checkRanges(const CN105Controller& c,int coolMin,int coolMax,int autoMax) {
 const auto caps=c.rangeCapabilities();CHECK(caps.detected.validMask==7);CHECK(caps.detected.freshMask==7);
 CHECK(caps.detected.mode[0].min2==20);CHECK(caps.detected.mode[0].max2==autoMax);
 CHECK(caps.detected.mode[1].min2==coolMin);CHECK(caps.detected.mode[1].max2==coolMax);
 CHECK(caps.detected.mode[2].min2==coolMin);CHECK(caps.detected.mode[2].max2==autoMax);
}
#define CASE(name) if (!strcmp(argv[1], name))
int main(int,char** argv) {
 Pump pump;CN105Controller c;now_ms=5000;
 CASE("one_identify_after_settings_baseline") {
  pump.identifyReply=GS12NA;c.begin(&pump);ticks(c);
  CHECK(pump.identifies==1);
  const std::vector<uint8_t> request={0xFC,0x5B,0x01,0x30,0x01,0xC9,0xAA};
  CHECK(pump.sent[6]==request);CHECK(pump.infos==5);
  ticks(c,1000);CHECK(pump.identifies==1);checkRanges(c,32,62,62);
 }
 CASE("svz_ranges") {
  pump.identifyReply=SVZ;boot(c,pump);checkRanges(c,38,60,56);
 }
 CASE("encoding_and_revision_are_confirmed_settings_only") {
  CHECK(c.getState().encoding==SetpointRange::Encoding::Unknown);
  c.begin(&pump);CHECK(c.getState().encoding==SetpointRange::Encoding::Unknown);ticks(c);
  auto state=c.getState();CHECK(state.encoding==SetpointRange::Encoding::Enhanced);CHECK(state.settingsRevision==1);
  pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_ROOMTEMP);c.loop();CHECK(c.getState().settingsRevision==1);
  std::vector<uint8_t> shortSettings={0xFC,0x62,1,0x30,7,2,0,0,1,8,9,0,0};
  inject(pump,shortSettings);c.loop();CHECK(c.getState().settingsRevision==1);
  auto bad=GS12NA;bad[1]=0x62;bad[5]=2;bad.back()=0;for(uint8_t b:bad)pump.rx.push_back(b);
  c.loop();CHECK(c.getState().settingsRevision==1);
  pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();CHECK(c.getState().settingsRevision==2);
  pump.legacy=true;now_ms+=4000;c.begin(&pump);CHECK(c.getState().encoding==SetpointRange::Encoding::Unknown);ticks(c);
  CHECK(c.getState().encoding==SetpointRange::Encoding::Legacy);CHECK(c.getState().settingsRevision==3);
 }
 CASE("delivery_queued_sent_and_confirmed") {
  boot(c,pump);c.setTargetTemp(24);
  CHECK(c.setpointDelivery(24)==SetpointDelivery::Pending);CHECK(c.setpointDelivery(23)==SetpointDelivery::None);
  c.sendPendingChanges();c.loop();
  CHECK(c.setpointDelivery(24)==SetpointDelivery::Pending);
  ticks(c,300);CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
 }
 CASE("lost_set_gets_one_retry_then_refusal") {
  boot(c,pump);pump.dropTemp=2;c.setTargetTemp(24);c.sendPendingChanges();ticks(c,1500);
  CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
  c.setTargetTemp(24);c.sendPendingChanges();ticks(c,1500);
  CHECK(c.setpointDelivery(24)==SetpointDelivery::Refused);CHECK(c.refusedSetpoint(24));
  c.clearSetpointRefusal();CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
 }
 CASE("refusal_recovers_on_actual_mode_and_reconnect") {
  boot(c,pump);c.setTargetTemp(24);c.sendPendingChanges();c.loop();pump.target=170;
  hold(c,pump);CHECK(c.setpointDelivery(24)==SetpointDelivery::Refused);
  pump.target=168;pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();
  CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
  pump.target=170;pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();
  CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
  c.setTargetTemp(24);c.sendPendingChanges();c.loop();pump.target=168;hold(c,pump,168);
  CHECK(c.setpointDelivery(24)==SetpointDelivery::Refused);
  pump.mode=CN105_MODE_HEAT;pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();
  CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
  pump.mode=CN105_MODE_AUTO;pump.packet(CN105_PKT_INFO_RESP,CN105_INFO_SETTINGS);c.loop();
  CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
  c.setTargetTemp(24);c.sendPendingChanges();c.loop();pump.target=170;hold(c,pump);
  CHECK(c.setpointDelivery(24)==SetpointDelivery::Refused);c.begin(&pump);
  CHECK(c.setpointDelivery(24)==SetpointDelivery::None);ticks(c);CHECK(c.setpointDelivery(24)==SetpointDelivery::None);
 }
 CASE("bad_pairs_are_independent") {
  auto response=GS12NA;response[15]=0;response[16]=0; // Cool/Dry absent
  response[19]=190;response[20]=160; // Auto reversed
  response.back()=CN105Controller::calcChecksum(response.data(),response.size()-1);
  pump.identifyReply=response;boot(c,pump);
  auto caps=c.rangeCapabilities();CHECK(caps.detected.validMask==1);CHECK(caps.detected.freshMask==1);
  CHECK(caps.detected.mode[0].min2==20);CHECK(caps.detected.mode[0].max2==62);
 }
 CASE("absent_flag_wrong_subtype_truncated_and_checksum_ignored") {
  for(int variant=0;variant<4;++variant) {
   Pump p;CN105Controller controller;now_ms+=4000;auto response=GS12NA;
   if(variant==0)response[13]&=~4;
   if(variant==1)response[5]=0xCA;
   if(variant==2){response.erase(response.end()-2);response[4]=15;}
   response.back()=CN105Controller::calcChecksum(response.data(),response.size()-1);
   if(variant==3)response.back()^=1;
   p.identifyReply=response;boot(controller,p);ticks(controller,400);
   CHECK(controller.rangeCapabilities().detected.validMask==0);
   CHECK(p.identifies==1);CHECK(p.infos>=10);CHECK(controller.isHealthy());
   CHECK(controller._errorPollFailures==0);CHECK(!controller._errorPollDisabled);
  }
 }
 CASE("timeout_preserves_health_poll_order_and_counters") {
  boot(c,pump);CHECK(pump.identifies==1);
  const auto health=c._lastSuccessfulResponse;const auto revision=c.getState().settingsRevision;
  now_ms+=CN105_RESPONSE_TIMEOUT-200;c.loop();CHECK(pump.infos==5);
  CHECK(c._lastSuccessfulResponse==health);CHECK(c.getState().settingsRevision==revision);
  now_ms+=200;c.loop();CHECK(pump.infos==5);ticks(c,400);
  CHECK(pump.infos>=15);CHECK(pump.identifies==1);CHECK(c.isHealthy());
  CHECK(c._errorPollFailures==0);CHECK(!c._errorPollDisabled);
  std::vector<uint8_t> order;
  for(const auto& pkt:pump.sent)if(pkt[1]==CN105_PKT_INFO_REQ)order.push_back(pkt[5]);
  const uint8_t expected[]={2,3,4,6,9};
  for(size_t i=0;i<order.size();++i)CHECK(order[i]==expected[i%5]);
 }
 CASE("late_identify_never_completes_normal_poll_or_refreshes_health") {
  boot(c,pump);now_ms+=3000;pump.responding=false;c.loop();CHECK(c._cycleRunning);
  CHECK(c._pollPhase==0);const auto health=c._lastSuccessfulResponse;
  for(uint8_t b:GS12NA)pump.rx.push_back(b);c.loop();
  CHECK(c._cycleRunning);CHECK(c._awaitingResponse);CHECK(c._pollPhase==0);
  CHECK(c._lastSuccessfulResponse==health);CHECK(c.rangeCapabilities().detected.validMask==0);
 }
 CASE("identify_is_behind_pending_set_and_yields_to_new_set") {
  c.begin(&pump);c.loop();now_ms+=10;c.loop();
  c.setTargetTemp(24);c.sendPendingChanges();ticks(c);
  CHECK(pump.sets==1);CHECK(pump.identifies==0);
  now_ms+=CN105_DEFER_DELAY;ticks(c);CHECK(pump.identifies==1);
  c.setTargetTemp(25);c.sendPendingChanges();c.loop();CHECK(pump.sets==2);
  for(uint8_t b:GS12NA)pump.rx.push_back(b);c.loop();
  CHECK(c.rangeCapabilities().detected.validMask==0);ticks(c,400);CHECK(c.getState().targetTemp==25);
 }
 CASE("discovery_retries_on_connection_and_preserves_cached_pairs") {
  pump.identifyReply=GS12NA;boot(c,pump);auto caps=c.rangeCapabilities();CHECK(caps.revision==1);
  pump.identifyReply.clear();now_ms+=4000;c.begin(&pump);
  caps=c.rangeCapabilities();CHECK(caps.detected.validMask==7);CHECK(caps.detected.freshMask==0);CHECK(caps.revision==2);
  ticks(c);CHECK(pump.identifies==2);ticks(c,400);CHECK(c.rangeCapabilities().revision==2);
  now_ms+=4000;pump.identifyReply=SVZ;c.begin(&pump);ticks(c);CHECK(pump.identifies==3);
  checkRanges(c,38,60,56);CHECK(c.rangeCapabilities().revision==3);
 }
 CASE("discovery_window_survives_timer_wrap") {
  now_ms=UINT32_MAX-90;boot(c,pump);CHECK(pump.identifies==1);
  now_ms+=500;for(uint8_t b:SVZ)pump.rx.push_back(b);c.loop();checkRanges(c,38,60,56);
  now_ms+=4000;c.begin(&pump);ticks(c);CHECK(pump.identifies==2);
  now_ms+=CN105_RESPONSE_TIMEOUT;for(uint8_t b:GS12NA)pump.rx.push_back(b);c.loop();
  auto caps=c.rangeCapabilities();CHECK(caps.detected.freshMask==0);CHECK(caps.detected.mode[1].min2==38);
 }
 CASE("baud_restart_resets_encoding_and_retries_discovery") {
  struct BaudPump:Pump{bool setBaudRate(uint32_t)override{return true;}} p;
  p.identifyReply=GS12NA;c.begin(&p);ticks(c);CHECK(c.getState().encoding==SetpointRange::Encoding::Enhanced);
  c.setBaudRate(9600);c.loop();CHECK(c.getState().encoding==SetpointRange::Encoding::Unknown);
  p.legacy=true;now_ms+=4000;ticks(c);CHECK(c.getState().encoding==SetpointRange::Encoding::Legacy);CHECK(p.identifies==2);
 }
 CASE("wider_raw_pair_is_preserved_and_resolved_inward") {
  auto response=GS12NA;response[17]=140;response[18]=194;
  response.back()=CN105Controller::calcChecksum(response.data(),response.size()-1);
  pump.identifyReply=response;boot(c,pump);auto detected=c.rangeCapabilities().detected;
  CHECK(detected.validMask==7);CHECK(detected.mode[0].min2==12);CHECK(detected.mode[0].max2==66);
  SetpointRange::Preferences preferences{};
  for(auto& mode:preferences.mode)mode.selection=SetpointRange::Selection::Automatic;
  const auto resolved=SetpointRange::resolve(preferences,detected,c.getState().encoding,c.getState().encoding,0xFF);
  CHECK(resolved.mode[0].range.min2==20);CHECK(resolved.mode[0].range.max2==62);
 }
 CASE("unusable_pairs_and_encoding_sentinels_rejected") {
  for(int variant=0;variant<5;++variant) {
   Pump p;CN105Controller controller;now_ms+=4000;auto response=GS12NA;
   if(variant==0){response[17]=148;response[18]=148;} // collapsed
   if(variant==1){response[17]=128;response[18]=190;} // disable/sentinel
   if(variant==2){response[17]=148;response[18]=255;} // unavailable
   if(variant==3){response[17]=129;response[18]=147;} // no supported intersection
   if(variant==4){p.legacy=true;response[17]=161;response[18]=163;} // only one legacy point
   response[19]=168;response[20]=171; // Auto narrower than 2 C
   response.back()=CN105Controller::calcChecksum(response.data(),response.size()-1);
   p.identifyReply=response;boot(controller,p);
   CHECK(controller.rangeCapabilities().detected.validMask==2);
  }
 }
}
