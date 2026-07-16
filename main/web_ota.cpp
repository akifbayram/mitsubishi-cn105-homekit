#include "web_server.h"
#include "event_log.h"
#include <esp_task_wdt.h>
#include <mbedtls/sha256.h>
#include <esp_app_desc.h>
#include <esp_partition.h>
#include "status_led.h"
#include "board_profile.h"
#include "ota_guard.h"

static const char *TAG = "web_ota";

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
    // which can block for several seconds on large partitions.
    esp_task_wdt_config_t wdt_ota = { .timeout_ms = 30000, .idle_core_mask = 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&wdt_ota);

    esp_ota_handle_t otaHandle;
    esp_err_t err = esp_ota_begin(partition, totalLen, &otaHandle);
    if (err != ESP_OK) {
        LOG_ERROR("esp_ota_begin failed: %s", esp_err_to_name(err));
        esp_task_wdt_config_t wdt_default = { .timeout_ms = 10000, .idle_core_mask = 1, .trigger_panic = true };
        esp_task_wdt_reconfigure(&wdt_default);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

#if PIN_LED_DATA >= 0
    statusLED.setState(SLED_OTA);
#endif

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

    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);  // 0 = SHA-256 (not SHA-224)

    // Stream firmware in chunks
    char *buf = (char *)malloc(4096);
    if (!buf) {
        mbedtls_sha256_free(&sha256_ctx);
        esp_ota_abort(otaHandle);
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
            mbedtls_sha256_free(&sha256_ctx);
            free(buf);
            esp_ota_abort(otaHandle);
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
                    mbedtls_sha256_free(&sha256_ctx);
                    free(buf);
                    esp_ota_abort(otaHandle);
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
            mbedtls_sha256_free(&sha256_ctx);
            free(buf);
            esp_ota_abort(otaHandle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        received += ret;
        mbedtls_sha256_update(&sha256_ctx, (const unsigned char *)buf, ret);

        // Progress update every ~64KB
        if ((received % 65536) < 4096) {
            uint8_t pct = (uint8_t)((received * 100) / totalLen);
            LOG_INFO("Progress: %u/%u bytes (%u%%)", (unsigned)received, (unsigned)totalLen, pct);
        }
    }

    free(buf);

    // Upload ended before the identity header completed — not a real image.
    if (!guardPassed) {
        mbedtls_sha256_free(&sha256_ctx);
        esp_ota_abort(otaHandle);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ota_guard_message(OTA_GUARD_SHORT));
        return ESP_FAIL;
    }

    // Finalize SHA256
    unsigned char hash[32];
    mbedtls_sha256_finish(&sha256_ctx, hash);
    mbedtls_sha256_free(&sha256_ctx);

    if (verifyHash) {
        char computed[65];
        for (int i = 0; i < 32; i++) {
            sprintf(computed + i * 2, "%02x", hash[i]);
        }
        computed[64] = '\0';

        if (strcmp(computed, expectedHash) != 0) {
            LOG_ERROR("SHA256 mismatch! Expected: %.16s... Got: %.16s...",
                      expectedHash, computed);
            esp_ota_abort(otaHandle);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SHA256 mismatch");
            return ESP_FAIL;
        }
        LOG_INFO("SHA256 verified: %.16s...", computed);
    }

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

    // Restore default WDT timeout
    esp_task_wdt_config_t wdt_default = { .timeout_ms = 10000, .idle_core_mask = 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&wdt_default);

    LOG_INFO("Firmware update successful (%u bytes). Restarting...", (unsigned)received);
    eventlog_append(EV_OTA_INSTALLED);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
