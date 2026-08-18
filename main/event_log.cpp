#include "event_log.h"

#include <ctime>
#include <cstring>
#include <cstdio>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "logging.h"
#include "esp_utils.h"
#include "json_utils.h"
#include "time_sync.h"

static const char *TAG = "events";

static constexpr const char *NVS_NAMESPACE  = "ac-events";
static constexpr const char *KEY_RING       = "ring";
static constexpr const char *KEY_BOOT_COUNT = "bootCount";
static constexpr const char *KEY_CRASH_TOT  = "crashTotal";

static nvs_handle_t s_nvs = 0;
static SemaphoreHandle_t s_mux = nullptr;
static EventBlob s_blob;
static uint32_t s_bootCount = 0;
static uint32_t s_crashTotal = 0;
static bool s_safeMode = false;
static uint16_t s_sessionCount[EV_WIFI_CREDS_CHANGED + 1] = {};

// Stamp + append (RAM only). Caller holds s_mux and calls persistLocked()
// after the last append of the batch — flash commits are the expensive part.
static void appendLocked(EventType type, uint8_t code) {
    EventEntry e{};
    e.bootN   = s_bootCount;
    e.uptimeS = uptime_ms() / 1000;
    e.epoch   = time_sync_epoch();
    e.type    = type;
    e.code    = code;
    eventblob_append(s_blob, e);
    if (type <= EV_WIFI_CREDS_CHANGED) s_sessionCount[type]++;
}

static void persistLocked() {
    if (!s_nvs) return;
    nvs_set_blob(s_nvs, KEY_RING, &s_blob, sizeof(s_blob));
    nvs_commit(s_nvs);
}

void eventlog_init(esp_reset_reason_t reason, bool wasCrash, bool safeMode) {
    s_mux = xSemaphoreCreateMutex();
    s_safeMode = safeMode;

    eventblob_reset(s_blob);
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        LOG_ERROR("nvs_open failed: %s — events won't persist", esp_err_to_name(err));
        s_nvs = 0;
    } else {
        EventBlob loaded;
        size_t len = sizeof(loaded);
        if (nvs_get_blob(s_nvs, KEY_RING, &loaded, &len) == ESP_OK &&
            len == sizeof(loaded) && eventblob_valid(loaded)) {
            s_blob = loaded;
        }
        nvs_get_u32(s_nvs, KEY_BOOT_COUNT, &s_bootCount);
        nvs_get_u32(s_nvs, KEY_CRASH_TOT, &s_crashTotal);
    }

    s_bootCount++;
    if (wasCrash) s_crashTotal++;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_nvs) {
        nvs_set_u32(s_nvs, KEY_BOOT_COUNT, s_bootCount);
        nvs_set_u32(s_nvs, KEY_CRASH_TOT, s_crashTotal);
    }
    appendLocked(EV_BOOT, (uint8_t)reason);
    if (wasCrash) appendLocked(EV_CRASH, (uint8_t)reason);
    if (safeMode) appendLocked(EV_SAFE_MODE, 0);
    persistLocked();   // one flash commit for the whole boot batch
    xSemaphoreGive(s_mux);

    LOG_INFO("Boot #%lu (crashes lifetime: %lu, %u events retained)",
             (unsigned long)s_bootCount, (unsigned long)s_crashTotal,
             (unsigned)s_blob.count);
}

void eventlog_append(EventType type, uint8_t code) {
    if (!s_mux) return;   // before init — nothing to record against yet
    xSemaphoreTake(s_mux, portMAX_DELAY);
    appendLocked(type, code);
    persistLocked();
    xSemaphoreGive(s_mux);
    LOG_INFO("Event: %s (code=%u)", eventlog_type_str(type), (unsigned)code);
}

uint32_t eventlog_boot_count()  { return s_bootCount; }
uint32_t eventlog_crash_total() { return s_crashTotal; }
bool     eventlog_safe_mode()   { return s_safeMode; }

uint32_t eventlog_session_count(EventType type) {
    return (type <= EV_WIFI_CREDS_CHANGED) ? s_sessionCount[type] : 0;
}

int eventlog_json(char *out, size_t cap) {
    if (!s_mux || !out || cap == 0) return 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = snprintf(out, cap, "\"bootCount\":%lu,\"crashTotal\":%lu,\"events\":[",
                     (unsigned long)s_bootCount, (unsigned long)s_crashTotal);
    for (size_t i = 0; ; i++) {
        const EventEntry *e = eventblob_newest(s_blob, i);
        if (!e) break;
        jsonAppend(out, cap, &n,
                   "%s{\"b\":%lu,\"u\":%lu,\"e\":%lu,\"t\":\"%s\",\"c\":%u",
                   i > 0 ? "," : "",
                   (unsigned long)e->bootN, (unsigned long)e->uptimeS,
                   (unsigned long)e->epoch, eventlog_type_str(e->type),
                   (unsigned)e->code);
        // Events whose code byte carries a variant name it here, so the
        // browser never needs its own copy of the esp_reset_reason_t
        // numbering or of what a code byte means.
        if (e->type == EV_BOOT || e->type == EV_CRASH) {
            jsonAppend(out, cap, &n, ",\"r\":\"%s\"",
                       resetReasonStr((esp_reset_reason_t)e->code));
        } else if (e->type == EV_WIFI_CREDS_CHANGED && e->code == 1) {
            jsonAppend(out, cap, &n, ",\"r\":\"erased\"");
        }
        jsonAppend(out, cap, &n, "}");
    }
    jsonAppend(out, cap, &n, "]");
    xSemaphoreGive(s_mux);
    return (n < (int)cap) ? n : 0;
}

const char *eventlog_type_str(uint8_t type) {
    switch (type) {
        case EV_BOOT:               return "BOOT";
        case EV_CRASH:              return "CRASH";
        case EV_SAFE_MODE:          return "SAFE_MODE";
        case EV_OTA_INSTALLED:      return "OTA";
        case EV_CN105_ERROR:        return "CN105_ERROR";
        case EV_CN105_LOST:         return "CN105_LOST";
        case EV_CN105_RESTORED:     return "CN105_OK";
        case EV_RECOVERY_AP:        return "RECOVERY_AP";
        case EV_HK_RESET:           return "HK_RESET";
        case EV_WIFI_CREDS_CHANGED: return "WIFI_CREDS";
        default:                    return "UNKNOWN";
    }
}
