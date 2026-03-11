#include <unity.h>
#include "cn105_protocol.h"
#include "mock_uart.h"

extern uint32_t stub_millis;

static MockUart uart;
static CN105Controller cn105;

static void buildPacket(uint8_t *pkt, uint8_t type, const uint8_t *data, uint8_t dataLen) {
    memset(pkt, 0, 5 + dataLen + 1);
    CN105Controller::buildHeader(pkt, type, dataLen);
    if (data && dataLen > 0) memcpy(pkt + 5, data, dataLen);
    pkt[5 + dataLen] = CN105Controller::calcChecksum(pkt, 5 + dataLen);
}

static void connectController() {
    stub_millis = 1000;
    cn105.begin(&uart);
    uart.clearTxLog();
    stub_millis = 5000;
    cn105.loop();
    uint8_t connectOk[8] = {0};
    buildPacket(connectOk, 0x7A, nullptr, 2);
    uart.feedBytes(connectOk, 8);
    cn105.loop();
    // Time out auto-started poll cycle
    stub_millis = 5000 + 5001;
    cn105.loop();
    uart.clearTxLog();
}

void setUp() {
    uart = MockUart();
    cn105 = CN105Controller();
    stub_millis = 0;
}

void tearDown() {}

// ── isFieldInGrace tests ────────────────────────────────────────────────────

void test_isFieldInGrace_within_timeout() {
    connectController();
    stub_millis = 11000;
    cn105.setPower(true);
    // Immediately after command, should be in grace
    TEST_ASSERT_TRUE(cn105.isFieldInGrace(cn105.getWanted().hasPower));
}

void test_isFieldInGrace_after_timeout() {
    connectController();
    stub_millis = 11000;
    cn105.setPower(true);
    // Advance past 10s grace timeout
    stub_millis = 22000;
    TEST_ASSERT_FALSE(cn105.isFieldInGrace(cn105.getWanted().hasPower));
}

void test_isFieldInGrace_no_flag() {
    connectController();
    // No command issued, hasPower is false
    TEST_ASSERT_FALSE(cn105.isFieldInGrace(false));
}

// ── getEffectiveState tests ─────────────────────────────────────────────────

void test_getEffectiveState_substitutes_during_grace() {
    connectController();
    stub_millis = 11000;
    cn105.setMode(CN105_MODE_HEAT);
    cn105.setTargetTemp(25.0f);

    CN105State eff = cn105.getEffectiveState();
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_HEAT, eff.mode);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, eff.targetTemp);
}

void test_getEffectiveState_actual_after_timeout() {
    connectController();
    stub_millis = 11000;
    cn105.setMode(CN105_MODE_HEAT);

    // Advance past grace
    stub_millis = 22000;
    CN105State eff = cn105.getEffectiveState();
    // Should return actual state (AUTO, the default)
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_AUTO, eff.mode);
}

// ── Confirmation / clear-on-match tests ─────────────────────────────────────

void test_confirmation_clears_wanted_flags() {
    connectController();
    stub_millis = 11000;
    cn105.setPower(true);
    cn105.setMode(CN105_MODE_COOL);

    // Simulate: send the SET packet (loop sends it outside poll cycle)
    stub_millis = 11100;
    cn105.loop(); // sends SET packet, sets hasBeenSent = true
    uart.clearTxLog();

    // Now start a poll cycle and feed SETTINGS response that matches
    stub_millis += 3000;
    cn105.loop(); // starts poll cycle

    uint8_t data[16] = {0};
    data[0] = 0x02;
    data[3] = 0x01; // power ON (matches)
    data[4] = 0x03; // COOL (matches)
    data[5] = 0x09;
    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_FALSE(cn105.getWanted().hasPower);
    TEST_ASSERT_FALSE(cn105.getWanted().hasMode);
}

void test_confirmation_mismatch_keeps_flags() {
    connectController();
    stub_millis = 11000;
    cn105.setPower(true);
    cn105.setMode(CN105_MODE_HEAT);

    stub_millis = 11100;
    cn105.loop(); // sends SET
    uart.clearTxLog();

    stub_millis += 3000;
    cn105.loop(); // starts poll cycle

    uint8_t data[16] = {0};
    data[0] = 0x02;
    data[3] = 0x01; // power ON (matches)
    data[4] = 0x03; // COOL (doesn't match HEAT)
    data[5] = 0x09;
    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_FALSE(cn105.getWanted().hasPower); // power matched
    TEST_ASSERT_TRUE(cn105.getWanted().hasMode);    // mode did NOT match
}

void test_temp_confirmation_tolerance() {
    connectController();
    stub_millis = 11000;
    cn105.setTargetTemp(22.5f);

    stub_millis = 11100;
    cn105.loop();
    uart.clearTxLog();

    stub_millis += 3000;
    cn105.loop();

    uint8_t data[16] = {0};
    data[0] = 0x02;
    data[3] = 0x01;
    data[4] = 0x03;
    data[11] = 173; // (173-128)/2 = 22.5 -> within 0.3 of 22.5
    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_FALSE(cn105.getWanted().hasTemp);
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_isFieldInGrace_within_timeout);
    RUN_TEST(test_isFieldInGrace_after_timeout);
    RUN_TEST(test_isFieldInGrace_no_flag);
    RUN_TEST(test_getEffectiveState_substitutes_during_grace);
    RUN_TEST(test_getEffectiveState_actual_after_timeout);
    RUN_TEST(test_confirmation_clears_wanted_flags);
    RUN_TEST(test_confirmation_mismatch_keeps_flags);
    RUN_TEST(test_temp_confirmation_tolerance);
    return UNITY_END();
}
