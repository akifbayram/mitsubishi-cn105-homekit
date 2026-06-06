#include "homekit_services.h"
#include "ble_config.h"

#ifdef BLE_ENABLE

#include "ble_sensor.h"
#include "logging.h"
#include "esp_utils.h"

extern "C" {
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
}

static const char *TAG = "hk_battery";

// ── HAP UUID not exposed as a helper in the SDK ─────────────────────────────
#define HAP_CHAR_UUID_CONFIGURED_NAME "E3"

// ── Tuning ──────────────────────────────────────────────────────────────────
static constexpr uint8_t BATT_LOW_THRESHOLD              = 20;  // StatusLowBattery=1 at/below this %
static constexpr uint8_t HAP_CHARGING_STATE_NOT_CHARGEABLE = 2; // coin cell, not rechargeable

// ── Characteristic handles ──────────────────────────────────────────────────
static hap_char_t *s_battLevel     = nullptr;
static hap_char_t *s_battLowStatus = nullptr;

static uint32_t s_lastSync       = 0;
static int8_t   s_lastValidLevel = 100;  // retained across stale periods

void homekit_create_ble_battery(hap_acc_t *acc)
{
    // Creates the service with mandatory chars: BatteryLevel, ChargingState,
    // StatusLowBattery. Initial values: 100% / not-chargeable / not-low.
    hap_serv_t *serv = hap_serv_battery_service_create(
        100, HAP_CHARGING_STATE_NOT_CHARGEABLE, 0);
    if (!serv) {
        LOG_ERROR("[HK:Battery] Failed to create battery service");
        return;
    }

    s_battLevel     = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_BATTERY_LEVEL);
    s_battLowStatus = hap_serv_get_char_by_uuid(serv, HAP_CHAR_UUID_STATUS_LOW_BATTERY);

    hap_char_t *cname = hap_char_string_create(
        const_cast<char*>(HAP_CHAR_UUID_CONFIGURED_NAME),
        HAP_CHAR_PERM_PR | HAP_CHAR_PERM_PW | HAP_CHAR_PERM_EV,
        const_cast<char*>("Sensor Battery"));
    hap_serv_add_char(serv, cname);

    hap_acc_add_serv(acc, serv);

    LOG_INFO("[HK:Battery] Service created (BLE sensor battery)");
}

void homekit_sync_ble_sensor(CN105Controller &cn105)
{
    (void)cn105;  // battery is independent of the CN105 link

    uint32_t now = uptime_ms();
    if (now - s_lastSync < 2000) return;
    s_lastSync = now;

    int8_t raw = BleSensor::battery();

    uint8_t level;
    if (raw >= 0) {
        // Fresh, valid reading
        s_lastValidLevel = raw;
        level = (uint8_t)raw;
    } else if (BleSensor::isStale() && s_lastValidLevel >= 0) {
        // Lost contact after having had data — retain last known level
        level = (uint8_t)s_lastValidLevel;
    } else {
        // No sensor / unsupported / never seen — safe default, never false-alarm
        level = 100;
    }

    uint8_t low = (level <= BATT_LOW_THRESHOLD) ? 1 : 0;

    if (s_battLevel) {
        const hap_val_t *cur = hap_char_get_val(s_battLevel);
        if (!cur || cur->u != level) {
            hap_val_t v; v.u = level;
            hap_char_update_val(s_battLevel, &v);
        }
    }
    if (s_battLowStatus) {
        const hap_val_t *cur = hap_char_get_val(s_battLowStatus);
        if (!cur || cur->u != low) {
            hap_val_t v; v.u = low;
            hap_char_update_val(s_battLowStatus, &v);
            LOG_INFO("[HK:Battery] level=%u%% low=%u", level, low);
        }
    }
}

#endif // BLE_ENABLE
