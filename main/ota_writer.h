#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

// Shared streaming OTA writer used by the HTTP upload handler (web_ota.cpp)
// and the MQTT-triggered download (mqtt_ota.cpp). Exactly one OTA may run at
// a time across all entry points — call tryAcquire() before begin() and
// release() after the writer is done (abort or non-rebooting failure).
//
// Verification contract: when expectedSha256 is provided, the new image is
// only marked bootable (esp_ota_set_boot_partition) after the computed hash
// matches. A nullptr sha256 skips verification (legacy curl upload path).
class OtaWriter {
public:
    // expectedSha256: 64-char hex string, or nullptr to skip verification.
    // expectedSize: total image size, or 0 if unknown (chunked HTTP download).
    esp_err_t begin(size_t expectedSize, const char *expectedSha256);

    // Streams one chunk. First chunk must start with the 0xE9 image magic
    // byte (returns ESP_ERR_INVALID_ARG otherwise).
    esp_err_t write(const uint8_t *data, size_t len);

    // Verify SHA256 (if enabled), validate image, set boot partition.
    // Returns ESP_ERR_INVALID_CRC on hash mismatch.
    esp_err_t finish();

    // Abort an in-progress write. Safe no-op if begin() failed or finish() ran.
    void abort();

    size_t received() const { return _received; }
    const char *errorMsg() const { return _err; }

    static bool tryAcquire();
    static void release();

private:
    void restoreWdt();

    esp_ota_handle_t _handle = 0;
    const esp_partition_t *_partition = nullptr;
    mbedtls_sha256_context _sha;
    char _expected[65] = {0};
    bool _verify = false;
    bool _active = false;
    bool _firstChunk = true;
    size_t _received = 0;
    const char *_err = "";
};
