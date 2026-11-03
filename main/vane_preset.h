#pragma once
// Vane Preset rules — no ESP-IDF deps (host-tested in test/vane_preset/).
// Vocabulary: CONTEXT.md › Airflow. A Vane Preset is one saved Vane position
// plus one Wide Vane position; the Vane Switch applies the On preset when
// turned on and the Off preset when turned off, and reads On only while the
// unit's Vane and Wide Vane match the On preset.
//
// The position values mirror CN105_VANE_* / CN105_WVANE_* (cn105_protocol.h
// static_asserts they agree); they live here so this header stays pure.

#include <cstdint>
#include <cstring>
#include <initializer_list>

// DeviceSettings::vaneConfig — the owner's Vane Type declaration.
inline constexpr uint8_t VANE_TYPE_NONE = 0;   // no vanes
inline constexpr uint8_t VANE_TYPE_VANE = 1;   // Vane only
inline constexpr uint8_t VANE_TYPE_BOTH = 2;   // Vane and Wide Vane

struct VanePreset {
    uint8_t vane;       // Vane: 0 auto, 1..5, 7 swing
    uint8_t wideVane;   // Wide Vane: 1 ll, 2 l, 3 c, 4 r, 5 rr, 8 split, 12 swing
    bool operator==(const VanePreset& o) const { return vane == o.vane && wideVane == o.wideVane; }
    bool operator!=(const VanePreset& o) const { return !(*this == o); }
};

// Distinct by design: enable is refused while On and Off are equal, so the
// defaults must already differ on the Vane axis alone (Vane-only units).
inline constexpr VanePreset VANE_PRESET_ON_DEFAULT  {3, 3};    // Vane 3, Center
inline constexpr VanePreset VANE_PRESET_OFF_DEFAULT {0, 12};   // Auto, Swing

// ── Values: exactly the positions the web UI can send ───────────────────────
inline constexpr bool vane_value_valid(uint8_t v) {
    return v <= 5 || v == 7;
}
inline constexpr bool wide_vane_value_valid(uint8_t v) {
    return (v >= 1 && v <= 5) || v == 8 || v == 12;
}
inline constexpr bool vane_preset_valid(const VanePreset& p) {
    return vane_value_valid(p.vane) && wide_vane_value_valid(p.wideVane);
}

// ── Web strings (the same vocabulary as the live vane controls) ─────────────
// Strict: an unknown string is rejected rather than mapped to a default, so
// a hand-edited settings file cannot silently store a position it never named.
inline const char* vane_to_web_str(uint8_t v) {
    switch (v) {
        case 0: return "auto";
        case 1: return "1";
        case 2: return "2";
        case 3: return "3";
        case 4: return "4";
        case 5: return "5";
        case 7: return "swing";
        default: return "auto";
    }
}
inline bool vane_from_web_str(const char* s, uint8_t* out) {
    if (!s || !out) return false;
    for (uint8_t v : {0, 1, 2, 3, 4, 5, 7}) {
        if (strcmp(s, vane_to_web_str(v)) == 0) { *out = v; return true; }
    }
    return false;
}
inline const char* wide_vane_to_web_str(uint8_t v) {
    switch (v) {
        case 1:  return "ll";
        case 2:  return "l";
        case 3:  return "c";
        case 4:  return "r";
        case 5:  return "rr";
        case 8:  return "split";
        case 12: return "swing";
        default: return "c";
    }
}
inline bool wide_vane_from_web_str(const char* s, uint8_t* out) {
    if (!s || !out) return false;
    for (uint8_t v : {1, 2, 3, 4, 5, 8, 12}) {
        if (strcmp(s, wide_vane_to_web_str(v)) == 0) { *out = v; return true; }
    }
    return false;
}

// ── Load: what NVS returned into a default seed ─────────────────────────────
// Absent keys leave the seed untouched. A preset with either axis outside the
// accepted set (corrupt NVS, a downgrade) goes back to the given default as a
// whole rather than half of it reaching the unit.
inline VanePreset vane_preset_load(uint8_t vane, uint8_t wideVane, VanePreset fallback) {
    const VanePreset p{vane, wideVane};
    return vane_preset_valid(p) ? p : fallback;
}

// ── Vane Type gating ────────────────────────────────────────────────────────
// The Wide Vane axis applies unless the owner declared a Vane-only unit. Under
// VANE_TYPE_NONE nothing is hidden: both axes still count, so an imported file
// with distinct presets never reads as equal just because the unit has no
// vanes right now.
inline bool vane_preset_wide_applies(uint8_t vaneType) {
    return vaneType != VANE_TYPE_VANE;
}
inline bool vane_preset_equal(const VanePreset& a, const VanePreset& b, uint8_t vaneType) {
    if (a.vane != b.vane) return false;
    return !vane_preset_wide_applies(vaneType) || a.wideVane == b.wideVane;
}
// Enable is refused while the two presets are equal on the axes that apply:
// a switch whose On and Off do the same thing could never read Off.
inline bool vane_switch_enable_allowed(const VanePreset& on, const VanePreset& off, uint8_t vaneType) {
    return !vane_preset_equal(on, off, vaneType);
}
// The HomeKit service is built at boot only when the owner enabled it AND the
// unit has vanes to drive. The flag itself is never cleared by a Vane Type
// change, so switching back restores the service at the next boot.
inline bool vane_switch_service_wanted(bool enabled, uint8_t vaneType) {
    return enabled && vaneType != VANE_TYPE_NONE;
}

// ── Mirror: does the unit's current state match a preset? ───────────────────
inline bool vane_preset_matches(const VanePreset& p, uint8_t vane, uint8_t wideVane, uint8_t vaneType) {
    return vane_preset_equal(p, VanePreset{vane, wideVane}, vaneType);
}
