#include <unity.h>
#include "status_led.h"

extern uint32_t stub_millis;
extern std::vector<NeopixelCall> neopixel_log;

static StatusLED led(7); // pin 7, no enable pin

void setUp() {
    stub_millis = 0;
    neopixel_log.clear();
    led = StatusLED(7);
}

void tearDown() {}

void test_initial_state_is_off() {
    TEST_ASSERT_EQUAL(SLED_OFF, led.getState());
}

void test_setState_boot_shows_white() {
    led.setState(SLED_BOOT);
    TEST_ASSERT_EQUAL(SLED_BOOT, led.getState());
    // Should have written white (30,30,30)
    TEST_ASSERT_TRUE(neopixel_log.size() > 0);
    auto& last = neopixel_log.back();
    TEST_ASSERT_EQUAL(30, last.r);
    TEST_ASSERT_EQUAL(30, last.g);
    TEST_ASSERT_EQUAL(30, last.b);
}

void test_setState_same_state_noop() {
    led.setState(SLED_BOOT);
    size_t count = neopixel_log.size();
    led.setState(SLED_BOOT); // same state
    TEST_ASSERT_EQUAL(count, neopixel_log.size()); // no new calls
}

void test_wifi_connecting_orange() {
    led.setState(SLED_WIFI_CONNECTING);
    auto& last = neopixel_log.back();
    TEST_ASSERT_EQUAL(30, last.r);
    TEST_ASSERT_EQUAL(10, last.g); // 30/3 = 10
    TEST_ASSERT_EQUAL(0, last.b);
}

void test_cn105_disconnected_red() {
    led.setState(SLED_CN105_DISCONNECTED);
    auto& last = neopixel_log.back();
    TEST_ASSERT_EQUAL(30, last.r);
    TEST_ASSERT_EQUAL(0, last.g);
    TEST_ASSERT_EQUAL(0, last.b);
}

void test_wifi_connected_auto_off() {
    stub_millis = 1000;
    led.setState(SLED_WIFI_CONNECTED);
    TEST_ASSERT_EQUAL(SLED_WIFI_CONNECTED, led.getState());

    // After 1000ms, loop should transition to OFF
    stub_millis = 2001;
    led.loop();
    TEST_ASSERT_EQUAL(SLED_OFF, led.getState());
}

void test_boot_blink_toggle() {
    stub_millis = 1000;
    led.setState(SLED_BOOT);
    neopixel_log.clear();

    // After 500ms, should toggle OFF
    stub_millis = 1501;
    led.loop();
    auto& off_call = neopixel_log.back();
    TEST_ASSERT_EQUAL(0, off_call.r);
    TEST_ASSERT_EQUAL(0, off_call.g);
    TEST_ASSERT_EQUAL(0, off_call.b);

    // After another 500ms, should toggle back ON (white)
    stub_millis = 2002;
    led.loop();
    auto& on_call = neopixel_log.back();
    TEST_ASSERT_EQUAL(30, on_call.r);
    TEST_ASSERT_EQUAL(30, on_call.g);
    TEST_ASSERT_EQUAL(30, on_call.b);
}

void test_error_fast_blink() {
    stub_millis = 1000;
    led.setState(SLED_ERROR_CODE);
    neopixel_log.clear();

    // After 200ms, toggle off
    stub_millis = 1201;
    led.loop();
    auto& off_call = neopixel_log.back();
    TEST_ASSERT_EQUAL(0, off_call.r);

    // After another 200ms, toggle on (red)
    stub_millis = 1402;
    led.loop();
    auto& on_call = neopixel_log.back();
    TEST_ASSERT_EQUAL(30, on_call.r);
    TEST_ASSERT_EQUAL(0, on_call.g);
}

void test_ota_pulse_ramps() {
    stub_millis = 1000;
    led.setState(SLED_OTA);
    neopixel_log.clear();

    // At t=0 in cycle: brightness should be 0
    stub_millis = 1021; // just past 20ms rate limit
    led.loop();
    auto& start = neopixel_log.back();
    TEST_ASSERT_EQUAL(0, start.r);
    TEST_ASSERT_EQUAL(0, start.g);
    TEST_ASSERT_EQUAL(0, start.b); // brightness = (21 * 30) / 1000 = 0

    // At t=1000 in cycle: brightness should be max (30)
    stub_millis = 2000;
    led.loop();
    auto& peak = neopixel_log.back();
    TEST_ASSERT_EQUAL(30, peak.b);

    // At t=2000 (back to 0): brightness should be ~0
    stub_millis = 3000;
    led.loop();
    auto& bottom = neopixel_log.back();
    TEST_ASSERT_EQUAL(0, bottom.b);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_initial_state_is_off);
    RUN_TEST(test_setState_boot_shows_white);
    RUN_TEST(test_setState_same_state_noop);
    RUN_TEST(test_wifi_connecting_orange);
    RUN_TEST(test_cn105_disconnected_red);
    RUN_TEST(test_wifi_connected_auto_off);
    RUN_TEST(test_boot_blink_toggle);
    RUN_TEST(test_error_fast_blink);
    RUN_TEST(test_ota_pulse_ramps);
    return UNITY_END();
}
