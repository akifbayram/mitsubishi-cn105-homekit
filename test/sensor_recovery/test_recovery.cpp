
static const uint8_t dial_a[6] = {1, 2, 3, 4, 5, 6}, dial_b[6] = {6, 5, 4, 3, 2, 1};
static void begin_ble() {
    strcpy(settings.get().bleSensors[0].addr, "aa:bb:cc:dd:ee:ff");
    BleSensor::begin();
}
static void advertise(const char* mac = "aa:bb:cc:dd:ee:ff") {
    // Real PVVX advertisement: 21.00 C, 55.00%, 95% battery.
    uint8_t data[] = {0x12,0x16,0x1a,0x18,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
                      0x34,0x08,0x7c,0x15,0x54,0x0b,0x5f,0x01,0x04};
    uint8_t address[6]; CHECK(mac_from_str(mac, address));
    ble_gap_event event = {}; event.type = BLE_GAP_EVENT_DISC;
    for (int i = 0; i < 6; i++) event.disc.addr.val[i] = address[5-i];
    event.disc.data = data; event.disc.length_data = sizeof data;
    gap_event_cb(&event, nullptr);
}
static void report_dial(const uint8_t mac[6], int16_t cc, bool capable = true) {
    sl2_dial_sensor_pkt packet = {};
    packet.flags = capable ? SL2_DSF_HAS_SENSOR : 0;
    packet.temp_cc = cc; packet.hum_cc = SL2_HUM_CC_NA;
    LinkSensor::feed(mac, &packet);
}
static void ticks(CN105Controller& c, int n = 2) {
    for (int i = 0; i < n; i++) { now_ms += 1000; BleSensor::loop(c); LinkSensor::loop(c); RoomAvg::loop(c); }
}
static void feed_ble(CN105Controller& c) {
    begin_ble(); advertise(); BleSensor::loop(c); CHECK(c.temp == 21 && c.sends == 1);
}

int main(int, char** argv) {
    CN105Controller c;
    CASE("unused_ble_disable_preserves_external_override") {
        settings.get().roomSingle = ROOM_MEMBER_INTERNAL;
        begin_ble(); c.temp = 24; BleSensor::setBleEnabled(false); ticks(c);
        CHECK(c.clears == 0 && c.temp == 24);
    }
    CASE("unfed_replacement_preserves_external_override") {
        begin_ble(); advertise(); c.temp = 24;
        BleSensor::setSensor(0, "11:22:33:44:55:66", nullptr); ticks(c);
        CHECK(c.clears == 0 && c.temp == 24);
    }
    CASE("disable_after_snapshot_retains_clear") {
        feed_ble(c); now_ms += 20000;
        c.atConnected = [] { BleSensor::setBleEnabled(false); };
        BleSensor::loop(c); ticks(c);
        CHECK(c.clears == 1 && std::isnan(c.temp)); CHECK(c.sends == 1);
    }
    CASE("replacement_after_snapshot_retains_clear") {
        feed_ble(c); now_ms += 20000;
        c.atConnected = [] { BleSensor::setSensor(0, "11:22:33:44:55:66", nullptr); };
        BleSensor::loop(c); ticks(c);
        CHECK(c.clears == 1 && std::isnan(c.temp)); CHECK(c.sends == 1);
    }
    CASE("disable_at_send_boundary_retains_clear") {
        feed_ble(c); now_ms += 20000;
        c.atSend = [] { BleSensor::setBleEnabled(false); };
        BleSensor::loop(c); ticks(c);
        CHECK(c.clears == 1 && std::isnan(c.temp));
    }
    CASE("replacement_at_send_boundary_retains_clear") {
        feed_ble(c); now_ms += 20000;
        c.atSend = [] { BleSensor::setSensor(0, "11:22:33:44:55:66", nullptr); };
        BleSensor::loop(c); ticks(c);
        CHECK(c.clears == 1 && std::isnan(c.temp));
    }
    CASE("fresh_replacement_supersedes_disconnected_clear") {
        feed_ble(c); c.connected = false;
        BleSensor::setSensor(0, "11:22:33:44:55:66", nullptr); ticks(c);
        advertise("11:22:33:44:55:66"); c.connected = true; now_ms += 20000; ticks(c);
        CHECK(c.clears == 0 && c.temp == 21);
    }
    CASE("ble_handoff_preserves_link_and_average") {
        feed_ble(c); report_dial(dial_a, 2400);
        settings.get().roomSingle = ROOM_MEMBER_LINK;
        LinkSensor::loop(c); BleSensor::setBleEnabled(false); ticks(c);
        CHECK(c.temp == 24 && c.clears == 0);
        settings.get().roomMode = 1; settings.get().roomMembers = 1u << ROOM_MEMBER_LINK;
        RoomAvg::loop(c); ticks(c);
        CHECK(c.temp == 24 && c.clears == 0 && RoomAvg::isFeeding());
    }
    CASE("link_and_average_handoff_preserve_ble") {
        begin_ble(); advertise(); report_dial(dial_a, 2400);
        settings.get().roomSingle = ROOM_MEMBER_LINK; LinkSensor::loop(c);
        settings.get().roomSingle = ROOM_MEMBER_BLE0; ticks(c);
        CHECK(c.temp == 21 && c.clears == 0);
        settings.get().roomMode = 1; settings.get().roomMembers = 1u << ROOM_MEMBER_LINK; ticks(c);
        const int clears = c.clears;
        settings.get().roomMode = 0; ticks(c);
        CHECK(c.temp == 21 && c.clears == clears);
    }
    CASE("scan_waits_for_host_sync") {
        host_ready = false; begin_ble(); ticks(c, 5);
        CHECK(scan_attempts == 0 && !radio_scanning);
        host_ready = true; if (ble_hs_cfg.sync_cb) ble_hs_cfg.sync_cb(); ticks(c);
        CHECK(radio_scanning);
    }
    CASE("failed_scan_retries_with_backoff") {
        scan_failures = 100; begin_ble();
        for (int i = 0; i < 100; i++) BleSensor::loop(c);
        CHECK(scan_attempts <= 2);
        scan_failures = 0; ticks(c, 10);
        CHECK(radio_scanning && scan_attempts >= 2);
    }
    CASE("failed_initialization_retries") {
        init_failures = 100; begin_ble();
        for (int i = 0; i < 100; i++) BleSensor::loop(c);
        CHECK(init_attempts <= 2); init_failures = 0; ticks(c, 10);
        CHECK(init_attempts >= 2 && radio_scanning);
    }
    CASE("failed_teardown_retries_with_backoff") {
        feed_ble(c); stop_failures = 100; BleSensor::setBleEnabled(false);
        for (int i = 0; i < 100; i++) BleSensor::loop(c);
        CHECK(c.clears == 1 && std::isnan(c.temp));
        CHECK(stop_attempts <= 2); stop_failures = 0; ticks(c, 10);
        const int attempts = stop_attempts; ticks(c, 3);
        CHECK(stop_attempts == attempts && !radio_scanning);
    }
    CASE("persistent_init_failure_backs_off") {
        init_failures = 100000; begin_ble(); ticks(c, 3600);
        CHECK(init_attempts >= 40 && init_attempts <= 80);
    }
    CASE("enable_toggle_resets_init_backoff") {
        init_failures = 100000; begin_ble(); ticks(c, 600);
        const int before = init_attempts; now_ms += 1000;
        BleSensor::setBleEnabled(false); BleSensor::setBleEnabled(true); BleSensor::loop(c);
        CHECK(init_attempts == before + 1);
    }
    CASE("persistent_teardown_failure_backs_off") {
        feed_ble(c); stop_failures = 100000; BleSensor::setBleEnabled(false); ticks(c, 3600);
        CHECK(stop_attempts >= 40 && stop_attempts <= 80);
    }
    CASE("failed_scan_cancel_retries_address_change") {
        // One scan window serves every sensor, so only an address change
        // restarts the scan. A failed cancel must retry, not pile up starts.
        begin_ble(); ticks(c); const int cancels = cancel_attempts, starts = scan_attempts;
        cancel_failures = 100; BleSensor::setSensor(0, "11:22:33:44:55:66", "Moved");
        for (int i = 0; i < 100; i++) BleSensor::loop(c);
        CHECK(radio_scanning && scan_attempts == starts);
        CHECK(cancel_attempts - cancels <= 2);
        cancel_failures = 0; ticks(c, 10);
        CHECK(radio_scanning && scan_attempts == starts + 1 && cancel_attempts > cancels);
    }
    CASE("host_reset_resumes_scanning_after_sync") {
        begin_ble(); ticks(c); CHECK(radio_scanning);
        host_ready = false; radio_scanning = false;
        if (ble_hs_cfg.reset_cb) ble_hs_cfg.reset_cb(22);
        const int attempts = scan_attempts; ticks(c, 3); CHECK(scan_attempts == attempts);
        host_ready = true; if (ble_hs_cfg.sync_cb) ble_hs_cfg.sync_cb(); ticks(c);
        CHECK(radio_scanning);
    }
    CASE("scan_completion_restarts_from_main_loop") {
        begin_ble(); ticks(c); CHECK(radio_scanning);
        scan_complete(); CHECK(!radio_scanning); ticks(c); CHECK(radio_scanning);
    }
    CASE("ble_full_clock_cycle_never_resurrects_stale_sample") {
        feed_ble(c); const uint64_t sampled = now_ms;
        now_ms += 600001; ticks(c); CHECK(BleSensor::isStale(0));
        const int sent = c.sends;
        now_ms = sampled + (UINT64_C(1) << 32) + 1; ticks(c);
        CHECK(!BleSensor::isActive(0) && BleSensor::isStale(0));
        CHECK(BleSensor::lastUpdateAge(0) == UINT32_MAX);
        CHECK(c.sends == sent && std::isnan(c.temp));
        advertise(); ticks(c); CHECK(BleSensor::isActive(0) && c.temp == 21);
    }
    CASE("link_full_clock_cycle_never_resurrects_stale_sample") {
        settings.get().bleEnabled = false; BleSensor::begin();
        settings.get().roomSingle = ROOM_MEMBER_LINK; report_dial(dial_a, 2400);
        LinkSensor::loop(c); const uint64_t sampled = now_ms;
        now_ms += 600001; ticks(c); CHECK(LinkSensor::isStale()); const int sent = c.sends;
        now_ms = sampled + (UINT64_C(1) << 32) + 1; ticks(c);
        CHECK(!LinkSensor::isActive() && LinkSensor::isStale());
        CHECK(LinkSensor::status() == SL2_ROOMST_STALE);
        CHECK(LinkSensor::lastUpdateAge() == UINT32_MAX);
        LinkSensor::Reading readings[SL2_MAX_DIALS]; CHECK(LinkSensor::readings(readings) == 1);
        CHECK(!readings[0].fresh && readings[0].ageMs == UINT32_MAX);
        CHECK(c.sends == sent && std::isnan(c.temp));
        report_dial(dial_a, 2500); ticks(c); CHECK(LinkSensor::isActive() && c.temp == 25);
    }
    CASE("recent_samples_remain_fresh_across_clock_rollover") {
        begin_ble(); now_ms = (UINT64_C(1) << 32) - 100;
        advertise(); report_dial(dial_a, 2400); now_ms += 200;
        CHECK(BleSensor::isActive(0) && BleSensor::lastUpdateAge(0) == 200);
        CHECK(LinkSensor::isActive() && LinkSensor::lastUpdateAge() == 200);
    }
    CASE("average_selects_two_link_dials_without_ble") {
        settings.get().roomMode = 1; settings.get().roomMembers = 1u << ROOM_MEMBER_LINK;
        report_dial(dial_a, 2000); report_dial(dial_b, 2400);
        CHECK(RoomAvg::averageSelectable()); RoomAvg::loop(c);
        CHECK(RoomAvg::status().feeding && RoomAvg::status().effective == 22);
        LinkSensor::forget(dial_b); CHECK(!RoomAvg::averageSelectable());
    }
    CASE("average_capability_counts_offline_but_not_sensorless_dials") {
        settings.get().roomMembers = 1u << ROOM_MEMBER_LINK;
        report_dial(dial_a, 2000); report_dial(dial_b, SL2_CC_NA, false);
        CHECK(!RoomAvg::averageSelectable());
        report_dial(dial_b, SL2_CC_NA); now_ms += 600001;
        CHECK(RoomAvg::averageSelectable());
        settings.get().roomMembers = 0; CHECK(!RoomAvg::averageSelectable());
    }
}
