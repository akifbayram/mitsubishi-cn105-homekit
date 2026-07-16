#pragma once
// Pure OTA image identity check — no ESP-IDF deps (host-tested in
// test/ota_guard/). Parses the stable ESP-IDF app-image header format to
// reject firmware built for a different chip or a different application
// BEFORE it is flashed. SHA256 verifies transfer integrity; this verifies
// the file is even meant for this device (a perfectly-transferred Dial
// image passes SHA256 fine — and both unit S3 boards and the Dial are
// esp32s3, indistinguishable at the flasher).
//
// Image layout (esp_image_header_t + esp_image_segment_header_t + esp_app_desc_t):
//   [0]       0xE9 image magic
//   [12..13]  chip_id (LE u16)          — esp_chip_id_t: 0x0000=ESP32, 0x0005=C3, 0x0009=S3, 0x000D=C6
//   [32..35]  app-desc magic 0xABCD5432 — absent for bootloader.bin / merged factory images
//   [80..111] project_name[32]          — CMake project(), NUL-padded
// Residual gap (documented): same chip + same project but a different board
// profile (e.g. ESP32S3_DEVKIT vs M5ATOMS3_LITE) is indistinguishable.

#include <cstdint>
#include <cstddef>
#include <cstring>

inline constexpr size_t OTA_GUARD_HDR_LEN = 288;  // 24 + 8 + 256 (full app desc)

enum OtaGuardVerdict : uint8_t {
    OTA_GUARD_OK = 0,
    OTA_GUARD_SHORT,          // need more bytes — not an error yet
    OTA_GUARD_BAD_MAGIC,      // not an ESP application image
    OTA_GUARD_WRONG_CHIP,     // built for a different chip
    OTA_GUARD_NO_APP_DESC,    // no app descriptor (bootloader/merged image?)
    OTA_GUARD_WRONG_PROJECT,  // a different application (e.g. Dial firmware)
};

// chip_id from an image header (LE u16 at offset 12..13) — the one place the
// offset is encoded; needs at least the first 14 bytes of the image.
inline uint16_t ota_guard_chip_id(const uint8_t *buf) {
    return (uint16_t)((uint16_t)buf[12] | ((uint16_t)buf[13] << 8));
}

// expectedChipId 0xFFFF skips the chip check; expectedProject NULL skips the
// project check (both "fail open" — the guard must never brick a legitimate
// update path just because the running image couldn't be read).
inline OtaGuardVerdict ota_guard_check(const uint8_t *buf, size_t len,
                                       uint16_t expectedChipId,
                                       const char *expectedProject) {
    if (len < OTA_GUARD_HDR_LEN) return OTA_GUARD_SHORT;
    if (buf[0] != 0xE9) return OTA_GUARD_BAD_MAGIC;

    uint16_t chipId = ota_guard_chip_id(buf);
    if (expectedChipId != 0xFFFF && chipId != expectedChipId) return OTA_GUARD_WRONG_CHIP;

    uint32_t appMagic = (uint32_t)buf[32] | ((uint32_t)buf[33] << 8) |
                        ((uint32_t)buf[34] << 16) | ((uint32_t)buf[35] << 24);
    if (appMagic != 0xABCD5432u) return OTA_GUARD_NO_APP_DESC;

    if (expectedProject) {
        char project[33];
        memcpy(project, buf + 80, 32);
        project[32] = '\0';
        if (strcmp(project, expectedProject) != 0) return OTA_GUARD_WRONG_PROJECT;
    }
    return OTA_GUARD_OK;
}

inline const char *ota_guard_message(OtaGuardVerdict v) {
    switch (v) {
        case OTA_GUARD_OK:            return "OK";
        case OTA_GUARD_SHORT:         return "Firmware file too small";
        case OTA_GUARD_BAD_MAGIC:     return "Not an ESP32 firmware image";
        case OTA_GUARD_WRONG_CHIP:    return "Firmware is for a different chip — check the board selection";
        case OTA_GUARD_NO_APP_DESC:   return "Not an app image (merged/bootloader image? OTA needs the app-only .bin)";
        case OTA_GUARD_WRONG_PROJECT: return "Firmware is a different application — refusing to install";
    }
    return "Firmware rejected";
}
