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
    // Time out auto-started poll cycle so _cycleRunning = false
    stub_millis = 5000 + 5001;
    cn105.loop();
    uart.clearTxLog();
}

// Enable enhanced temp mode by feeding a SETTINGS response with data[11] != 0
static void enableEnhancedTempMode() {
    // Start a new poll cycle (phase 0 = SETTINGS)
    stub_millis += CN105_UPDATE_INTERVAL + 1;
    cn105.loop(); // starts cycle, sends SETTINGS INFO_REQ

    uint8_t data[16] = {0};
    data[0] = 0x02;
    data[3] = 0x01;
    data[4] = 0x03;
    data[11] = 173; // non-zero -> enables enhanced mode
    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop(); // processes response

    // Time out remaining poll cycle so SET packets can be sent
    stub_millis += 5001;
    cn105.loop();
    uart.clearTxLog();
}

// Set temp, trigger loop, return pointer to the 22-byte SET packet in TX log.
// Requires no active poll cycle (_cycleRunning = false).
static const uint8_t* setTempAndCapture(float temp) {
    uart.clearTxLog();
    cn105.setTargetTemp(temp);
    stub_millis += 100;
    cn105.loop(); // sends SET packet (no cycle running)
    return uart.getTxLog().data();
}

void setUp() {
    uart = MockUart();
    cn105 = CN105Controller();
    stub_millis = 0;
}

void tearDown() {}

// ── Legacy mode tests ──────────────────────────────────────────────────────

void test_legacy_22C() {
    connectController();
    const uint8_t *pkt = setTempAndCapture(22.0f);
    // Legacy: pkt[10] = 31 - 22 = 9
    TEST_ASSERT_EQUAL_HEX8(9, pkt[10]);
}

void test_legacy_16C_min() {
    connectController();
    const uint8_t *pkt = setTempAndCapture(16.0f);
    TEST_ASSERT_EQUAL_HEX8(15, pkt[10]); // 31 - 16
}

void test_legacy_31C_max() {
    connectController();
    const uint8_t *pkt = setTempAndCapture(31.0f);
    TEST_ASSERT_EQUAL_HEX8(0, pkt[10]); // 31 - 31
}

void test_legacy_clamped_below_min() {
    connectController();
    const uint8_t *pkt = setTempAndCapture(10.0f); // below 16
    TEST_ASSERT_EQUAL_HEX8(15, pkt[10]); // clamped to 16: 31 - 16
}

void test_legacy_clamped_above_max() {
    connectController();
    const uint8_t *pkt = setTempAndCapture(40.0f); // above 31
    TEST_ASSERT_EQUAL_HEX8(0, pkt[10]); // clamped to 31: 31 - 31
}

// ── Enhanced mode tests ────────────────────────────────────────────────────

void test_enhanced_22_5C() {
    connectController();
    enableEnhancedTempMode();
    const uint8_t *pkt = setTempAndCapture(22.5f);
    TEST_ASSERT_EQUAL_HEX8(0x00, pkt[10]); // byte 10 = 0 in enhanced
    TEST_ASSERT_EQUAL_HEX8(173, pkt[19]);   // 22.5 * 2 + 128 = 173
}

void test_enhanced_16C_min() {
    connectController();
    enableEnhancedTempMode();
    const uint8_t *pkt = setTempAndCapture(16.0f);
    TEST_ASSERT_EQUAL_HEX8(160, pkt[19]); // 16 * 2 + 128 = 160
}

void test_enhanced_31C_max() {
    connectController();
    enableEnhancedTempMode();
    const uint8_t *pkt = setTempAndCapture(31.0f);
    TEST_ASSERT_EQUAL_HEX8(190, pkt[19]); // 31 * 2 + 128 = 190
}

void test_enhanced_rounding_22_3() {
    connectController();
    enableEnhancedTempMode();
    // 22.3 rounds to 22.5 via round(22.3*2)/2 = round(44.6)/2 = 45/2 = 22.5
    const uint8_t *pkt = setTempAndCapture(22.3f);
    TEST_ASSERT_EQUAL_HEX8(173, pkt[19]); // 22.5 * 2 + 128 = 173
}

void test_enhanced_rounding_22_7() {
    connectController();
    enableEnhancedTempMode();
    // 22.7 rounds to 22.5 via round(22.7*2)/2 = round(45.4)/2 = 45/2 = 22.5
    const uint8_t *pkt = setTempAndCapture(22.7f);
    TEST_ASSERT_EQUAL_HEX8(173, pkt[19]); // 22.5
}

void test_enhanced_rounding_22_8() {
    connectController();
    enableEnhancedTempMode();
    // 22.8 rounds to 23.0 via round(22.8*2)/2 = round(45.6)/2 = 46/2 = 23.0
    const uint8_t *pkt = setTempAndCapture(22.8f);
    TEST_ASSERT_EQUAL_HEX8(174, pkt[19]); // 23.0 * 2 + 128 = 174
}

// ── Main ────────────────────────────────────────────────────────────────────

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_legacy_22C);
    RUN_TEST(test_legacy_16C_min);
    RUN_TEST(test_legacy_31C_max);
    RUN_TEST(test_legacy_clamped_below_min);
    RUN_TEST(test_legacy_clamped_above_max);

    RUN_TEST(test_enhanced_22_5C);
    RUN_TEST(test_enhanced_16C_min);
    RUN_TEST(test_enhanced_31C_max);
    RUN_TEST(test_enhanced_rounding_22_3);
    RUN_TEST(test_enhanced_rounding_22_7);
    RUN_TEST(test_enhanced_rounding_22_8);

    return UNITY_END();
}
