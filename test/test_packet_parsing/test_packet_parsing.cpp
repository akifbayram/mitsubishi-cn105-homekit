#include <unity.h>
#include "cn105_protocol.h"
#include "mock_uart.h"

extern uint32_t stub_millis;

static MockUart uart;
static CN105Controller cn105;

// ── Packet builder helper ──────────────────────────────────────────────────

static void buildPacket(uint8_t *pkt, uint8_t type, const uint8_t *data, uint8_t dataLen) {
    memset(pkt, 0, 5 + dataLen + 1);
    CN105Controller::buildHeader(pkt, type, dataLen);
    if (data && dataLen > 0) memcpy(pkt + 5, data, dataLen);
    pkt[5 + dataLen] = CN105Controller::calcChecksum(pkt, 5 + dataLen);
}

// Connect the controller and wait out the auto-started poll cycle.
// After this, the controller is connected with no active cycle.
static void connectController() {
    stub_millis = 1000;
    cn105.begin(&uart);
    uart.clearTxLog();

    // Advance to trigger connect attempt
    stub_millis = 5000;
    cn105.loop(); // sends CONNECT_PKT

    // Feed CONNECT_OK
    uint8_t connectOk[8] = {0};
    buildPacket(connectOk, 0x7A, nullptr, 2); // CONNECT_OK, 2-byte data
    uart.feedBytes(connectOk, 8);
    cn105.loop(); // processes CONNECT_OK; also starts poll cycle

    // Time out the auto-started poll cycle so _cycleRunning = false.
    // Cycle timeout = (2 * updateInterval) + 1000 = 5000ms.
    stub_millis = 5000 + 5001;
    cn105.loop(); // cycle times out, _cycleRunning = false
    uart.clearTxLog();
}

// Start a fresh poll cycle and return after first INFO_REQ has been sent.
// Requires connectController() to have been called first.
static void startPollCycle() {
    stub_millis += CN105_UPDATE_INTERVAL + 1; // past update interval
    uart.clearTxLog();
    cn105.loop(); // starts cycle, sends first INFO_REQ (SETTINGS)
}

void setUp() {
    uart = MockUart();
    cn105 = CN105Controller();
    stub_millis = 0;
}

void tearDown() {}

// ── Tests ──────────────────────────────────────────────────────────────────

void test_connect_ok() {
    connectController();
    TEST_ASSERT_TRUE(cn105.isConnected());
}

void test_settings_response() {
    connectController();
    startPollCycle(); // phase 0 = SETTINGS

    // Build SETTINGS INFO_RESP: data[0]=0x02, data[3]=power, data[4]=mode,
    // data[5]=tempLegacy, data[6]=fan, data[7]=vane, data[10]=wideVane, data[11]=tempEnhanced
    uint8_t data[16] = {0};
    data[0] = 0x02; // CN105_INFO_SETTINGS
    data[3] = 0x01; // power ON
    data[4] = 0x03; // COOL mode
    data[5] = 0x09; // legacy temp: 31 - 9 = 22
    data[6] = 0x02; // fan speed 1
    data[7] = 0x03; // vane 3
    data[10] = 0x03; // wide vane center
    data[11] = 0x00; // no enhanced temp (use legacy)

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16); // INFO_RESP
    uart.feedBytes(pkt, 22);
    cn105.loop();

    const CN105State& s = cn105.getState();
    TEST_ASSERT_TRUE(s.power);
    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_COOL, s.mode);
    TEST_ASSERT_EQUAL_FLOAT(22.0f, s.targetTemp);
    TEST_ASSERT_EQUAL_HEX8(CN105_FAN_1, s.fanSpeed);
    TEST_ASSERT_EQUAL_HEX8(CN105_VANE_3, s.vane);
    TEST_ASSERT_EQUAL_HEX8(CN105_WVANE_CENTER, s.wideVane);
}

void test_settings_enhanced_temp() {
    connectController();
    startPollCycle();

    uint8_t data[16] = {0};
    data[0] = 0x02;
    data[3] = 0x01; // power ON
    data[4] = 0x01; // HEAT
    data[6] = 0x00; // fan auto
    data[7] = 0x00; // vane auto
    data[11] = 173; // enhanced: (173 - 128) / 2 = 22.5

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_EQUAL_FLOAT(22.5f, cn105.getState().targetTemp);
}

void test_settings_isee_mode_stripping() {
    connectController();
    startPollCycle();

    uint8_t data[16] = {0};
    data[0] = 0x02;
    data[3] = 0x01;
    data[4] = 0x03 + 0x08; // COOL + iSee flag = 0x0B
    data[5] = 0x09;

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_EQUAL_HEX8(CN105_MODE_COOL, cn105.getState().mode);
}

void test_roomtemp_enhanced() {
    connectController();
    startPollCycle();

    // First, feed SETTINGS response to advance past phase 0
    uint8_t settingsData[16] = {0};
    settingsData[0] = 0x02;
    settingsData[3] = 0x01;
    settingsData[4] = 0x03;
    settingsData[5] = 0x09;
    uint8_t settingsPkt[22];
    buildPacket(settingsPkt, 0x62, settingsData, 16);
    uart.feedBytes(settingsPkt, 22);
    cn105.loop(); // processes SETTINGS, sends ROOMTEMP request

    // Now feed ROOMTEMP response
    uint8_t data[16] = {0};
    data[0] = 0x03; // CN105_INFO_ROOMTEMP
    data[5] = 168;  // outside: (168-128)/2 = 20.0
    data[6] = 172;  // room: (172-128)/2 = 22.0

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_EQUAL_FLOAT(22.0f, cn105.getState().roomTemp);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, cn105.getState().outsideTemp);
    TEST_ASSERT_TRUE(cn105.getState().outsideTempValid);
}

void test_roomtemp_legacy() {
    connectController();
    startPollCycle();

    uint8_t settingsData[16] = {0};
    settingsData[0] = 0x02; settingsData[3] = 0x01; settingsData[4] = 0x03; settingsData[5] = 0x09;
    uint8_t settingsPkt[22];
    buildPacket(settingsPkt, 0x62, settingsData, 16);
    uart.feedBytes(settingsPkt, 22);
    cn105.loop();

    uint8_t data[16] = {0};
    data[0] = 0x03;
    data[3] = 15;  // legacy room: 15 + 10 = 25.0
    data[6] = 0;   // enhanced = 0, triggers legacy path

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_EQUAL_FLOAT(25.0f, cn105.getState().roomTemp);
}

void test_roomtemp_outside_invalid() {
    connectController();
    startPollCycle();

    uint8_t settingsData[16] = {0};
    settingsData[0] = 0x02; settingsData[3] = 0x01; settingsData[4] = 0x03; settingsData[5] = 0x09;
    uint8_t settingsPkt[22];
    buildPacket(settingsPkt, 0x62, settingsData, 16);
    uart.feedBytes(settingsPkt, 22);
    cn105.loop();

    uint8_t data[16] = {0};
    data[0] = 0x03;
    data[5] = 0x01; // outside = 1 -> invalid
    data[6] = 172;

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_FALSE(cn105.getState().outsideTempValid);
}

void test_status_response() {
    connectController();
    startPollCycle();

    // Advance through SETTINGS, ROOMTEMP, ERRORCODE to reach STATUS (phase 3)
    uint8_t types[] = {0x02, 0x03, 0x04};
    for (int i = 0; i < 3; i++) {
        uint8_t data[16] = {0};
        data[0] = types[i];
        if (types[i] == 0x02) { data[3] = 0x01; data[4] = 0x03; data[5] = 0x09; }
        if (types[i] == 0x04) { data[4] = 0x80; } // normal error code
        uint8_t pkt[22];
        buildPacket(pkt, 0x62, data, 16);
        uart.feedBytes(pkt, 22);
        cn105.loop();
    }

    // Now feed STATUS
    uint8_t data[16] = {0};
    data[0] = 0x06; // CN105_INFO_STATUS
    data[3] = 42;   // compressor 42 Hz
    data[4] = 0x01; // operating

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_EQUAL(42, cn105.getState().compressorHz);
    TEST_ASSERT_TRUE(cn105.getState().operating);
}

void test_standby_response() {
    connectController();
    startPollCycle();

    // Advance through SETTINGS, ROOMTEMP, ERRORCODE, STATUS to reach STANDBY (phase 4)
    uint8_t types[] = {0x02, 0x03, 0x04, 0x06};
    for (int i = 0; i < 4; i++) {
        uint8_t data[16] = {0};
        data[0] = types[i];
        if (types[i] == 0x02) { data[3] = 0x01; data[4] = 0x03; data[5] = 0x09; }
        if (types[i] == 0x04) { data[4] = 0x80; }
        uint8_t pkt[22];
        buildPacket(pkt, 0x62, data, 16);
        uart.feedBytes(pkt, 22);
        cn105.loop();
    }

    // Now feed STANDBY response
    uint8_t data[16] = {0};
    data[0] = 0x09; // CN105_INFO_STANDBY
    data[3] = 0x08; // subMode = STANDBY
    data[4] = 0x03; // stage = MED
    data[5] = 0x02; // autoSubMode = HEAT

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_EQUAL_HEX8(0x08, cn105.getState().subMode);
    TEST_ASSERT_EQUAL_HEX8(0x03, cn105.getState().stage);
    TEST_ASSERT_EQUAL_HEX8(0x02, cn105.getState().autoSubMode);
}

void test_errorcode_response() {
    connectController();
    startPollCycle();

    // Advance through SETTINGS, ROOMTEMP to reach ERRORCODE (phase 2)
    uint8_t types[] = {0x02, 0x03};
    for (int i = 0; i < 2; i++) {
        uint8_t data[16] = {0};
        data[0] = types[i];
        if (types[i] == 0x02) { data[3] = 0x01; data[4] = 0x03; data[5] = 0x09; }
        uint8_t pkt[22];
        buildPacket(pkt, 0x62, data, 16);
        uart.feedBytes(pkt, 22);
        cn105.loop();
    }

    // Feed ERRORCODE response with an error
    uint8_t data[16] = {0};
    data[0] = 0x04; // CN105_INFO_ERRORCODE
    data[4] = 0x42; // error code (not 0x80 = normal)

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_TRUE(cn105.getState().hasError);
    TEST_ASSERT_EQUAL_HEX8(0x42, cn105.getState().errorCode);
}

void test_errorcode_normal() {
    connectController();
    startPollCycle();

    uint8_t types[] = {0x02, 0x03};
    for (int i = 0; i < 2; i++) {
        uint8_t data[16] = {0};
        data[0] = types[i];
        if (types[i] == 0x02) { data[3] = 0x01; data[4] = 0x03; data[5] = 0x09; }
        uint8_t pkt[22];
        buildPacket(pkt, 0x62, data, 16);
        uart.feedBytes(pkt, 22);
        cn105.loop();
    }

    // Feed ERRORCODE response with 0x80 (normal)
    uint8_t data[16] = {0};
    data[0] = 0x04;
    data[4] = 0x80; // normal

    uint8_t pkt[22];
    buildPacket(pkt, 0x62, data, 16);
    uart.feedBytes(pkt, 22);
    cn105.loop();

    TEST_ASSERT_FALSE(cn105.getState().hasError);
}

void test_bad_checksum_rejected() {
    connectController();

    uint8_t pkt[22];
    uint8_t data[16] = {0};
    data[0] = 0x02;
    buildPacket(pkt, 0x62, data, 16);
    pkt[21] = 0xFF; // corrupt checksum

    uart.feedBytes(pkt, 22);
    cn105.loop();

    // State should not have been updated from this packet
    // (power should still be false from default)
    TEST_ASSERT_FALSE(cn105.getState().power);
}

void test_incomplete_packet_timeout() {
    connectController();

    // Feed only 3 bytes (incomplete packet)
    uint8_t partial[] = {0xFC, 0x62, 0x01};
    uart.feedBytes(partial, 3);
    cn105.loop();

    // Advance time past response timeout (1000ms)
    stub_millis += 1500;
    cn105.loop();

    // Should not crash; buffer should be reset
    TEST_ASSERT_TRUE(cn105.isConnected());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_connect_ok);
    RUN_TEST(test_settings_response);
    RUN_TEST(test_settings_enhanced_temp);
    RUN_TEST(test_settings_isee_mode_stripping);
    RUN_TEST(test_roomtemp_enhanced);
    RUN_TEST(test_roomtemp_legacy);
    RUN_TEST(test_roomtemp_outside_invalid);
    RUN_TEST(test_status_response);
    RUN_TEST(test_standby_response);
    RUN_TEST(test_errorcode_response);
    RUN_TEST(test_errorcode_normal);
    RUN_TEST(test_bad_checksum_rejected);
    RUN_TEST(test_incomplete_packet_timeout);
    return UNITY_END();
}
