#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

// Shared streaming OTA writer used by the HTTP upload handler (web_ota.cpp)
// and the MQTT-triggered download (mqtt_ota.cpp). Exactly one OTA may run at
// a time across all entry points.
//
// Acquire/release contract: begin() does NOT check the acquire flag itself —
// callers MUST call tryAcquire() first and only proceed if it returns true.
// After a successful tryAcquire(), release() must be called exactly once on
// every non-rebooting path (abort, any failure); success paths that call
// esp_restart() may skip it. A forgotten release() permanently wedges all
// future OTAs until reboot.
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
    // True while an OTA holds the writer lock (between tryAcquire and release).
    // Lets other subsystems avoid tearing down resources an OTA is using.
    static bool isInProgress();

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
