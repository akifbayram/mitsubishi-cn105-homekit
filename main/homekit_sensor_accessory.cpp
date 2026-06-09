#include "homekit_sensor_accessory.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "branding.h"
#include "logging.h"
#include "esp_utils.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
}

static const char *TAG = "hk_sensor";

// ── Tuning ──────────────────────────────────────────────────────────────────
static constexpr uint8_t BATT_LOW_THRESHOLD              = 20;  // StatusLowBattery=1 at/below this %
static constexpr uint8_t HAP_CHARGING_STATE_NOT_CHARGEABLE = 2; // coin cell, not rechargeable

// ── State ─────────────────────────────────────────────────────────────────────
static hap_acc_t  *s_rsAcc       = nullptr;
static hap_char_t *s_rsTemp      = nullptr;
static hap_char_t *s_rsHum       = nullptr;
static hap_char_t *s_rsTempActive = nullptr;  // StatusActive on the temperature service
static hap_char_t *s_rsHumActive  = nullptr;  // StatusActive on the humidity service
static hap_char_t *s_rsBattLevel = nullptr;
static hap_char_t *s_rsBattLow   = nullptr;
static char        s_serial[20]  = {0};
static char        s_fwRev[16]   = "0.0.0";
static uint32_t    s_lastSync    = 0;

static int sensor_identify(hap_acc_t *ha)
{
    LOG_INFO("[HK:Sensor] identify");
    return HAP_SUCCESS;
}

// Map the raw BLE battery reading (-1 = unknown) to HAP level + low-battery flag.
static void read_battery(uint8_t *level, uint8_t *low)
{
    int8_t raw = BleSensor::battery();
    *level = (raw >= 0) ? (uint8_t)raw : 100;
    *low   = (*level <= BATT_LOW_THRESHOLD) ? 1 : 0;
}

// Push a value to a HAP characteristic only when it actually changed (avoids
// spurious HomeKit notifications). The _u variant reports whether it wrote.
static void update_char_f(hap_char_t *c, float v)
{
    if (!c) return;
    const hap_val_t *cur = hap_char_get_val(c);
    if (!cur || cur->f != v) { hap_val_t nv; nv.f = v; hap_char_update_val(c, &nv); }
}

static bool update_char_u(hap_char_t *c, uint8_t v)
{
    if (!c) return false;
    const hap_val_t *cur = hap_char_get_val(c);
    if (cur && cur->u == v) return false;
    hap_val_t nv; nv.u = v; hap_char_update_val(c, &nv);
    return true;
}

static void update_char_b(hap_char_t *c, bool v)
{
    if (!c) return;
    const hap_val_t *cur = hap_char_get_val(c);
    if (cur && cur->b == v) return;
    hap_val_t nv; nv.b = v; hap_char_update_val(c, &nv);
}

// Gates whether the Remote Sensor accessory is published in HomeKit: the BLE feature
// must be enabled AND a sensor MAC set. A MAC alone is not enough — one can be present
// without the operator opting in (a compile-time BLE_SENSOR_ADDR default, or stale
// NVS), and without the isBleEnabled() check those units would surface a phantom
// "Remote Sensor" on initial HomeKit import even though no sensor was provisioned.
//
// Persistence: once added we never remove the accessory at runtime (toggling BLE off
// flips StatusActive instead — see sensor_active), preserving the Home-app room/name
// across a live toggle and avoiding a free of an accessory the HAP task may be
// traversing. It is only (re)created at boot when BLE is enabled, so a unit left with
// BLE disabled does not carry the accessory across a reboot.
static bool sensor_configured()
{
    return BleSensor::isBleEnabled() && BleSensor::getAddr()[0] != '\0';
}

// StatusActive: enabled + configured (both implied by sensor_configured) and we have
// fresh data (isActive() requires at least one reading). false surfaces as "Not
// Responding" in the Home app.
static bool sensor_active()
{
    return sensor_configured() && BleSensor::isActive();
}

// Delete the accessory object and clear all cached char pointers. Used by the
// create() failure paths (the accessory is never removed once successfully added).
static void free_sensor_accessory()
{
    if (s_rsAcc) hap_acc_delete(s_rsAcc);
    s_rsAcc = nullptr;
    s_rsTemp = s_rsHum = s_rsBattLevel = s_rsBattLow = nullptr;
    s_rsTempActive = s_rsHumActive = nullptr;
}

static void create_sensor_accessory()
{
    if (s_serial[0] == '\0') {
        LOG_WARN("[HK:Sensor] serial not set — pass it to homekit_sensor_begin() first");
    }

    // Unique SerialNumber per accessory (HAP spec): the bridge and AC use the base
    // serial, so the sensor gets a "-rs" suffix. The same "-rs" string is also the
    // AID key (hap_get_unique_aid below), which is the existing accessory identity —
    // so this only adds a distinct SerialNumber and doesn't re-key the accessory.
    char rsSerial[24];
    snprintf(rsSerial, sizeof(rsSerial), "%s-rs", s_serial);

    hap_acc_cfg_t cfg = {
        .name             = const_cast<char*>("Remote Sensor"),
        .model            = const_cast<char*>(BRAND_MODEL),
        .manufacturer     = const_cast<char*>(BRAND_MANUFACTURER),
        .serial_num       = rsSerial,
        .fw_rev           = s_fwRev,
        .hw_rev           = nullptr,
        .pv               = const_cast<char*>("1.1.0"),
        .cid              = HAP_CID_SENSOR,
        .identify_routine = sensor_identify,
    };
    s_rsAcc = hap_acc_create(&cfg);
    if (!s_rsAcc) {
        LOG_ERROR("[HK:Sensor] hap_acc_create failed");
        return;
    }

    bool active = sensor_active();

    // Temperature Sensor (primary)
    float t = BleSensor::temperature();
    hap_serv_t *ts = hap_serv_temperature_sensor_create(std::isnan(t) ? 0.0f : t);
    if (!ts) {
        LOG_ERROR("[HK:Sensor] temperature service alloc failed");
        free_sensor_accessory();
        return;
    }
    s_rsTemp = hap_serv_get_char_by_uuid(ts, HAP_CHAR_UUID_CURRENT_TEMPERATURE);
    // Widen the default 0–100 °C range: the SDK silently drops out-of-range updates,
    // so a remote sensor below freezing would otherwise freeze at its last value.
    if (s_rsTemp) hap_char_float_set_constraints(s_rsTemp, -50.0f, 100.0f, 0.1f);
    s_rsTempActive = hap_char_status_active_create(active);
    hap_serv_add_char(ts, s_rsTempActive);
    hap_acc_add_serv(s_rsAcc, ts);

    // Humidity Sensor
    float h = BleSensor::humidity();
    hap_serv_t *hs = hap_serv_humidity_sensor_create(std::isnan(h) ? 0.0f : h);
    if (!hs) {
        LOG_ERROR("[HK:Sensor] humidity service alloc failed");
        free_sensor_accessory();
        return;
    }
    s_rsHum = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_CURRENT_RELATIVE_HUMIDITY);
    s_rsHumActive = hap_char_status_active_create(active);
    hap_serv_add_char(hs, s_rsHumActive);
    hap_acc_add_serv(s_rsAcc, hs);

    // Battery Service
    uint8_t level, low;
    read_battery(&level, &low);
    hap_serv_t *bs = hap_serv_battery_service_create(level, HAP_CHARGING_STATE_NOT_CHARGEABLE, low);
    if (!bs) {
        LOG_ERROR("[HK:Sensor] battery service alloc failed");
        free_sensor_accessory();
        return;
    }
    s_rsBattLevel = hap_serv_get_char_by_uuid(bs, HAP_CHAR_UUID_BATTERY_LEVEL);
    s_rsBattLow   = hap_serv_get_char_by_uuid(bs, HAP_CHAR_UUID_STATUS_LOW_BATTERY);
    hap_acc_add_serv(s_rsAcc, bs);

    // rsSerial ("<serial>-rs") doubles as the stable AID key — one source string.
    hap_add_bridged_accessory(s_rsAcc, hap_get_unique_aid(rsSerial));
    LOG_INFO("[HK:Sensor] Remote Sensor accessory added (temp/humidity/battery)");
}

// Create the accessory the first time a sensor MAC is configured, then leave it in
// place. We never remove it at runtime: enable/disable and staleness are reflected
// via StatusActive instead (see sensor_active). This is an append-only mutation of
// the HAP accessory list — safe to call from the main loop alongside the HAP task.
static void ensure_sensor_accessory()
{
    if (sensor_configured() && !s_rsAcc) {
        create_sensor_accessory();
    }
}

void homekit_sensor_begin(const char* serial, const char* fwRev)
{
    if (serial) {
        strncpy(s_serial, serial, sizeof(s_serial) - 1);
        s_serial[sizeof(s_serial) - 1] = '\0';
    }
    if (fwRev) {
        strncpy(s_fwRev, fwRev, sizeof(s_fwRev) - 1);
        s_fwRev[sizeof(s_fwRev) - 1] = '\0';
    }
    ensure_sensor_accessory();  // pre-hap_start: include in the initial bridge DB if configured
}

void homekit_sensor_loop()
{
    // Throttle to 2s: covers the (one-shot) first-configuration create and the
    // value push — BLE sensor data changes far slower than this.
    uint32_t now = uptime_ms();
    if (now - s_lastSync < 2000) return;
    s_lastSync = now;

    ensure_sensor_accessory();
    if (!s_rsAcc) return;

    bool active = sensor_active();
    update_char_b(s_rsTempActive, active);
    update_char_b(s_rsHumActive,  active);

    float t = BleSensor::temperature();
    if (!std::isnan(t)) update_char_f(s_rsTemp, t);

    float h = BleSensor::humidity();
    if (!std::isnan(h)) update_char_f(s_rsHum, h);

    uint8_t level, low;
    read_battery(&level, &low);
    if (update_char_u(s_rsBattLevel, level)) LOG_DEBUG("[HK:Sensor] battery=%u%%", level);
    if (update_char_u(s_rsBattLow,   low))   LOG_INFO("[HK:Sensor] battery=%u%% low=%u", level, low);
}

#endif // BLE_ENABLE
