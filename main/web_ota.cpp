#include "web_server.h"
#include "event_log.h"
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <esp_app_desc.h>
#include <esp_partition.h>
#include "ota_guard.h"

static const char *TAG = "web_ota";

// True while an OTA upload is in flight. Read by the main-loop LED policy
// and the button handler; written only from the httpd task.
static volatile bool s_otaActive = false;

bool webota_active() { return s_otaActive; }

// The default task-watchdog timeout the rest of the firmware runs with. The
// erase/flash phase needs longer, but it must go back on EVERY exit: a 30 s
// watchdog left armed after a failed upload triples how long a hung main task
// takes to panic, and the crash-loop -> safe-mode timing depends on it. That
// used to be a manual call on two of ten exits, so the eight failure returns
// (now including wrong-board rejection, which the identity guard makes a
// routine event) silently degraded the watchdog until the next reboot.
static constexpr uint32_t OTA_WDT_MS     = 30000;
static constexpr uint32_t DEFAULT_WDT_MS = 10000;

static void setTaskWdtTimeout(uint32_t ms) {
    esp_task_wdt_config_t cfg = { .timeout_ms = ms, .idle_core_mask = 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&cfg);
}

namespace {

// Scope guards, so every early return unwinds the same way. Order of
// declaration is order of construction; destruction runs in reverse.
struct WdtWindow {
    explicit WdtWindow(uint32_t ms) { setTaskWdtTimeout(ms); }
    ~WdtWindow() { setTaskWdtTimeout(DEFAULT_WDT_MS); }
};

struct Sha256Ctx {
    mbedtls_sha256_context c;
    Sha256Ctx() { mbedtls_sha256_init(&c); mbedtls_sha256_starts(&c, 0); }  // 0 = SHA-256
    ~Sha256Ctx() { mbedtls_sha256_free(&c); }
};

struct HeapBuf {
    char *p;
    explicit HeapBuf(size_t n) : p((char *)malloc(n)) {}
    ~HeapBuf() { free(p); }
};

// Aborts the OTA unless disarmed — disarm once the image is handed to
// esp_ota_end(), which owns the handle from that point on.
struct OtaSession {
    esp_ota_handle_t handle;
    bool armed = true;
    explicit OtaSession(esp_ota_handle_t h) : handle(h) { s_otaActive = true; }
    ~OtaSession() {
        if (armed) esp_ota_abort(handle);
        s_otaActive = false;
    }
    void disarm() { armed = false; }
};

}  // namespace

// chip_id of the RUNNING image — self-maintaining: no per-chip constant table
// to keep in sync with new board profiles.
// 0xFFFF = unknown → ota_guard_check skips the chip comparison (fail open).
static uint16_t runningChipId() {
    const esp_partition_t *run = esp_ota_get_running_partition();
    uint8_t hdr[14];
    if (run && esp_partition_read(run, 0, hdr, sizeof(hdr)) == ESP_OK)
        return ota_guard_chip_id(hdr);
    return 0xFFFF;
}

// ══════════════════════════════════════════════════════════════════════════════
// OTA firmware upload: POST /upload  (raw binary body, not multipart)
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::handleOtaUpload(httpd_req_t *req) {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        LOG_ERROR("No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    size_t totalLen = req->content_len;
    if (totalLen == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }
    if (totalLen > partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware too large");
        return ESP_FAIL;
    }

    LOG_INFO("Starting firmware upload: %u bytes -> partition '%s'",
             (unsigned)totalLen, partition->label);

    // OTA progress is shown client-side via the XHR upload (xhr.upload.onprogress)
    // and completion via the HTTP 200 response — deliberately no WS frames from
    // this long-running handler (sending them stalls recv and can trip the OTA WDT).

    // Increase WDT timeout during OTA — esp_ota_begin() erases the partition
    // which can block for several seconds on large partitions. The guard puts
    // it back on every exit from here down.
    WdtWindow wdtWindow(OTA_WDT_MS);

    esp_ota_handle_t otaHandle;
    esp_err_t err = esp_ota_begin(partition, totalLen, &otaHandle);
    if (err != ESP_OK) {
        LOG_ERROR("esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    OtaSession ota(otaHandle);

    // SHA256 verification: check for X-Firmware-SHA256 header
    char expectedHash[65] = {0};
    bool verifyHash = false;
    {
        size_t hdrLen = httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256");
        if (hdrLen == 64) {
            httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", expectedHash, sizeof(expectedHash));
            verifyHash = true;
            LOG_INFO("SHA256 verification enabled");
        }
    }

    Sha256Ctx sha;

    // Stream firmware in chunks
    HeapBuf bufOwner(4096);
    char *buf = bufOwner.p;
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    // Identity guard state: accumulate the first 288 bytes (image header +
    // app descriptor) across recv() calls, then verify chip_id/project_name
    // before accepting the rest. Replaces the old single-byte 0xE9 check.
    uint8_t hdrBuf[OTA_GUARD_HDR_LEN];
    size_t hdrFill = 0;
    bool guardPassed = false;
    const uint16_t expChip = runningChipId();
    const char *expProject = esp_app_get_description()->project_name;

    size_t received = 0;
    while (received < totalLen) {
        int ret = httpd_req_recv(req, buf, 4096);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            LOG_ERROR("Receive error at %u/%u bytes", (unsigned)received, (unsigned)totalLen);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        // Identity guard on the first 288 bytes (magic byte, chip_id,
        // project_name) — rejects wrong-chip and wrong-app images (e.g. Dial
        // firmware onto a unit: same esp32s3 chip, different project).
        if (!guardPassed) {
            size_t need = sizeof(hdrBuf) - hdrFill;
            size_t take = ((size_t)ret < need) ? (size_t)ret : need;
            memcpy(hdrBuf + hdrFill, buf, take);
            hdrFill += take;
            if (hdrFill == sizeof(hdrBuf)) {
                OtaGuardVerdict v = ota_guard_check(hdrBuf, hdrFill, expChip, expProject);
                if (v != OTA_GUARD_OK) {
                    LOG_ERROR("Firmware rejected (%d): %s", (int)v, ota_guard_message(v));
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ota_guard_message(v));
                    return ESP_FAIL;
                }
                guardPassed = true;
                LOG_INFO("Firmware identity OK (chip 0x%04X, project '%s')", expChip, expProject);
            }
        }

        err = esp_ota_write(otaHandle, buf, ret);
        if (err != ESP_OK) {
            LOG_ERROR("esp_ota_write failed at %u bytes: %s",
                      (unsigned)received, esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        received += ret;
        mbedtls_sha256_update(&sha.c, (const unsigned char *)buf, ret);

        // Progress update every ~64KB
        if ((received % 65536) < 4096) {
            uint8_t pct = (uint8_t)((received * 100) / totalLen);
            LOG_INFO("Progress: %u/%u bytes (%u%%)", (unsigned)received, (unsigned)totalLen, pct);
        }
    }

    // Upload ended before the identity header completed — not a real image.
    if (!guardPassed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ota_guard_message(OTA_GUARD_SHORT));
        return ESP_FAIL;
    }

    // Finalize SHA256
    unsigned char hash[32];
    mbedtls_sha256_finish(&sha.c, hash);

    if (verifyHash) {
        char computed[65];
        for (int i = 0; i < 32; i++) {
            sprintf(computed + i * 2, "%02x", hash[i]);
        }
        computed[64] = '\0';

        if (strcmp(computed, expectedHash) != 0) {
            LOG_ERROR("SHA256 mismatch! Expected: %.16s... Got: %.16s...",
                      expectedHash, computed);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SHA256 mismatch");
            return ESP_FAIL;
        }
        LOG_INFO("SHA256 verified: %.16s...", computed);
    }

    // esp_ota_end() takes ownership of the handle and frees it on success AND
    // on failure, so the abort guard must stand down before the call, not
    // after a successful one.
    ota.disarm();
    err = esp_ota_end(otaHandle);
    if (err != ESP_OK) {
        LOG_ERROR("esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        LOG_ERROR("esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    LOG_INFO("Firmware update successful (%u bytes). Restarting...", (unsigned)received);
    eventlog_append(EV_OTA_INSTALLED);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
