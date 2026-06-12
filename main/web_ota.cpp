#include "web_server.h"
#include "ota_writer.h"
#include "logging.h"

static const char *TAG = "web_ota";

// ══════════════════════════════════════════════════════════════════════════════
// OTA firmware upload: POST /upload  (raw binary body, not multipart)
// Streaming/verify logic lives in OtaWriter (shared with MQTT-triggered OTA).
// ══════════════════════════════════════════════════════════════════════════════

esp_err_t WebUI::handleOtaUpload(httpd_req_t *req) {
    size_t totalLen = req->content_len;
    if (totalLen == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
        return ESP_FAIL;
    }

    if (!OtaWriter::tryAcquire()) {
        LOG_WARN("OTA upload rejected — another update is in progress");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Update already in progress");
        return ESP_FAIL;
    }

    // SHA256 verification: check for X-Firmware-SHA256 header
    char expectedHash[65] = {0};
    const char *sha = nullptr;
    if (httpd_req_get_hdr_value_len(req, "X-Firmware-SHA256") == 64) {
        httpd_req_get_hdr_value_str(req, "X-Firmware-SHA256", expectedHash, sizeof(expectedHash));
        sha = expectedHash;
    }

    LOG_INFO("Starting firmware upload: %u bytes", (unsigned)totalLen);
    char otaMsg[128];
    snprintf(otaMsg, sizeof(otaMsg),
        "{\"type\":\"ota\",\"status\":\"starting\",\"size\":%u}", (unsigned)totalLen);
    webUI.sendWsText(webUI._wsClientFd, otaMsg);

    OtaWriter writer;
    esp_err_t err = writer.begin(totalLen, sha);
    if (err != ESP_OK) {
        OtaWriter::release();
        httpd_resp_send_err(req,
            (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_ARG)
                ? HTTPD_400_BAD_REQUEST : HTTPD_500_INTERNAL_SERVER_ERROR,
            writer.errorMsg());
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(4096);
    if (!buf) {
        writer.abort();
        OtaWriter::release();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < totalLen) {
        int ret = httpd_req_recv(req, buf, 4096);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            LOG_ERROR("Receive error at %u/%u bytes", (unsigned)received, (unsigned)totalLen);
            free(buf);
            writer.abort();
            OtaWriter::release();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }

        err = writer.write((const uint8_t *)buf, ret);
        if (err != ESP_OK) {
            free(buf);
            writer.abort();
            OtaWriter::release();
            httpd_resp_send_err(req,
                err == ESP_ERR_INVALID_ARG ? HTTPD_400_BAD_REQUEST
                                           : HTTPD_500_INTERNAL_SERVER_ERROR,
                writer.errorMsg());
            return ESP_FAIL;
        }

        received += ret;

        // Progress update every ~64KB
        if ((received % 65536) < 4096) {
            uint8_t pct = (uint8_t)((received * 100) / totalLen);
            LOG_INFO("Progress: %u/%u bytes (%u%%)", (unsigned)received, (unsigned)totalLen, pct);
            snprintf(otaMsg, sizeof(otaMsg),
                "{\"type\":\"ota\",\"status\":\"progress\",\"pct\":%u}", pct);
            webUI.sendWsText(webUI._wsClientFd, otaMsg);
        }
    }

    free(buf);

    err = writer.finish();
    if (err != ESP_OK) {
        OtaWriter::release();
        httpd_resp_send_err(req,
            err == ESP_ERR_INVALID_CRC ? HTTPD_400_BAD_REQUEST
                                       : HTTPD_500_INTERNAL_SERVER_ERROR,
            writer.errorMsg());
        return ESP_FAIL;
    }

    LOG_INFO("Firmware update successful (%u bytes). Restarting...", (unsigned)received);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"success\"}");

    snprintf(otaMsg, sizeof(otaMsg),
        "{\"type\":\"ota\",\"status\":\"done\",\"pct\":100}");
    webUI.sendWsText(webUI._wsClientFd, otaMsg);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
