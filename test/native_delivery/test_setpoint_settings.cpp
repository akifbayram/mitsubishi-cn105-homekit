#define CASE(name) if (!strcmp(argv[1], name))
using namespace SetpointRange;
static void rebootSettings(){settings=SettingsStore{};hkNvs.pending.clear();settings.begin();}
static void expectSelection(Selection s){for(auto p:settings.setpointSettings().preferences.mode)CHECK(p.selection==s);}
static SetpointSettings extended(){auto p=settings.setpointSettings();p.preferences.mode[0]={Selection::Manual,{20,62}};p.band={20,62};p.autoOwnerUnit=true;p.lastEncoding=Encoding::Legacy;return p;}
static void unchanged(uint32_t gen){CHECK(settings.generation()==gen);CHECK(settings.setpointSettings().band.heat2==40);CHECK(!settings.get().autoOwnerUnit);CHECK(settings.get().heatingThreshold==20);}
int main(int argc,char**argv){
 if(!strcmp(argv[1],"schema_less_upgrade")||!strcmp(argv[1],"heat_only_edit_preserves_upgrade"))hkNvs.committed["ac-settings"]["deviceName"]={'O','l','d',0};
 if(!strcmp(argv[1],"schema_version_upgrade"))hkNvs.committed["ac-settings"]["schemaVer"]={1};
 if(!strcmp(argv[1],"migration_only_upgrade"))hkNvs.committed["ac-settings"]["bleFeed"]={0};
 hkNvs.immediateWrites=argc>2 && !strcmp(argv[2],"immediate");
 settings.begin();
 CASE("fresh_automatic") {CHECK(settings.setpointSettingsAvailable());expectSelection(Selection::Automatic);settings.save();rebootSettings();expectSelection(Selection::Automatic);}
 CASE("schema_less_upgrade") {expectSelection(Selection::Compatibility);settings.save();rebootSettings();expectSelection(Selection::Compatibility);}
 CASE("schema_version_upgrade") {expectSelection(Selection::Compatibility);}
 CASE("migration_only_upgrade") {expectSelection(Selection::Compatibility);}
 CASE("heat_only_edit_preserves_upgrade") {
  auto p=settings.setpointSettings();p.preferences.mode[0]={Selection::Manual,{20,62}};
  CHECK(settings.saveSetpointSettings(p));rebootSettings();auto q=settings.setpointSettings();
  CHECK(q.preferences.mode[0].selection==Selection::Manual);CHECK(q.preferences.mode[0].manual.min2==20);
  CHECK(q.preferences.mode[1].selection==Selection::Compatibility);CHECK(q.preferences.mode[2].selection==Selection::Compatibility);
  CHECK(settings.get().heatingThreshold==20);CHECK(settings.get().coolingThreshold==25);CHECK(!settings.get().autoOwnerUnit);
 }
 CASE("extended_band_survives_reload") {
  CHECK(settings.saveSetpointSettings(extended()));rebootSettings();
  CHECK(settings.get().heatingThreshold==10);CHECK(settings.get().coolingThreshold==31);CHECK(settings.get().autoOwnerUnit);
  CHECK(settings.setpointSettings().lastEncoding==Encoding::Legacy);CHECK(settings.setpointSettings().preferences.mode[0].manual.min2==20);
 }
 CASE("legacy_band_writer_is_checked") {
  settings.get().heatingThreshold=10;settings.get().coolingThreshold=31;settings.get().autoOwnerUnit=true;
  settings.save();rebootSettings();CHECK(settings.get().heatingThreshold==10);CHECK(settings.get().coolingThreshold==31);CHECK(settings.get().autoOwnerUnit);
 }
 CASE("initial_policy_failure_blocks_unrelated_writes") {
  hkNvs=FakeSettingsNvs{};hkNvs.immediateWrites=argc>2 && !strcmp(argv[2],"immediate");
  settings=SettingsStore{};hkNvs.failWriteKey="rangeCfg";settings.begin();
  settings.get().deviceName[0]='X';settings.save();CHECK(!hkNvs.committed["ac-settings"].count("deviceName"));
  hkNvs.failWriteKey.clear();settings.save();rebootSettings();expectSelection(Selection::Automatic);
 }
 CASE("failed_first_policy_does_not_create_upgrade_on_restart") {
  hkNvs=FakeSettingsNvs{};hkNvs.immediateWrites=argc>2 && !strcmp(argv[2],"immediate");
  settings=SettingsStore{};hkNvs.failWriteKey="rangeCfg";settings.begin();
  hkNvs.failWriteKey.clear();rebootSettings();expectSelection(Selection::Automatic);
 }
 CASE("failed_repair_preserves_unknown_record") {
  std::vector<uint8_t> bad={99,4,3,2,1,0};hkNvs.committed["ac-settings"]["rangeCfg"]=bad;rebootSettings();
  CHECK(!settings.setpointSettingsAvailable());hkNvs.failCommit=true;
  CHECK(!settings.saveSetpointSettings(extended()));CHECK(!settings.setpointSettingsAvailable());
  CHECK(hkNvs.committed["ac-settings"]["rangeCfg"]==bad);
 }
 CASE("failed_restoration_marks_policy_unavailable") {
  uint32_t gen=settings.generation();hkNvs.failCommit=true;
  hkNvs.onCommit=[](){hkNvs.onCommit=nullptr;hkNvs.failWriteKey="rangeCfg";};
  CHECK(!settings.saveSetpointSettings(extended()));unchanged(gen);CHECK(!settings.setpointSettingsAvailable());
  hkNvs.failCommit=false;hkNvs.failWriteKey.clear();CHECK(settings.saveSetpointSettings(extended()));CHECK(settings.setpointSettingsAvailable());
 }
 CASE("write_before_error_restores_policy") {
  auto old=hkNvs.committed["ac-settings"]["rangeCfg"];uint32_t gen=settings.generation();
  hkNvs.writeBeforeErrorKey="rangeCfg";hkNvs.writeBeforeErrorCount=1;
  CHECK(!settings.saveSetpointSettings(extended()));unchanged(gen);CHECK(settings.setpointSettingsAvailable());
  CHECK(hkNvs.committed["ac-settings"]["rangeCfg"]==old);settings.save();rebootSettings();
  CHECK(settings.get().heatingThreshold==20);expectSelection(Selection::Automatic);
 }
 CASE("write_before_error_unconfirmed_restore_marks_unavailable") {
  uint32_t gen=settings.generation();hkNvs.writeBeforeErrorKey="rangeCfg";hkNvs.writeBeforeErrorCount=2;
  CHECK(!settings.saveSetpointSettings(extended()));unchanged(gen);CHECK(!settings.setpointSettingsAvailable());
 }
 CASE("write_before_error_restores_cache") {
  Detection old{{{20,62},{},{}},1,0};CHECK(settings.saveDetectedRanges(old));
  auto bytes=hkNvs.committed["ac-settings"]["rangeCache"];
  hkNvs.writeBeforeErrorKey="rangeCache";hkNvs.writeBeforeErrorCount=1;
  Detection next{{{24,60},{},{}},1,0};CHECK(!settings.saveDetectedRanges(next));
  CHECK(settings.detectedRanges().mode[0].min2==20);CHECK(hkNvs.committed["ac-settings"]["rangeCache"]==bytes);
  settings.save();rebootSettings();CHECK(settings.detectedRanges().mode[0].min2==20);
 }
 CASE("cache_after_failed_initialization_preserves_fresh_identity") {
  hkNvs=FakeSettingsNvs{};hkNvs.immediateWrites=argc>2 && !strcmp(argv[2],"immediate");
  settings=SettingsStore{};hkNvs.failWriteKey="rangeCfg";settings.begin();hkNvs.failWriteKey.clear();
  CHECK(settings.saveDetectedRanges({{{20,62},{},{}},1,0}));rebootSettings();expectSelection(Selection::Automatic);
  CHECK(settings.detectedRanges().validMask==1);
 }
 CASE("cache_waits_for_initial_policy_persistence") {
  hkNvs=FakeSettingsNvs{};hkNvs.immediateWrites=argc>2 && !strcmp(argv[2],"immediate");
  settings=SettingsStore{};hkNvs.failWriteKey="rangeCfg";settings.begin();
  CHECK(!settings.saveDetectedRanges({{{20,62},{},{}},1,0}));CHECK(!hkNvs.committed["ac-settings"].count("rangeCache"));
 }
 CASE("failed_record_write_is_atomic") {
  uint32_t g=settings.generation();hkNvs.failWriteKey="rangeCfg";CHECK(!settings.saveSetpointSettings(extended()));unchanged(g);
  hkNvs.failWriteKey.clear();settings.get().deviceName[0]='X';settings.save();rebootSettings();CHECK(settings.get().heatingThreshold==20);expectSelection(Selection::Automatic);
 }
 CASE("failed_mirror_write_is_atomic") {
  uint32_t g=settings.generation();hkNvs.failWriteKey="coolThresh";CHECK(!settings.saveSetpointSettings(extended()));unchanged(g);
  hkNvs.failWriteKey.clear();settings.save();rebootSettings();CHECK(settings.get().heatingThreshold==20);expectSelection(Selection::Automatic);
 }
 CASE("failed_commit_is_atomic") {
  uint32_t g=settings.generation();hkNvs.failCommit=true;CHECK(!settings.saveSetpointSettings(extended()));unchanged(g);
  hkNvs.failCommit=false;settings.save();rebootSettings();CHECK(settings.get().heatingThreshold==20);expectSelection(Selection::Automatic);
 }
 CASE("legacy_writer_failure_restores_views") {
  uint32_t g=settings.generation();hkNvs.failCommit=true;settings.get().heatingThreshold=10;settings.get().autoOwnerUnit=true;settings.save();unchanged(g);
 }
 CASE("invalid_candidate_does_not_write") {
  auto p=extended();p.band={20,21};int writes=hkNvs.writes;CHECK(!settings.saveSetpointSettings(p));CHECK(hkNvs.writes==writes);
  p=extended();p.preferences.mode[0].manual={10,62};CHECK(!settings.saveSetpointSettings(p));CHECK(hkNvs.writes==writes);
  p=extended();p.lastEncoding=(Encoding)99;CHECK(!settings.saveSetpointSettings(p));CHECK(hkNvs.writes==writes);
 }
 CASE("identical_policy_does_not_write") {
  auto p=extended();CHECK(settings.saveSetpointSettings(p));int writes=hkNvs.writes;uint32_t g=settings.generation();CHECK(settings.saveSetpointSettings(p));CHECK(hkNvs.writes==writes);CHECK(settings.generation()==g);
 }
 CASE("cache_changed_pairs_only_and_not_freshness") {
  Detection d{{{20,62},{32,62},{32,60}},7,7};CHECK(settings.saveDetectedRanges(d));int writes=hkNvs.writes;
  d.freshMask=0;CHECK(settings.saveDetectedRanges(d));CHECK(hkNvs.writes==writes);rebootSettings();auto q=settings.detectedRanges();CHECK(q.validMask==7);CHECK(q.freshMask==0);CHECK(q.mode[0].min2==20);
  d.mode[1]={1,19};CHECK(!settings.saveDetectedRanges(d));CHECK(hkNvs.writes==writes);
 }
 CASE("cache_preserves_wider_raw_discovery") {
  Detection d{{{12,66},{32,62},{32,60}},7,7};CHECK(settings.saveDetectedRanges(d));rebootSettings();
  CHECK(settings.detectedRanges().mode[0].min2==12);CHECK(settings.detectedRanges().mode[0].max2==66);
 }
 CASE("cache_failure_keeps_active_pairs") {
  Detection d{{{20,62},{32,62},{32,60}},7,7};hkNvs.failCommit=true;CHECK(!settings.saveDetectedRanges(d));CHECK(settings.detectedRanges().validMask==0);
  hkNvs.failCommit=false;settings.save();rebootSettings();CHECK(settings.detectedRanges().validMask==0);
 }
 CASE("malformed_policy_requires_deliberate_repair") {
  CHECK(settings.saveSetpointSettings(extended()));auto stored=hkNvs.committed["ac-settings"]["rangeCfg"];
  for(int variant=0;variant<4;variant++){
   auto bad=stored;if(variant==0)bad[0]=99;if(variant==1)bad.pop_back();if(variant==2)bad[1]=99;if(variant==3)bad.push_back(0);
   hkNvs.committed["ac-settings"]["rangeCfg"]=bad;rebootSettings();CHECK(!settings.setpointSettingsAvailable());
   settings.save();CHECK(hkNvs.committed["ac-settings"]["rangeCfg"]==bad);CHECK(!settings.setpointSettingsAvailable());
  }
  CHECK(settings.saveSetpointSettings(extended()));CHECK(settings.setpointSettingsAvailable());rebootSettings();CHECK(settings.setpointSettingsAvailable());
 }
 CASE("reset_overlap_at_write") {
  auto p=extended();uint32_t g=settings.generation();std::thread reset;
  hkNvs.onWrite=[&](){hkNvs.onWrite=nullptr;reset=std::thread([]{settings.beginFactoryReset();});while(!settings.isFactoryResetting())std::this_thread::yield();};
  CHECK(!settings.saveSetpointSettings(p));reset.join();unchanged(g);CHECK(!settings.saveDetectedRanges({{{20,62},{},{ }},1,0}));
 }
 CASE("reset_overlap_at_commit") {
  auto p=extended();uint32_t g=settings.generation();std::thread reset;
  hkNvs.onCommit=[&](){hkNvs.onCommit=nullptr;reset=std::thread([]{settings.beginFactoryReset();});while(!settings.isFactoryResetting())std::this_thread::yield();};
  CHECK(!settings.saveSetpointSettings(p));reset.join();unchanged(g);
 }
 CASE("factory_reset_clears_both_records") {
  CHECK(settings.saveSetpointSettings(extended()));CHECK(settings.saveDetectedRanges({{{20,62},{},{}},1,0}));
  settings.beginFactoryReset();CHECK(nvs_flash_erase()==ESP_OK);CHECK(nvs_flash_init()==ESP_OK);s_factoryResetting.store(false);rebootSettings();expectSelection(Selection::Automatic);CHECK(settings.detectedRanges().validMask==0);
 }
}
