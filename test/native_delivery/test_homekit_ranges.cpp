using namespace SetpointRange;
#define CASE(name) if (!strcmp(argv[1], name))
static void ranges(int low, int high) {
    auto cfg = settings.setpointSettings();
    for (auto &p : cfg.preferences.mode)
        p = {Selection::Manual, {int16_t(low * 2), int16_t(high * 2)}};
    CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
}
static void validCache() {
    for (auto *c : thermostat->chars)
        if (c->floating)
            CHECK(c->value.f >= c->min && c->value.f <= c->max);
}
int main(int, char **argv) {
    settings.begin();
    Pump pump;
    pump.mode = CN105_MODE_HEAT;
    CN105Controller c;
    now_ms = 5000;
    c.begin(&pump);
    ticks(c);
    SetpointControl::begin(c);
    g_homekitCtrl = &c;
    hap_acc_t acc;
    CASE("custom_initial_value_excludes_22") {
        ranges(10, 14);
        homekit_create_thermostat(&acc);
        CHECK(s_targetTemp->min == 10 && s_targetTemp->max == 14);
        CHECK(s_heatThresh->min == 10 && s_coolThresh->max == 14);
        validCache();
    }
    CASE("union_and_auto_metadata") {
        auto cfg = settings.setpointSettings();
        cfg.preferences.mode[0] = {Selection::Manual, {20, 28}};
        cfg.preferences.mode[1] = {Selection::Manual, {40, 60}};
        cfg.preferences.mode[2] = {Selection::Manual, {36, 52}};
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        homekit_create_thermostat(&acc);
        CHECK(s_targetTemp->min == 10 && s_targetTemp->max == 30);
        CHECK(s_heatThresh->min == 18 && s_coolThresh->max == 26);
        validCache();
    }
    CASE("actual_outside_metadata_read_fails_with_valid_cache") {
        ranges(10, 14);
        homekit_create_thermostat(&acc);
        s_started = true;
        for (int i = 0; i < 18000; ++i) {
            c.loop();
            SetpointControl::loop();
            pump.target = 172;
            now_ms += 10;
        }
        homekit_sync_thermostat(c);
        drainHttp();
        CHECK(thermostat->read_cb);
        int status = 0;
        CHECK(thermostat->read_cb(s_targetTemp, &status, nullptr, nullptr) ==
              HAP_FAIL);
        CHECK(status == HAP_STATUS_COMM_ERR);
        validCache();
        CHECK(s_statusFault->value.u == 1);
        CHECK(s_statusActive->value.b);
    }

    CASE("metadata_config_failed_enqueue_retries_once") {
        ranges(10, 14);
        homekit_create_thermostat(&acc);
        s_started = true;
        configResult = HAP_FAIL;
        homekit_reconcile_service_shape();
        CHECK(configCalls == 1);
        CHECK(!hkNvs.committed["hk-meta"].count("tempMeta"));
        // Action: retry publication after the enqueue failure.
        configResult = HAP_SUCCESS;
        homekit_reconcile_service_shape();
        CHECK(configCalls == 2);
        homekit_reconcile_service_shape();
        CHECK(configCalls == 2);
    }
    CASE("http_queue_defers_all_temperature_publication_and_retries") {
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        const float before = s_targetTemp->min;
        int calls = configCalls;
        ranges(10, 14);
        queueFails = true;
        homekit_sync_thermostat(c);
        CHECK(s_targetTemp->min == before);
        CHECK(httpQueue.empty());
        // Action: recover the HTTP queue; publication still waits for its task.
        queueFails = false;
        now_ms += 2000;
        homekit_sync_thermostat(c);
        CHECK(httpQueue.size() == 1);
        CHECK(s_targetTemp->min == before);
        validCache();
        drainHttp();
        CHECK(s_targetTemp->min == 10);
        validCache();
        CHECK(configCalls == calls + 1);
    }
    CASE("mode_and_target_do_not_bump_configuration") {
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        int calls = configCalls;
        SetpointControl::Command cmd;
        cmd.unit.mode = CN105_MODE_COOL;
        cmd.unit.targetTemp = 25;
        CHECK(SetpointControl::submit(cmd) == Result::Ok);
        homekit_sync_thermostat(c);
        drainHttp();
        CHECK(s_targetTemp->value.f == 25);
        CHECK(configCalls == calls);
    }
    CASE("heat_plus_10_both_characteristic_orders") {
        auto cfg = settings.setpointSettings();
        cfg.preferences.mode[0] = {Selection::Manual, {20, 28}};
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        homekit_create_thermostat(&acc);
        for (bool reverse : {false, true}) {
            int st[2] = {-1, -1};
            hap_write_data_t w[2] = {{s_targetState, {.u = 1}, &st[0]},
                                     {s_targetTemp, {.f = 10}, &st[1]}};
            if (reverse)
                std::swap(w[0], w[1]);
            CHECK(thermostat->write_cb(w, 2, nullptr, nullptr) == HAP_SUCCESS);
            CHECK(st[0] == 0 && st[1] == 0);
            CHECK(c.getEffectiveState().targetTemp == 10);
            validCache();
        }
    }
    CASE("auto_threshold_batch_and_stale_common_slider") {
        auto cfg = settings.setpointSettings();
        cfg.preferences.mode[0] = {Selection::Manual, {20, 28}};
        cfg.preferences.mode[2] = {Selection::Manual, {20, 60}};
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        homekit_create_thermostat(&acc);
        int st[2] = {-1, -1};
        hap_write_data_t band[2] = {{s_heatThresh, {.f = 10}, &st[0]},
                                    {s_coolThresh, {.f = 12}, &st[1]}};
        CHECK(thermostat->write_cb(band, 2, nullptr, nullptr) == HAP_SUCCESS);
        CHECK(s_heatThresh->value.f == 10 && s_coolThresh->value.f == 12);
        int status = 0;
        hap_write_data_t stale{s_targetTemp, {.f = 25}, &status};
        CHECK(thermostat->write_cb(&stale, 1, nullptr, nullptr) == HAP_FAIL);
        CHECK(status == HAP_STATUS_VAL_INVALID);
        validCache();
    }
    CASE("offline_name_units_are_independent") {
        homekit_create_thermostat(&acc);
        c.restartHandshake();
        auto *name = hap_serv_get_char_by_uuid(thermostat, "E3");
        int st[3] = {-1, -1, -1};
        hap_write_data_t w[3] = {
            {name, {.s = const_cast<char *>("Office")}, &st[0]},
            {s_tempUnits, {.u = 1}, &st[1]},
            {s_targetTemp, {.f = 22}, &st[2]}};
        CHECK(thermostat->write_cb(w, 3, nullptr, nullptr) == HAP_FAIL);
        CHECK(st[0] == 0 && st[1] == 0 && st[2] == HAP_STATUS_RES_BUSY);
        CHECK(!strcmp(name->value.s, "Office"));
        CHECK(settings.get().useFahrenheit);
    }
    CASE("boot_without_discovery_then_discovery_without_save") {
        CN105Controller cold;
        Pump coldPump;
        cold.begin(&coldPump);
        SetpointControl::begin(cold);
        g_homekitCtrl = &cold;
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        validCache();
        int calls = configCalls;
        uint8_t reply[] = {0xFC, 0x7B, 0x01, 0x30, 0x10, 0xC9, 0x03, 0x00,
                           0x20, 0x00, 0x0A, 0x07, 0x05, 0xE4, 0x25, 0xA6,
                           0xBC, 0x94, 0xB8, 0xA6, 0xB8, 0x2D};
        coldPump.identifyReply.assign(reply, reply + sizeof(reply));
        ticks(cold, 500);
        SetpointControl::loop();
        homekit_sync_thermostat(cold);
        drainHttp();
        CHECK(configCalls == calls + 1);
        CHECK(s_targetTemp->min == 10 && s_targetTemp->max == 30);
        CHECK(s_heatThresh->min == 19 && s_coolThresh->max == 28);
        validCache();
    }
    CASE("pending_then_refused_representable_actual_and_recovery_faults") {
        auto cfg = settings.setpointSettings();
        cfg.preferences.mode[0] = {Selection::Manual, {20, 28}};
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_sync_thermostat(c);
        drainHttp();
        CHECK(s_targetTemp->value.f == 14);
        for (int i = 0; i < 18000; ++i) {
            c.loop();
            SetpointControl::loop();
            pump.target = 172;
            now_ms += 10;
        }
        homekit_sync_thermostat(c);
        drainHttp();
        int status = 0;
        CHECK(thermostat->read_cb(s_targetTemp, &status, nullptr, nullptr) ==
              HAP_SUCCESS);
        CHECK(s_targetTemp->value.f == 22);
        CHECK(s_statusFault->value.u == 1);
        CHECK(s_statusActive->value.b);
        pump.target = 156;
        ticks(c, 300);
        SetpointControl::loop();
        CHECK(thermostat->read_cb(s_targetTemp, &status, nullptr, nullptr) ==
              HAP_SUCCESS);
        CHECK(s_targetTemp->value.f == 14);
        CHECK(s_statusFault->value.u == 0);
        c._state.hasError = true;
        CHECK(thermostat->read_cb(s_targetTemp, &status, nullptr, nullptr) ==
              HAP_SUCCESS);
        CHECK(s_statusFault->value.u == 1);
        c.restartHandshake();
        now_ms += 2000;
        homekit_sync_thermostat(c);
        drainHttp();
        CHECK(s_statusFault->value.u == 1);
        CHECK(!s_statusActive->value.b);
    }
    CASE("write_after_range_edit_installs_valid_cache_in_http_context") {
        homekit_create_thermostat(&acc);
        ranges(10, 14);
        int status = 0;
        hap_write_data_t w{s_targetTemp, {.f = 10}, &status};
        CHECK(thermostat->write_cb(&w, 1, nullptr, nullptr) == HAP_SUCCESS);
        CHECK(status == 0);
        CHECK(s_targetTemp->value.f == 10);
        validCache();
    }
    CASE("sdk_characteristics_error_but_accessories_serializes_valid_cache") {
        ranges(10, 14);
        homekit_create_thermostat(&acc);
        int iid = 1;
        for (auto *hc : thermostat->chars)
            hc->iid = iid++;
        thermostat->bulk_read = hap_serv_def_bulk_read_cb;
        for (int i = 0; i < 18000; ++i) {
            c.loop();
            SetpointControl::loop();
            pump.target = 172;
            now_ms += 10;
        }
        // Assert the SDK read error and valid /accessories cache separately.
        auto response = sdk_characteristic(s_targetTemp);
        CHECK(!response.values.count(s_targetTemp->iid));
        CHECK(response.statuses[s_targetTemp->iid] == HAP_STATUS_COMM_ERR);
        json_gen_str_t accessories;
        CHECK(hap_prepare_serv_db(thermostat, &accessories, 1) == HAP_SUCCESS);
        CHECK(accessories.values.count(s_targetTemp->iid));
        CHECK(accessories.values[s_targetTemp->iid] >= 10 &&
              accessories.values[s_targetTemp->iid] <= 14);
        pump.target = 156;
        ticks(c, 300);
        SetpointControl::loop();
        response = sdk_characteristic(s_targetTemp);
        CHECK(response.values[s_targetTemp->iid] == 14);
        CHECK(response.statuses.empty());
        CHECK(s_statusFault->value.u == 0);
    }
    CASE("compatibility_upgrade_and_provenance_do_not_bump") {
        auto cfg = settings.setpointSettings();
        for (auto &p : cfg.preferences.mode)
            p.selection = Selection::Compatibility;
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        CHECK(configCalls == 0);
        CHECK(hkNvs.committed["hk-meta"].count("tempMeta"));
        for (auto &p : cfg.preferences.mode)
            p = {Selection::Manual, {32, 61}};
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        homekit_reconcile_temperature_metadata(c);
        drainHttp();
        CHECK(configCalls == 0);
    }
    CASE("fingerprint_covers_all_bounds_and_steps") {
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        auto m = s_temperatureMetadata;
        int calls = configCalls;
        for (int i = 0; i < 3; ++i) {
            ++m.min2[i];
            homekit_record_temperature_metadata(m);
            CHECK(configCalls == ++calls);
            ++m.max2[i];
            homekit_record_temperature_metadata(m);
            CHECK(configCalls == ++calls);
            ++m.step2[i];
            homekit_record_temperature_metadata(m);
            CHECK(configCalls == ++calls);
        }
        homekit_record_temperature_metadata(m);
        CHECK(configCalls == calls);
    }
    CASE("shape_and_temperature_share_one_configuration_update") {
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        int calls = configCalls;
        s_bootModeMask = MODE_CAP_HEAT;
        ranges(10, 14);
        homekit_reconcile_temperature_metadata(c);
        drainHttp();
        CHECK(configCalls == calls + 1);
        homekit_reconcile_service_shape();
        CHECK(configCalls == calls + 1);
    }
    CASE("queued_publication_uses_newest_snapshot_and_serializes_with_reads") {
        homekit_create_thermostat(&acc);
        s_started = true;
        homekit_reconcile_service_shape();
        requireHttp = true;
        ranges(10, 14);
        homekit_reconcile_temperature_metadata(c);
        CHECK(httpQueue.size() == 1);
        ranges(18, 24);
        inHttp = true;
        int status = 0;
        CHECK(thermostat->read_cb(s_targetTemp, &status, nullptr, nullptr) ==
              HAP_SUCCESS);
        inHttp = false;
        CHECK(s_targetTemp->min == 18 && s_targetTemp->max == 24);
        validCache();
        int calls = configCalls;
        drainHttp();
        CHECK(s_targetTemp->min == 18 && s_targetTemp->max == 24);
        CHECK(configCalls == calls);
        SetpointControl::Command cmd;
        cmd.unit.targetTemp = 20;
        CHECK(SetpointControl::submit(cmd) == Result::Ok);
        float before = s_targetTemp->value.f;
        now_ms += 2000;
        homekit_sync_thermostat(c);
        CHECK(s_targetTemp->value.f == before);
        drainHttp();
        CHECK(s_targetTemp->value.f == 20);
        validCache();
    }
    CASE("runtime_shape_record_preserves_failed_temperature_acknowledgement") {
        ranges(10, 14);
        homekit_create_thermostat(&acc);
        s_started = true;
        configResult = HAP_FAIL;
        homekit_reconcile_service_shape();
        CHECK(configCalls == 1);
        CHECK(!hkNvs.committed["hk-meta"].count("tempMeta"));

        // The runtime SDK mutation does not report whether its config enqueue
        // worked. Recording its shape must not acknowledge our failed
        // temperature enqueue.
        s_bootModeMask = MODE_CAP_HEAT;
        homekit_record_service_shape();
        CHECK(configCalls == 1);
        CHECK(hkNvs.committed["hk-meta"]["svcShape"] ==
              std::vector<uint8_t>{MODE_CAP_HEAT});
        CHECK(!hkNvs.committed["hk-meta"].count("tempMeta"));

        // Action: retry publication after the enqueue failure.
        configResult = HAP_SUCCESS;
        homekit_reconcile_service_shape();
        CHECK(configCalls == 2);
        CHECK(hkNvs.committed["hk-meta"].count("tempMeta"));
        const auto acknowledged = hkNvs.committed["hk-meta"]["tempMeta"];
        homekit_reconcile_service_shape();
        CHECK(configCalls == 2);

        // Preserve an older acknowledged fingerprint too, not only a missing
        // key.
        configResult = HAP_FAIL;
        ranges(18, 24);
        homekit_reconcile_temperature_metadata(c);
        drainHttp();
        CHECK(configCalls == 3);
        s_bootModeMask = MODE_CAP_ALL;
        homekit_record_service_shape();
        CHECK(configCalls == 3);
        CHECK(hkNvs.committed["hk-meta"]["svcShape"] ==
              std::vector<uint8_t>{MODE_CAP_ALL});
        CHECK(hkNvs.committed["hk-meta"]["tempMeta"] == acknowledged);

        // Action: retry publication after the enqueue failure.
        configResult = HAP_SUCCESS;
        homekit_reconcile_service_shape();
        CHECK(configCalls == 4);
        CHECK(hkNvs.committed["hk-meta"]["tempMeta"] != acknowledged);
        homekit_record_service_shape();
        homekit_reconcile_service_shape();
        CHECK(configCalls == 4);
    }

    CASE("discovery_and_encoding_use_effective_auto_band") {
        // Fresh discovery changes the usable band without rewriting saved
        // intent.
        auto cfg = settings.setpointSettings();
        cfg.preferences.mode[2].selection = Selection::Automatic;
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        c._state.mode = CN105_MODE_AUTO;
        c._state.power = true;
        uint8_t data[16] = {};
        data[0] = 0xC9;
        data[8] = 4;
        data[14] = 176;
        data[15] = 184; // Auto 24..28 C.
        c.handleIdentifyResponse(data, sizeof data);
        SetpointControl::loop();
        float heat = 24, cool = 26;
        homekit_create_thermostat(&acc);
        install_temperature_metadata(&c);
        CHECK(s_heatThresh->value.f == heat);
        CHECK(s_coolThresh->value.f == cool);
        CHECK(settings.setpointSettings().band.heat2 == 40);
        CHECK(settings.setpointSettings().band.cool2 == 50);
        const auto persisted = hkNvs.committed["ac-settings"]["rangeCfg"];
        for (int i = 0; i < 3; ++i)
            install_temperature_metadata(&c);
        CHECK(hkNvs.committed["ac-settings"]["rangeCfg"] == persisted);

        // Action: steering uses precisely the published cooling threshold.
        c._state.roomTemp = 28;
        SetpointControl::steerAuto();
        CHECK(c.getEffectiveState().targetTemp == cool);

        // A partial edit starts from the effective 24..26 band, not
        // saved 20..25.
        int status = 0;
        hap_write_data_t edit{s_heatThresh, {.f = 26}, &status};
        CHECK(thermostat->write_cb(&edit, 1, nullptr, nullptr) == HAP_SUCCESS);
        CHECK(s_heatThresh->value.f == 25 && s_coolThresh->value.f == 27);
        CHECK(settings.setpointSettings().band.heat2 == 50);
        CHECK(settings.setpointSettings().band.cool2 == 54);
    }
    CASE("legacy_refresh_uses_effective_auto_band") {
        // A half-degree saved band survives reconnect; operational thresholds
        // use whole C.
        auto cfg = settings.setpointSettings();
        cfg.preferences.mode[2].selection = Selection::Automatic;
        cfg.band = {45, 49};
        CHECK(SetpointControl::applySettings(cfg) == Result::Ok);
        pump.mode = CN105_MODE_AUTO;
        c.restartHandshake();
        pump.legacy = true;
        now_ms += 4000;
        ticks(c, 300);
        SetpointControl::loop();
        float heat = 22, cool = 24;
        homekit_create_thermostat(&acc);
        install_temperature_metadata(&c);
        CHECK(s_heatThresh->value.f == heat);
        CHECK(s_coolThresh->value.f == cool);
        CHECK(settings.setpointSettings().band.heat2 == 45);
        CHECK(settings.setpointSettings().band.cool2 == 49);
        c._state.roomTemp = 20;
        SetpointControl::steerAuto();
        CHECK(c.getEffectiveState().targetTemp == heat);
    }
}
