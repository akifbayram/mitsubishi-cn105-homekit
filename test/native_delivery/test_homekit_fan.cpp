#define CASE(name) if (!strcmp(argv[1], name))
int main(int, char** argv) {
 Pump pump; CN105Controller c;
 hap_char_t fanHealth{}, autoHealth{}, autoOn{};
 fanHealth.val.b=autoHealth.val.b=true;
 s_fanStatusActive=&fanHealth;s_fanAutoStatusActive=&autoHealth;s_fanAutoOn=&autoOn;
 CASE("fan_auto_reports_unhealthy_on_disconnected_boot") {
  now_ms=5000;homekit_sync_fan(c);
  CHECK(!fanHealth.val.b);CHECK(!autoHealth.val.b);
 }
 CASE("fan_auto_tracks_loss_and_recovery_with_fan") {
  now_ms=5000;c.begin(&pump);ticks(c);homekit_sync_fan(c);
  CHECK(fanHealth.val.b&&autoHealth.val.b&&autoOn.val.b);
  pump.responding=false;now_ms+=15000;ticks(c);homekit_sync_fan(c);
  CHECK(!c.isHealthy());CHECK(!fanHealth.val.b);CHECK(!autoHealth.val.b);
  int updates=autoOn.updates;
  pump.responding=true;now_ms+=4000;ticks(c);homekit_sync_fan(c);
  CHECK(c.isHealthy());CHECK(fanHealth.val.b&&autoHealth.val.b&&autoOn.val.b);
  CHECK(autoOn.updates==updates+1);
 }
 CASE("fan_auto_syncs_when_fan_is_hidden") {
  s_fanStatusActive=nullptr;   // Fan not built this boot
  now_ms=5000;c.begin(&pump);ticks(c);homekit_sync_fan(c);
  CHECK(autoHealth.val.b&&autoOn.val.b);CHECK(fanHealth.updates==0);
  pump.responding=false;now_ms+=15000;ticks(c);homekit_sync_fan(c);
  CHECK(!autoHealth.val.b);CHECK(fanHealth.updates==0);
 }
 CASE("fan_syncs_when_fan_auto_is_hidden") {
  s_fanAutoStatusActive=nullptr;s_fanAutoOn=nullptr;   // Fan Auto not built this boot
  now_ms=5000;c.begin(&pump);ticks(c);homekit_sync_fan(c);
  CHECK(fanHealth.val.b);CHECK(autoHealth.updates==0&&autoOn.updates==0);
  pump.responding=false;now_ms+=15000;ticks(c);homekit_sync_fan(c);
  CHECK(!fanHealth.val.b);CHECK(autoHealth.updates==0);
 }
 CASE("fan_sync_does_not_update_before_its_next_tick") {
  now_ms=5000;homekit_sync_fan(c);int updates=autoHealth.updates;
  now_ms+=1000;homekit_sync_fan(c);CHECK(autoHealth.updates==updates);
 }
}
