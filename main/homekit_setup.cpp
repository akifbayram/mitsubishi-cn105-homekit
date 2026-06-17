#include "homekit_setup.h"
#include "homekit_services.h"
#include "homekit_sensor_accessory.h"
#include "settings.h"
#include "logging.h"
#include "branding.h"
#include <cstring>
#include <cstdio>

#include <esp_event.h>
#include <esp_random.h>
#include <nvs_flash.h>
#include <mdns.h>

extern "C" {
#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>
}

static const char *TAG = "hk_setup";

// ── State ───────────────────────────────────────────────────────────────────
static char s_setupCode[12] = "000-00-000";    // XXX-XX-XXX + null
static char* s_setupPayload = nullptr;          // allocated by esp_hap_get_setup_payload
static const char* s_statusString = "Not Ready";
static hap_cid_t s_cid = HAP_CID_BRIDGE;

// ── HAP event handler (esp_event style) ─────────────────────────────────────

static void hap_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event, void* data)
{
    switch (event) {
        case HAP_EVENT_PAIRING_STARTED:
            LOG_INFO("[HK] Pairing started");
            s_statusString = "Pairing";
            break;
        case HAP_EVENT_PAIRING_ABORTED:
            LOG_INFO("[HK] Pairing aborted");
            s_statusString = (hap_get_paired_controller_count() > 0) ? "Paired" : "Ready";
            break;
        case HAP_EVENT_CTRL_PAIRED:
            LOG_INFO("[HK] Controller paired: %s (count=%d)",
                     data ? (char*)data : "?", hap_get_paired_controller_count());
            s_statusString = "Paired";
            break;
        case HAP_EVENT_CTRL_UNPAIRED:
            LOG_INFO("[HK] Controller unpaired: %s (count=%d)",
                     data ? (char*)data : "?", hap_get_paired_controller_count());
            s_statusString = (hap_get_paired_controller_count() > 0) ? "Paired" : "Ready";
            break;
        case HAP_EVENT_CTRL_CONNECTED:
            LOG_INFO("[HK] Controller connected: %s", data ? (char*)data : "?");
            s_statusString = "Connected";
            break;
        case HAP_EVENT_CTRL_DISCONNECTED:
            LOG_INFO("[HK] Controller disconnected: %s", data ? (char*)data : "?");
            s_statusString = (hap_get_paired_controller_count() > 0) ? "Paired" : "Ready";
            break;
        case HAP_EVENT_ACC_REBOOTING:
            LOG_INFO("[HK] Accessory rebooting (reason: %s)", data ? (char*)data : "?");
            break;
        case HAP_EVENT_PAIRING_MODE_TIMED_OUT:
            LOG_WARN("[HK] Pairing mode timed out");
            s_statusString = "Timed Out";
            break;
        default:
            break;
    }
}

// Mandatory identify routine for the accessory
static int accessory_identify(hap_acc_t *ha)
{
    LOG_INFO("[HK] Accessory identified");
    return HAP_SUCCESS;
}

static void homekit_set_setup_id(const char* setupId)
{
    if (setupId) {
        hap_set_setup_id(setupId);
    }
}

// ── Public API ──────────────────────────────────────────────────────────────

bool homekit_init(const char* name, const char* manufacturer,
                  const char* model, const char* serialNumber,
                  const char* fwRevision, const char* mdnsHostname)
{
    // main.cpp retries this function every loop iteration on failure, so each
    // one-shot step (hap_init, accessory creation, handler registration) is
    // guarded by a progress flag — re-running them would duplicate accessories
    // in the HAP database.
    static bool s_coreInited        = false;
    static bool s_bridgeAdded       = false;
    static bool s_acAdded           = false;
    static bool s_handlerRegistered = false;

    if (!s_coreInited) {
        hap_cfg_t hap_cfg;
        hap_get_config(&hap_cfg);
        // UNIQUE_NONE: don't let the SDK append a MAC suffix to the accessory
        // name — the name passed in (displayName) already carries one.
        hap_cfg.unique_param = UNIQUE_NONE;
        // Notification queue depth (SDK default 8, sized for standalone
        // accessories). This bridge has ~23 evented characteristics and a
        // CN105-recovery force-sync updates more than 8 in one tick; on queue
        // overflow the SDK drops the notification silently and controllers
        // show stale state.
        hap_cfg.max_event_notif_chars = 24;
        hap_set_config(&hap_cfg);

        // Initialize HAP core
        int ret = hap_init(HAP_TRANSPORT_WIFI);
        if (ret != HAP_SUCCESS) {
            LOG_ERROR("[HK] hap_init failed: %d", ret);
            return false;
        }
        s_coreInited = true;
    }

    // Generate / load setup code before creating accessory
    homekit_generate_setup_code();
    homekit_set_setup_id(BRAND_QR_ID);

    // ── Bridge (primary) accessory ──────────────────────────────────────────
    if (!s_bridgeAdded) {
        uint8_t product_data[] = {'M','C','A','C','H','A','P','1'};
        hap_acc_cfg_t bridgeCfg = {
            .name             = const_cast<char*>(name),
            .model            = const_cast<char*>(model),
            .manufacturer     = const_cast<char*>(manufacturer),
            .serial_num       = const_cast<char*>(serialNumber),
            .fw_rev           = const_cast<char*>(fwRevision),
            .hw_rev           = nullptr,
            .pv               = const_cast<char*>("1.1.0"),
            .cid              = HAP_CID_BRIDGE,
            .identify_routine = accessory_identify,
        };
        hap_acc_t *bridge = hap_acc_create(&bridgeCfg);
        if (!bridge) {
            LOG_ERROR("[HK] bridge hap_acc_create failed");
            return false;
        }
        hap_acc_add_product_data(bridge, product_data, sizeof(product_data));
        hap_acc_add_wifi_transport_service(bridge, 0);
        hap_add_accessory(bridge);
        s_bridgeAdded = true;
    }

    // ── Air Conditioner (bridged) accessory ─────────────────────────────────
    // Unique SerialNumber per accessory (HAP spec): the bridge keeps the base
    // serial, so the AC gets an "-ac" suffix. hap_acc_create strdup's the string,
    // so a stack buffer is fine. The AID still derives from the base serialNumber
    // below, so accessory identity is unchanged.
    if (!s_acAdded) {
        char acSerial[24];
        snprintf(acSerial, sizeof(acSerial), "%s-ac", serialNumber);
        hap_acc_cfg_t acCfg = {
            .name             = const_cast<char*>("Air Conditioner"),
            .model            = const_cast<char*>(model),
            .manufacturer     = const_cast<char*>(manufacturer),
            .serial_num       = acSerial,
            .fw_rev           = const_cast<char*>(fwRevision),
            .hw_rev           = nullptr,
            .pv               = const_cast<char*>("1.1.0"),
            .cid              = HAP_CID_AIR_CONDITIONER,
            .identify_routine = accessory_identify,
        };
        hap_acc_t *acAcc = hap_acc_create(&acCfg);
        if (!acAcc) {
            LOG_ERROR("[HK] AC hap_acc_create failed");
            return false;
        }
        homekit_services_create_all(acAcc);
        hap_add_bridged_accessory(acAcc, hap_get_unique_aid(serialNumber));
        s_acAdded = true;
    }
#ifdef BLE_ENABLE
    // adds the Remote Sensor bridged accessory if configured
    // (internally guarded against re-creation)
    homekit_sensor_begin(serialNumber, fwRevision);
#endif

    // Register event handler
    if (!s_handlerRegistered) {
        esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID, &hap_event_handler, nullptr);
        s_handlerRegistered = true;
    }

    // Generate setup payload (QR code URI)
    if (s_setupPayload) {
        free(s_setupPayload);
        s_setupPayload = nullptr;
    }
    s_setupPayload = esp_hap_get_setup_payload(
        s_setupCode, const_cast<char*>(BRAND_QR_ID), false, s_cid);

    // Start HAP (binds CONFIG_HAP_HTTP_SERVER_PORT, 8080; advertised via mDNS)
    int ret = hap_start();
    if (ret != HAP_SUCCESS) {
        LOG_ERROR("[HK] hap_start failed: %d", ret);
        return false;
    }

    // Override the hardcoded "MyHost" mDNS hostname from esp-homekit-sdk
    // with our unique per-device hostname (e.g. "Serin-AB12")
    if (mdnsHostname && mdnsHostname[0] != '\0') {
        mdns_hostname_set(mdnsHostname);
        LOG_INFO("[HK] mDNS hostname set to %s", mdnsHostname);
    }

    // Set initial status
    if (hap_get_paired_controller_count() > 0) {
        s_statusString = "Paired";
    } else {
        s_statusString = "Ready";
    }

    LOG_INFO("[HK] HomeKit initialized (code: %s, QR: %s, controllers: %d)",
             s_setupCode,
             s_setupPayload ? s_setupPayload : "N/A",
             hap_get_paired_controller_count());

    return true;
}

void homekit_generate_setup_code(void)
{
    // Check if NVS already has a code
    const DeviceSettings &cfg = settings.get();
    if (cfg.setupCode[0] != '\0' && strlen(cfg.setupCode) == 8) {
        // Format existing 8-digit code as XXX-XX-XXX
        snprintf(s_setupCode, sizeof(s_setupCode), "%.3s-%.2s-%.3s",
                 cfg.setupCode, cfg.setupCode + 3, cfg.setupCode + 5);
        hap_set_setup_code(s_setupCode);
        LOG_INFO("[HK] Loaded setup code from NVS: %s", s_setupCode);
        return;
    }

    // Generate random 8-digit code
    // HAP spec forbids these codes: 000-00-000, 111-11-111, ..., 999-99-999,
    // 123-45-678 and 876-54-321
    char digits[9];
    do {
        uint32_t r = esp_random();
        snprintf(digits, sizeof(digits), "%08lu", (unsigned long)(r % 100000000UL));
    } while (
        (digits[0] == digits[1] && digits[1] == digits[2] &&
         digits[2] == digits[3] && digits[3] == digits[4] &&
         digits[4] == digits[5] && digits[5] == digits[6] &&
         digits[6] == digits[7]) ||
        strcmp(digits, "12345678") == 0 ||
        strcmp(digits, "87654321") == 0
    );

    // Save raw 8-digit code to NVS
    memcpy(settings.get().setupCode, digits, 8);
    settings.get().setupCode[8] = '\0';
    settings.save();

    // Format as XXX-XX-XXX
    snprintf(s_setupCode, sizeof(s_setupCode), "%.3s-%.2s-%.3s",
             digits, digits + 3, digits + 5);
    hap_set_setup_code(s_setupCode);
    LOG_INFO("[HK] Generated new setup code: %s", s_setupCode);
}

const char* homekit_get_setup_payload(void)
{
    return s_setupPayload ? s_setupPayload : "";
}

const char* homekit_get_setup_code(void)
{
    return s_setupCode;
}

int homekit_get_controller_count(void)
{
    return hap_get_paired_controller_count();
}

void homekit_reset_pairings(void)
{
    LOG_WARN("[HK] Resetting all pairings");
    hap_reset_pairings();
}

const char* homekit_get_status_string(void)
{
    return s_statusString;
}
