// Host tests for the Vane Preset rules (vane_preset.h): which presets the
// firmware stores, when the Vane Switch may be enabled, and when the unit's
// louvers count as matching a preset. Vocabulary: CONTEXT.md › Airflow.
#include "vane_preset.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

int main() {
    // ── Values: exactly what the web UI can send today ──────────────────
    // Vane: Auto, 1..5, Swing
    for (uint8_t v : {0, 1, 2, 3, 4, 5, 7}) assert(vane_value_valid(v));
    for (uint8_t v : {6, 8, 12, 255})       assert(!vane_value_valid(v));
    // Wide Vane: Left-Left, Left, Center, Right, Right-Right, Split, Swing
    for (uint8_t v : {1, 2, 3, 4, 5, 8, 12}) assert(wide_vane_value_valid(v));
    for (uint8_t v : {0, 6, 7, 9, 11, 13, 255}) assert(!wide_vane_value_valid(v));

    // ── Web strings round-trip, unknown strings are rejected ────────────
    uint8_t out = 99;
    assert(vane_from_web_str("auto", &out) && out == 0);
    assert(vane_from_web_str("3", &out) && out == 3);
    assert(vane_from_web_str("swing", &out) && out == 7);
    assert(!vane_from_web_str("6", &out));
    assert(!vane_from_web_str("", &out));
    assert(!vane_from_web_str("c", &out));
    assert(wide_vane_from_web_str("ll", &out) && out == 1);
    assert(wide_vane_from_web_str("c", &out) && out == 3);
    assert(wide_vane_from_web_str("split", &out) && out == 8);
    assert(wide_vane_from_web_str("swing", &out) && out == 12);
    assert(!wide_vane_from_web_str("auto", &out));
    assert(!wide_vane_from_web_str("center", &out));
    for (uint8_t v : {0, 1, 2, 3, 4, 5, 7}) {
        assert(vane_from_web_str(vane_to_web_str(v), &out) && out == v);
    }
    for (uint8_t v : {1, 2, 3, 4, 5, 8, 12}) {
        assert(wide_vane_from_web_str(wide_vane_to_web_str(v), &out) && out == v);
    }

    // ── Defaults: On = Vane 3 + Center, Off = Auto + Swing ──────────────
    assert(VANE_PRESET_ON_DEFAULT.vane == 3 && VANE_PRESET_ON_DEFAULT.wideVane == 3);
    assert(VANE_PRESET_OFF_DEFAULT.vane == 0 && VANE_PRESET_OFF_DEFAULT.wideVane == 12);
    assert(vane_preset_valid(VANE_PRESET_ON_DEFAULT));
    assert(vane_preset_valid(VANE_PRESET_OFF_DEFAULT));
    assert(!vane_preset_valid(VanePreset{6, 3}));
    assert(!vane_preset_valid(VanePreset{3, 0}));

    // ── Load: a stored preset is sanitized back to its default ──────────
    // Absent keys leave the seed (the default) untouched.
    assert(vane_preset_load(3, 3, VANE_PRESET_ON_DEFAULT) == VANE_PRESET_ON_DEFAULT);
    assert(vane_preset_load(7, 8, VANE_PRESET_ON_DEFAULT) == (VanePreset{7, 8}));
    // One bad axis (corrupt NVS, a downgrade) resets the whole preset.
    assert(vane_preset_load(6, 3, VANE_PRESET_ON_DEFAULT) == VANE_PRESET_ON_DEFAULT);
    assert(vane_preset_load(3, 0, VANE_PRESET_OFF_DEFAULT) == VANE_PRESET_OFF_DEFAULT);

    // ── Equality honours the Vane Type ──────────────────────────────────
    const VanePreset a{3, 3}, sameVaneOtherWide{3, 12}, otherVane{0, 3};
    assert(vane_preset_equal(a, a, VANE_TYPE_BOTH));
    assert(!vane_preset_equal(a, sameVaneOtherWide, VANE_TYPE_BOTH));
    assert(!vane_preset_equal(a, otherVane, VANE_TYPE_BOTH));
    // Vane only: the Wide Vane axis does not apply
    assert(vane_preset_equal(a, sameVaneOtherWide, VANE_TYPE_VANE));
    assert(!vane_preset_equal(a, otherVane, VANE_TYPE_VANE));
    // None: nothing is hidden, both axes compare (an imported file with the
    // switch on and distinct presets must not read as equal)
    assert(!vane_preset_equal(a, sameVaneOtherWide, VANE_TYPE_NONE));

    // ── Enable is refused while the presets are equal ───────────────────
    assert(vane_switch_enable_allowed(VANE_PRESET_ON_DEFAULT, VANE_PRESET_OFF_DEFAULT, VANE_TYPE_BOTH));
    assert(vane_switch_enable_allowed(VANE_PRESET_ON_DEFAULT, VANE_PRESET_OFF_DEFAULT, VANE_TYPE_VANE));
    assert(!vane_switch_enable_allowed(a, a, VANE_TYPE_BOTH));
    // Equal on the only axis that applies
    assert(!vane_switch_enable_allowed(a, sameVaneOtherWide, VANE_TYPE_VANE));
    assert(vane_switch_enable_allowed(a, sameVaneOtherWide, VANE_TYPE_BOTH));

    // ── The service exists only when enabled and the unit has vanes ─────
    assert(vane_switch_service_wanted(true, VANE_TYPE_BOTH));
    assert(vane_switch_service_wanted(true, VANE_TYPE_VANE));
    assert(!vane_switch_service_wanted(true, VANE_TYPE_NONE));
    assert(!vane_switch_service_wanted(false, VANE_TYPE_BOTH));

    // ── Mirror: the switch reads On only while the vanes match ──────────
    // Both axes: exact match on both
    assert(vane_preset_matches(a, 3, 3, VANE_TYPE_BOTH));
    assert(!vane_preset_matches(a, 3, 12, VANE_TYPE_BOTH));
    assert(!vane_preset_matches(a, 0, 3, VANE_TYPE_BOTH));
    // Vane only: the unit's Wide Vane reading is ignored
    assert(vane_preset_matches(a, 3, 12, VANE_TYPE_VANE));
    assert(!vane_preset_matches(a, 0, 3, VANE_TYPE_VANE));
    // Swing and Auto are ordinary positions
    const VanePreset swinging{7, 12};
    assert(vane_preset_matches(swinging, 7, 12, VANE_TYPE_BOTH));
    assert(!vane_preset_matches(swinging, 7, 3, VANE_TYPE_BOTH));

    printf("vane_preset: all tests passed\n");
    return 0;
}
