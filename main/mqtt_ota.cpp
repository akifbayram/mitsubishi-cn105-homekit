#include "mqtt_ota.h"

#ifdef MQTT_ENABLE

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "logging.h"
#include "ota_writer.h"
#include "mqtt_app.h"

static const char *TAG = "mqtt_ota";

struct OtaParams {
    char url[256];
    char sha[65];
};

static void publishStatusf(const char *fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    MqttClient::publishOtaStatus(buf);
}

// Runs in its own short-lived task — the esp-mqtt event callback must not
// block for the minutes a download can take.
static void otaTask(void *arg) {
    OtaParams *p = (OtaParams *)arg;
    // All declarations hoisted to before the first goto to avoid C++ "jump
    // to label crosses initialization" errors.
    esp_http_client_handle_t client = nullptr;
    char *buf = nullptr;
    OtaWriter writer;
    bool writerActive = false;
    int64_t contentLen = 0;

    LOG_INFO("OTA download starting: %s", p->url);
    publishStatusf("{\"state\":\"downloading\",\"pct\":0}");

    esp_http_client_config_t hc = {};
    hc.url = p->url;
    hc.crt_bundle_attach = esp_crt_bundle_attach;  // HTTPS incl. GitHub releases
    hc.timeout_ms = 15000;
    hc.buffer_size = 4096;
    client = esp_http_client_init(&hc);
    if (!client) {
        publishStatusf("{\"state\":\"error\",\"reason\":\"http client init failed\"}");
        goto fail;
    }

    // open()+read() streaming bypasses perform()'s automatic redirect
    // handling, so follow redirects manually (GitHub release assets redirect
    // to a CDN URL).
    {
        int status = 0;
        int redirects = 0;
        while (true) {
            esp_err_t err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                publishStatusf("{\"state\":\"error\",\"reason\":\"connect failed: %s\"}",
                               esp_err_to_name(err));
                goto fail;
            }
            esp_http_client_fetch_headers(client);
            status = esp_http_client_get_status_code(client);
            if (status == 301 || status == 302 || status == 303 ||
                status == 307 || status == 308) {
                if (++redirects > 5) {
                    publishStatusf("{\"state\":\"error\",\"reason\":\"too many redirects\"}");
                    goto fail;
                }
                esp_http_client_set_redirection(client);
                esp_http_client_close(client);
                continue;
            }
            break;
        }
        if (status != 200) {
            publishStatusf("{\"state\":\"error\",\"reason\":\"HTTP %d\"}", status);
            goto fail;
        }
    }

    contentLen = esp_http_client_get_content_length(client);  // -1 if chunked
    {
        esp_err_t err = writer.begin(contentLen > 0 ? (size_t)contentLen : 0, p->sha);
        if (err != ESP_OK) {
            publishStatusf("{\"state\":\"error\",\"reason\":\"%s\"}", writer.errorMsg());
            goto fail;
        }
        writerActive = true;

        buf = (char *)malloc(4096);
        if (!buf) {
            publishStatusf("{\"state\":\"error\",\"reason\":\"out of memory\"}");
            goto fail;
        }

        int lastPct = 0;
        while (true) {
            int r = esp_http_client_read(client, buf, 4096);
            if (r < 0) {
                publishStatusf("{\"state\":\"error\",\"reason\":\"download failed at %u bytes\"}",
                               (unsigned)writer.received());
                goto fail;
            }
            if (r == 0) break;  // download complete

            err = writer.write((const uint8_t *)buf, r);
            if (err != ESP_OK) {
                publishStatusf("{\"state\":\"error\",\"reason\":\"%s\"}", writer.errorMsg());
                goto fail;
            }

            if (contentLen > 0) {
                int pct = (int)((writer.received() * 100) / (size_t)contentLen);
                if (pct >= lastPct + 10) {
                    lastPct = pct;
                    LOG_INFO("OTA download: %d%% (%u bytes)", pct, (unsigned)writer.received());
                    publishStatusf("{\"state\":\"downloading\",\"pct\":%d}", pct);
                }
            }
        }
    }

    publishStatusf("{\"state\":\"verifying\"}");
    if (writer.finish() != ESP_OK) {
        writerActive = false;  // finish() already aborted/cleaned up
        publishStatusf("{\"state\":\"error\",\"reason\":\"%s\"}", writer.errorMsg());
        goto fail;
    }

    LOG_INFO("OTA install successful (%u bytes). Restarting...", (unsigned)writer.received());
    publishStatusf("{\"state\":\"rebooting\"}");

    free(buf);
    esp_http_client_cleanup(client);
    free(p);
    vTaskDelay(pdMS_TO_TICKS(1500));  // let the status publish flush
    esp_restart();

fail:
    if (writerActive) writer.abort();
    if (buf) free(buf);
    if (client) esp_http_client_cleanup(client);
    free(p);
    OtaWriter::release();
    LOG_ERROR("OTA install failed — running image unchanged");
    vTaskDelete(NULL);
}

bool MqttOta::start(const char *url, const char *sha256) {
    if (!OtaWriter::tryAcquire()) {
        LOG_WARN("OTA install rejected — another update is in progress");
        MqttClient::publishOtaStatus(
            "{\"state\":\"error\",\"reason\":\"update already in progress\"}");
        return false;
    }

    OtaParams *p = (OtaParams *)malloc(sizeof(OtaParams));
    if (!p) {
        OtaWriter::release();
        return false;
    }
    strncpy(p->url, url, sizeof(p->url) - 1);
    p->url[sizeof(p->url) - 1] = '\0';
    strncpy(p->sha, sha256, sizeof(p->sha) - 1);
    p->sha[sizeof(p->sha) - 1] = '\0';

    if (xTaskCreate(otaTask, "mqtt_ota", 8192, p, 5, NULL) != pdPASS) {
        LOG_ERROR("Failed to create OTA task");
        free(p);
        OtaWriter::release();
        MqttClient::publishOtaStatus(
            "{\"state\":\"error\",\"reason\":\"task create failed\"}");
        return false;
    }
    return true;
}

#endif // MQTT_ENABLE
