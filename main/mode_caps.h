#pragma once

#include <cstdint>

// ════════════════════════════════════════════════════════════════════════════
// Mode capability mask — which operating modes this unit supports.
// Set via web UI "Unit Capabilities"; persisted in NVS (settings.h modeMask).
// Off is always available and is not part of the mask.
//
// Pure header (no ESP-IDF dependencies) so the sanitizer stays host-testable.
// CN105-mode mapping helpers live in cn105_strings.h.
// ════════════════════════════════════════════════════════════════════════════

constexpr uint8_t MODE_CAP_HEAT = 1 << 0;
constexpr uint8_t MODE_CAP_COOL = 1 << 1;
constexpr uint8_t MODE_CAP_DRY  = 1 << 2;
constexpr uint8_t MODE_CAP_FAN  = 1 << 3;
constexpr uint8_t MODE_CAP_AUTO = 1 << 4;
constexpr uint8_t MODE_CAP_ALL  = 0x1F;

// Enforce mask invariants. Applied on NVS load and on every web write, so
// every consumer may assume a sanitized mask and must not re-sanitize:
//   0. Unknown bits are stripped.
//   1. At least one real mode (HEAT/COOL/DRY/FAN) — else reset to ALL.
//   2. AUTO requires both HEAT and COOL (it alternates between them).
inline uint8_t mode_mask_sanitize(uint8_t mask) {
    mask &= MODE_CAP_ALL;
    if ((mask & (MODE_CAP_HEAT | MODE_CAP_COOL | MODE_CAP_DRY | MODE_CAP_FAN)) == 0)
        return MODE_CAP_ALL;
    if ((mask & (MODE_CAP_HEAT | MODE_CAP_COOL)) != (MODE_CAP_HEAT | MODE_CAP_COOL))
        mask &= ~MODE_CAP_AUTO;
    return mask;
}
