#include "ota_writer.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_task_wdt.h>

#include "logging.h"
#include "board_profile.h"
#include "status_led.h"

static const char *TAG = "ota_writer";

#if PIN_LED_DATA >= 0
extern StatusLED statusLED;
#endif

static std::atomic<bool> s_otaInProgress{false};

bool OtaWriter::tryAcquire() {
    bool expected = false;
    return s_otaInProgress.compare_exchange_strong(expected, true);
}

void OtaWriter::release() {
    s_otaInProgress = false;
}

static void setWdtTimeout(uint32_t ms) {
    esp_task_wdt_config_t cfg = { .timeout_ms = ms, .idle_core_mask = 1, .trigger_panic = true };
    esp_task_wdt_reconfigure(&cfg);
}

void OtaWriter::restoreWdt() {
    setWdtTimeout(10000);
}

esp_err_t OtaWriter::begin(size_t expectedSize, const char *expectedSha256) {
    _partition = esp_ota_get_next_update_partition(NULL);
    if (!_partition) {
        _err = "No OTA partition";
        LOG_ERROR("%s", _err);
        return ESP_FAIL;
    }
    if (expectedSize > _partition->size) {
        _err = "Firmware too large";
        LOG_ERROR("%s: %u > %lu", _err, (unsigned)expectedSize, (unsigned long)_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (expectedSha256) {
        if (strlen(expectedSha256) != 64) {
            _err = "Bad sha256 length";
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(_expected, expectedSha256, 64);
        _expected[64] = '\0';
        _verify = true;
        LOG_INFO("SHA256 verification enabled");
    }

    // esp_ota_begin() erases the partition, which can block for several seconds.
    setWdtTimeout(30000);

    esp_err_t err = esp_ota_begin(_partition,
                                  expectedSize ? expectedSize : OTA_SIZE_UNKNOWN,
                                  &_handle);
    if (err != ESP_OK) {
        restoreWdt();
        _err = "OTA begin failed";
        LOG_ERROR("esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    mbedtls_sha256_init(&_sha);
    mbedtls_sha256_starts(&_sha, 0);  // 0 = SHA-256 (not SHA-224)
    _active = true;
    _firstChunk = true;
    _received = 0;

#if PIN_LED_DATA >= 0
    statusLED.setState(SLED_OTA);
#endif
    LOG_INFO("OTA begin -> partition '%s' (%u bytes expected)",
             _partition->label, (unsigned)expectedSize);
    return ESP_OK;
}

esp_err_t OtaWriter::write(const uint8_t *data, size_t len) {
    if (!_active || len == 0) return ESP_ERR_INVALID_STATE;

    if (_firstChunk) {
        if (data[0] != 0xE9) {
            _err = "Invalid firmware file";
            LOG_ERROR("Invalid firmware magic byte: 0x%02X (expected 0xE9)", data[0]);
            return ESP_ERR_INVALID_ARG;
        }
        _firstChunk = false;
    }

    esp_err_t err = esp_ota_write(_handle, data, len);
    if (err != ESP_OK) {
        _err = "Flash write failed";
        LOG_ERROR("esp_ota_write failed at %u bytes: %s",
                  (unsigned)_received, esp_err_to_name(err));
        return err;
    }

    mbedtls_sha256_update(&_sha, data, len);
    _received += len;
    return ESP_OK;
}

esp_err_t OtaWriter::finish() {
    if (!_active) return ESP_ERR_INVALID_STATE;

    unsigned char hash[32];
    mbedtls_sha256_finish(&_sha, hash);
    mbedtls_sha256_free(&_sha);

    if (_verify) {
        char computed[65];
        for (int i = 0; i < 32; i++) {
            sprintf(computed + i * 2, "%02x", hash[i]);
        }
        computed[64] = '\0';
        if (strcasecmp(computed, _expected) != 0) {
            LOG_ERROR("SHA256 mismatch! Expected: %.16s... Got: %.16s...",
                      _expected, computed);
            _err = "SHA256 mismatch";
            esp_ota_abort(_handle);
            _active = false;
            restoreWdt();
            return ESP_ERR_INVALID_CRC;
        }
        LOG_INFO("SHA256 verified: %.16s...", computed);
    }

    esp_err_t err = esp_ota_end(_handle);
    _active = false;
    if (err != ESP_OK) {
        _err = "Image validation failed";
        LOG_ERROR("esp_ota_end failed: %s", esp_err_to_name(err));
        restoreWdt();
        return err;
    }

    err = esp_ota_set_boot_partition(_partition);
    if (err != ESP_OK) {
        _err = "Set boot partition failed";
        LOG_ERROR("esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        restoreWdt();
        return err;
    }

    restoreWdt();
    LOG_INFO("OTA complete (%u bytes)", (unsigned)_received);
    return ESP_OK;
}

void OtaWriter::abort() {
    if (!_active) return;
    mbedtls_sha256_free(&_sha);
    esp_ota_abort(_handle);
    _active = false;
    restoreWdt();
#if PIN_LED_DATA >= 0
    // Neutral handoff back to the main-loop priority chain, which only skips
    // re-evaluation while state == SLED_OTA — it reclassifies on the next tick.
    statusLED.setState(SLED_OFF);
#endif
}
