#include "ota_guard.h"
#include <cassert>
#include <cstring>
#include <cstdio>

// Build a minimal synthetic app image header (288 bytes).
static void makeImage(uint8_t *buf, uint16_t chipId, uint32_t appMagic, const char *project) {
    memset(buf, 0, OTA_GUARD_HDR_LEN);
    buf[0] = 0xE9;                       // image magic
    buf[12] = (uint8_t)(chipId & 0xFF);  // chip_id LE
    buf[13] = (uint8_t)(chipId >> 8);
    buf[32] = (uint8_t)(appMagic);       // esp_app_desc_t.magic_word LE
    buf[33] = (uint8_t)(appMagic >> 8);
    buf[34] = (uint8_t)(appMagic >> 16);
    buf[35] = (uint8_t)(appMagic >> 24);
    strncpy((char *)buf + 80, project, 31);  // project_name
}

int main() {
    uint8_t img[OTA_GUARD_HDR_LEN];
    const uint16_t C6 = 0x000D, S3 = 0x0009;

    // Matching image passes
    makeImage(img, C6, 0xABCD5432, "mitsubishi-cn105-homekit");
    assert(ota_guard_check(img, sizeof img, C6, "mitsubishi-cn105-homekit") == OTA_GUARD_OK);

    // Too short => SHORT (caller keeps accumulating)
    assert(ota_guard_check(img, 100, C6, "mitsubishi-cn105-homekit") == OTA_GUARD_SHORT);

    // Not an ESP image
    img[0] = 0x1F;  // gzip, someone uploaded the wrong file entirely
    assert(ota_guard_check(img, sizeof img, C6, "mitsubishi-cn105-homekit") == OTA_GUARD_BAD_MAGIC);

    // Wrong chip (AtomS3 build onto a NanoC6)
    makeImage(img, S3, 0xABCD5432, "mitsubishi-cn105-homekit");
    assert(ota_guard_check(img, sizeof img, C6, "mitsubishi-cn105-homekit") == OTA_GUARD_WRONG_CHIP);

    // Unknown running chip id (0xFFFF) skips the chip check
    assert(ota_guard_check(img, sizeof img, 0xFFFF, "mitsubishi-cn105-homekit") == OTA_GUARD_OK);

    // No app descriptor (bootloader.bin / merged factory image)
    makeImage(img, C6, 0xDEADBEEF, "mitsubishi-cn105-homekit");
    assert(ota_guard_check(img, sizeof img, C6, "mitsubishi-cn105-homekit") == OTA_GUARD_NO_APP_DESC);

    // Wrong app (Dial firmware onto the unit — same chip!)
    makeImage(img, S3, 0xABCD5432, "serin_dial");
    assert(ota_guard_check(img, sizeof img, S3, "mitsubishi-cn105-homekit") == OTA_GUARD_WRONG_PROJECT);

    // NULL expectedProject skips the project check
    assert(ota_guard_check(img, sizeof img, S3, nullptr) == OTA_GUARD_OK);

    // Unterminated project_name field must not read past the 32-byte field
    makeImage(img, C6, 0xABCD5432, "x");
    memset(img + 80, 'A', 32);  // 32 'A's, no NUL
    assert(ota_guard_check(img, sizeof img, C6, "mitsubishi-cn105-homekit") == OTA_GUARD_WRONG_PROJECT);

    // Every verdict has a message
    for (int v = 0; v <= OTA_GUARD_WRONG_PROJECT; v++)
        assert(ota_guard_message((OtaGuardVerdict)v) != nullptr);

    printf("ota_guard: all tests passed\n");
    return 0;
}
