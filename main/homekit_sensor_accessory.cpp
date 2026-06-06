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

#ifndef FW_VERSION
#define FW_VERSION "0.0.0"
#endif

static const char *TAG = "hk_sensor";

// ── Tuning ──────────────────────────────────────────────────────────────────
static constexpr uint8_t BATT_LOW_THRESHOLD              = 20;  // StatusLowBattery=1 at/below this %
static constexpr uint8_t HAP_CHARGING_STATE_NOT_CHARGEABLE = 2; // coin cell, not rechargeable

// ── State ─────────────────────────────────────────────────────────────────────
static hap_acc_t  *s_rsAcc       = nullptr;
static hap_char_t *s_rsTemp      = nullptr;
static hap_char_t *s_rsHum       = nullptr;
static hap_char_t *s_rsBattLevel = nullptr;
static hap_char_t *s_rsBattLow   = nullptr;
static char        s_serial[20]  = {0};
static uint32_t    s_lastSync    = 0;

static int sensor_identify(hap_acc_t *ha)
{
    LOG_INFO("[HK:Sensor] identify");
    return HAP_SUCCESS;
}

void homekit_sensor_set_serial(const char* serial)
{
    if (!serial) return;
    strncpy(s_serial, serial, sizeof(s_serial) - 1);
    s_serial[sizeof(s_serial) - 1] = '\0';
}

static void create_sensor_accessory()
{
    if (s_serial[0] == '\0') {
        LOG_WARN("[HK:Sensor] serial not set — call homekit_sensor_set_serial() first");
    }

    hap_acc_cfg_t cfg = {
        .name             = const_cast<char*>("Remote Sensor"),
        .model            = const_cast<char*>(BRAND_MODEL),
        .manufacturer     = const_cast<char*>(BRAND_MANUFACTURER),
        .serial_num       = s_serial,
        .fw_rev           = const_cast<char*>(FW_VERSION),
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

    // No StatusActive characteristic here (intentional): unlike the AC accessory,
    // this accessory's presence is conveyed by dynamic add/remove (it disappears
    // when BLE is disabled/unconfigured), and a stale-but-enabled sensor holds its
    // last values rather than showing "Not Responding".

    // Temperature Sensor (primary)
    float t = BleSensor::temperature();
    hap_serv_t *ts = hap_serv_temperature_sensor_create(std::isnan(t) ? 0.0f : t);
    if (!ts) {
        LOG_ERROR("[HK:Sensor] temperature service alloc failed");
        hap_acc_delete(s_rsAcc); s_rsAcc = nullptr;
        return;
    }
    s_rsTemp = hap_serv_get_char_by_uuid(ts, HAP_CHAR_UUID_CURRENT_TEMPERATURE);
    hap_acc_add_serv(s_rsAcc, ts);

    // Humidity Sensor
    float h = BleSensor::humidity();
    hap_serv_t *hs = hap_serv_humidity_sensor_create(std::isnan(h) ? 0.0f : h);
    if (!hs) {
        LOG_ERROR("[HK:Sensor] humidity service alloc failed");
        hap_acc_delete(s_rsAcc); s_rsAcc = nullptr;
        s_rsTemp = nullptr;
        return;
    }
    s_rsHum = hap_serv_get_char_by_uuid(hs, HAP_CHAR_UUID_CURRENT_RELATIVE_HUMIDITY);
    hap_acc_add_serv(s_rsAcc, hs);

    // Battery Service
    int8_t  raw   = BleSensor::battery();
    uint8_t level = (raw >= 0) ? (uint8_t)raw : 100;
    uint8_t low   = (level <= BATT_LOW_THRESHOLD) ? 1 : 0;
    hap_serv_t *bs = hap_serv_battery_service_create(level, HAP_CHARGING_STATE_NOT_CHARGEABLE, low);
    if (!bs) {
        LOG_ERROR("[HK:Sensor] battery service alloc failed");
        hap_acc_delete(s_rsAcc); s_rsAcc = nullptr;
        s_rsTemp = nullptr; s_rsHum = nullptr;
        return;
    }
    s_rsBattLevel = hap_serv_get_char_by_uuid(bs, HAP_CHAR_UUID_BATTERY_LEVEL);
    s_rsBattLow   = hap_serv_get_char_by_uuid(bs, HAP_CHAR_UUID_STATUS_LOW_BATTERY);
    hap_acc_add_serv(s_rsAcc, bs);

    char aidStr[24];
    snprintf(aidStr, sizeof(aidStr), "%s-rs", s_serial);
    hap_add_bridged_accessory(s_rsAcc, hap_get_unique_aid(aidStr));
    LOG_INFO("[HK:Sensor] Remote Sensor accessory added (temp/humidity/battery)");
}

static void destroy_sensor_accessory()
{
    if (!s_rsAcc) return;
    hap_remove_bridged_accessory(s_rsAcc);  // unlinks + bumps config number
    hap_acc_delete(s_rsAcc);                // frees the accessory and its services
    s_rsAcc = nullptr;
    s_rsTemp = s_rsHum = s_rsBattLevel = s_rsBattLow = nullptr;
    LOG_INFO("[HK:Sensor] Remote Sensor accessory removed");
}

// Desired = BLE enabled AND a sensor MAC configured. Adds/removes on transition.
static void update_topology()
{
    bool desired = BleSensor::isBleEnabled() && BleSensor::getAddr()[0] != '\0';
    if (desired && !s_rsAcc) {
        create_sensor_accessory();
    } else if (!desired && s_rsAcc) {
        destroy_sensor_accessory();
    }
}

void homekit_sensor_begin()
{
    update_topology();  // pre-hap_start: include in the initial bridge DB if configured
}

void homekit_sensor_loop()
{
    update_topology();
    if (!s_rsAcc) return;

    uint32_t now = uptime_ms();
    if (now - s_lastSync < 2000) return;
    s_lastSync = now;

    float t = BleSensor::temperature();
    if (s_rsTemp && !std::isnan(t)) {
        const hap_val_t *cur = hap_char_get_val(s_rsTemp);
        if (!cur || cur->f != t) { hap_val_t v; v.f = t; hap_char_update_val(s_rsTemp, &v); }
    }

    float h = BleSensor::humidity();
    if (s_rsHum && !std::isnan(h)) {
        const hap_val_t *cur = hap_char_get_val(s_rsHum);
        if (!cur || cur->f != h) { hap_val_t v; v.f = h; hap_char_update_val(s_rsHum, &v); }
    }

    int8_t  raw   = BleSensor::battery();
    uint8_t level = (raw >= 0) ? (uint8_t)raw : 100;
    uint8_t low   = (level <= BATT_LOW_THRESHOLD) ? 1 : 0;
    if (s_rsBattLevel) {
        const hap_val_t *cur = hap_char_get_val(s_rsBattLevel);
        if (!cur || cur->u != level) {
            LOG_DEBUG("[HK:Sensor] battery=%u%%", level);
            hap_val_t v; v.u = level; hap_char_update_val(s_rsBattLevel, &v);
        }
    }
    if (s_rsBattLow) {
        const hap_val_t *cur = hap_char_get_val(s_rsBattLow);
        if (!cur || cur->u != low) {
            hap_val_t v; v.u = low; hap_char_update_val(s_rsBattLow, &v);
            LOG_INFO("[HK:Sensor] battery=%u%% low=%u", level, low);
        }
    }
}

#endif // BLE_ENABLE
