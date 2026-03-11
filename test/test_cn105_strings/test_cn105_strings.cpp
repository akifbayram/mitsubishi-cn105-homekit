#include <unity.h>
#include "cn105_strings.h"

// ── Mode ────────────────────────────────────────────────────────────────────

void test_mode_log_str() {
    TEST_ASSERT_EQUAL_STRING("HEAT", modeToLogStr(CN105_MODE_HEAT));
    TEST_ASSERT_EQUAL_STRING("DRY",  modeToLogStr(CN105_MODE_DRY));
    TEST_ASSERT_EQUAL_STRING("COOL", modeToLogStr(CN105_MODE_COOL));
    TEST_ASSERT_EQUAL_STRING("FAN",  modeToLogStr(CN105_MODE_FAN));
    TEST_ASSERT_EQUAL_STRING("AUTO", modeToLogStr(CN105_MODE_AUTO));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", modeToLogStr(0xFF));
}

void test_mode_web_str() {
    TEST_ASSERT_EQUAL_STRING("heat", modeToWebStr(CN105_MODE_HEAT));
    TEST_ASSERT_EQUAL_STRING("cool", modeToWebStr(CN105_MODE_COOL));
    TEST_ASSERT_EQUAL_STRING("unknown", modeToWebStr(0xFF));
}

void test_mode_parse() {
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_HEAT, strToMode("heat"));
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_COOL, strToMode("cool"));
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_AUTO, strToMode("auto"));
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_AUTO, strToMode("invalid")); // default
}

// ── Fan ─────────────────────────────────────────────────────────────────────

void test_fan_log_str() {
    TEST_ASSERT_EQUAL_STRING("AUTO",  fanToLogStr(CN105_FAN_AUTO));
    TEST_ASSERT_EQUAL_STRING("QUIET", fanToLogStr(CN105_FAN_QUIET));
    TEST_ASSERT_EQUAL_STRING("1",     fanToLogStr(CN105_FAN_1));
    TEST_ASSERT_EQUAL_STRING("4",     fanToLogStr(CN105_FAN_4));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", fanToLogStr(0xFF));
}

void test_fan_parse() {
    TEST_ASSERT_EQUAL_HEX8(CN105_FAN_QUIET, strToFan("quiet"));
    TEST_ASSERT_EQUAL_HEX8(CN105_FAN_3, strToFan("3"));
    TEST_ASSERT_EQUAL_HEX8(CN105_FAN_AUTO, strToFan("invalid")); // default
}

// ── Vane ────────────────────────────────────────────────────────────────────

void test_vane_log_str() {
    TEST_ASSERT_EQUAL_STRING("AUTO",  vaneToLogStr(CN105_VANE_AUTO));
    TEST_ASSERT_EQUAL_STRING("SWING", vaneToLogStr(CN105_VANE_SWING));
    TEST_ASSERT_EQUAL_STRING("3",     vaneToLogStr(CN105_VANE_3));
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", vaneToLogStr(0xFF));
}

void test_vane_parse() {
    TEST_ASSERT_EQUAL_HEX8(CN105_VANE_SWING, strToVane("swing"));
    TEST_ASSERT_EQUAL_HEX8(CN105_VANE_AUTO, strToVane("invalid"));
}

// ── Wide Vane ───────────────────────────────────────────────────────────────

void test_wide_vane_web_str() {
    TEST_ASSERT_EQUAL_STRING("ll", wideVaneToWebStr(CN105_WVANE_LEFT_LEFT));
    TEST_ASSERT_EQUAL_STRING("c",  wideVaneToWebStr(CN105_WVANE_CENTER));
    TEST_ASSERT_EQUAL_STRING("swing", wideVaneToWebStr(CN105_WVANE_SWING));
    TEST_ASSERT_EQUAL_STRING("unknown", wideVaneToWebStr(0xFF));
}

void test_wide_vane_parse() {
    TEST_ASSERT_EQUAL_HEX8(CN105_WVANE_SPLIT, strToWideVane("split"));
    TEST_ASSERT_EQUAL_HEX8(CN105_WVANE_CENTER, strToWideVane("invalid")); // default
}

// ── Sub Mode / Stage / Auto Sub Mode ─────────────────────────────────────

void test_sub_mode_str() {
    TEST_ASSERT_EQUAL_STRING("NORMAL",  subModeToLogStr(0x00));
    TEST_ASSERT_EQUAL_STRING("DEFROST", subModeToLogStr(0x02));
    TEST_ASSERT_EQUAL_STRING("standby", subModeToWebStr(0x08));
    TEST_ASSERT_EQUAL_STRING("unknown", subModeToWebStr(0xFF));
}

void test_stage_str() {
    TEST_ASSERT_EQUAL_STRING("IDLE",    stageToLogStr(0x00));
    TEST_ASSERT_EQUAL_STRING("DIFFUSE", stageToLogStr(0x06));
    TEST_ASSERT_EQUAL_STRING("high",    stageToWebStr(0x05));
    TEST_ASSERT_EQUAL_STRING("unknown", stageToWebStr(0xFF));
}

void test_auto_sub_mode_str() {
    TEST_ASSERT_EQUAL_STRING("OFF",    autoSubModeToLogStr(0x00));
    TEST_ASSERT_EQUAL_STRING("HEAT",   autoSubModeToLogStr(0x02));
    TEST_ASSERT_EQUAL_STRING("leader", autoSubModeToWebStr(0x03));
    TEST_ASSERT_EQUAL_STRING("unknown", autoSubModeToWebStr(0xFF));
}

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_mode_log_str);
    RUN_TEST(test_mode_web_str);
    RUN_TEST(test_mode_parse);
    RUN_TEST(test_fan_log_str);
    RUN_TEST(test_fan_parse);
    RUN_TEST(test_vane_log_str);
    RUN_TEST(test_vane_parse);
    RUN_TEST(test_wide_vane_web_str);
    RUN_TEST(test_wide_vane_parse);
    RUN_TEST(test_sub_mode_str);
    RUN_TEST(test_stage_str);
    RUN_TEST(test_auto_sub_mode_str);

    return UNITY_END();
}
