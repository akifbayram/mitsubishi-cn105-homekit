#include "time_sync.h"

#include <ctime>
#include <esp_netif_sntp.h>
#include "logging.h"

static const char *TAG = "time";

static bool s_started = false;

void time_sync_start() {
    if (s_started) return;
    s_started = true;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;             // begin polling immediately
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        LOG_WARN("SNTP init failed: %s (timestamps stay boot-relative)",
                 esp_err_to_name(err));
        return;
    }
    LOG_INFO("SNTP started (pool.ntp.org)");
}

uint32_t time_sync_epoch() {
    // Anything before 2020-01-01 means the clock was never set.
    time_t t = time(nullptr);
    return (t > (time_t)1577836800) ? (uint32_t)t : 0;
}

bool time_sync_valid() {
    return time_sync_epoch() != 0;
}
