#pragma once
// HomeKit service visibility and fixed IID slots — no ESP-IDF deps
// (host-tested in test/hk_service_slots/). Vocabulary: CONTEXT.md › HomeKit.
//
// The owner can remove Fan, Fan Auto, Fan Mode and Dry Mode from the Heat
// Pump accessory (DeviceSettings::hkHidden). The SDK numbers IIDs in creation
// order, so a removed service would hand its IIDs to the next one, and a Home
// scene bound to one switch could drive another. Instead each optional
// service owns a fixed slot of IIDs, counted from the thermostat's last IID
// and reserved whether or not the service is built. With every service built
// the slots equal the SDK's own sequential numbering.

#include <cstdint>
#include "mode_caps.h"

// Service bits, shared by DeviceSettings::hkHidden (NVS ac-settings "hkHidden",
// bit set = removed) and hk-meta "svcBuilt" (bit set = built this boot).
// Stable NVS contract: never renumber a bit; a new service takes the next free one.
inline constexpr uint8_t HK_SVC_FAN      = 1 << 0;   // Fan (web "Fan Speed", Home "Fan")
inline constexpr uint8_t HK_SVC_FAN_AUTO = 1 << 1;
inline constexpr uint8_t HK_SVC_FAN_MODE = 1 << 2;
inline constexpr uint8_t HK_SVC_DRY_MODE = 1 << 3;
inline constexpr uint8_t HK_SVC_ALL      = 0x0F;

// The services this mode mask allows: Fan Mode and Dry Mode need their mode.
inline constexpr uint8_t hk_services_available(uint8_t modeMask) {
    return HK_SVC_FAN | HK_SVC_FAN_AUTO |
           ((modeMask & MODE_CAP_FAN) ? HK_SVC_FAN_MODE : 0) |
           ((modeMask & MODE_CAP_DRY) ? HK_SVC_DRY_MODE : 0);
}

// The services a boot builds. Hiding can only remove one.
inline constexpr uint8_t hk_services_built(uint8_t hidden, uint8_t modeMask) {
    return hk_services_available(modeMask) & ~hidden & HK_SVC_ALL;
}

// Slots in creation order (homekit_services_create_all()).
enum HkSlot : uint8_t {
    HK_SLOT_FAN,
    HK_SLOT_FAN_AUTO,
    HK_SLOT_FAN_MODE,
    HK_SLOT_DRY_MODE,
    HK_SLOT_VANE_PRESET,
    HK_SLOT_COUNT
};

// IIDs per slot: the service plus its characteristics.
inline constexpr uint8_t HK_SLOT_SIZE[HK_SLOT_COUNT] = {
    6,   // Fan: Active, RotationSpeed, StatusActive, Name, ConfiguredName
    5,   // Fan Auto: On, Name, ConfiguredName, StatusActive
    5,   // Fan Mode: same
    5,   // Dry Mode: same
    5,   // Vane Preset: same
};

// The service's IID; its characteristics follow it in order.
inline constexpr int32_t hk_slot_first_iid(int32_t thermostatLastIid, HkSlot slot) {
    int32_t iid = thermostatLastIid + 1;
    for (int s = 0; s < slot; ++s) iid += HK_SLOT_SIZE[s];
    return iid;
}
