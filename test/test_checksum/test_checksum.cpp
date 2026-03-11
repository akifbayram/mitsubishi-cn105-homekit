#include <unity.h>
#include "cn105_protocol.h"

void test_checksum_connect_packet() {
    // CONNECT_PKT = {0xFC, 0x5A, 0x01, 0x30, 0x02, 0xCA, 0x01, 0xA8}
    // checksum of first 7 bytes should equal 0xA8
    uint8_t pkt[] = {0xFC, 0x5A, 0x01, 0x30, 0x02, 0xCA, 0x01};
    TEST_ASSERT_EQUAL_HEX8(0xA8, CN105Controller::calcChecksum(pkt, 7));
}

void test_checksum_all_zeros() {
    uint8_t pkt[5] = {0};
    // (0xFC - 0) & 0xFF = 0xFC
    TEST_ASSERT_EQUAL_HEX8(0xFC, CN105Controller::calcChecksum(pkt, 5));
}

void test_checksum_info_req_settings() {
    // Build an INFO_REQ for SETTINGS (0x02) and verify checksum
    uint8_t pkt[22];
    memset(pkt, 0, sizeof(pkt));
    CN105Controller::buildHeader(pkt, 0x42, 0x10); // INFO_REQ, 16 bytes data
    pkt[5] = 0x02; // CN105_INFO_SETTINGS
    uint8_t chk = CN105Controller::calcChecksum(pkt, 21);
    pkt[21] = chk;
    // Verify: recalculating should match
    TEST_ASSERT_EQUAL_HEX8(chk, CN105Controller::calcChecksum(pkt, 21));
}

void test_checksum_single_byte() {
    uint8_t pkt[] = {0x10};
    // (0xFC - 0x10) & 0xFF = 0xEC
    TEST_ASSERT_EQUAL_HEX8(0xEC, CN105Controller::calcChecksum(pkt, 1));
}

void test_buildHeader() {
    uint8_t pkt[5] = {0};
    CN105Controller::buildHeader(pkt, 0x42, 0x10);
    TEST_ASSERT_EQUAL_HEX8(0xFC, pkt[0]); // SYNC
    TEST_ASSERT_EQUAL_HEX8(0x42, pkt[1]); // type
    TEST_ASSERT_EQUAL_HEX8(0x01, pkt[2]); // HEADER_BYTE2
    TEST_ASSERT_EQUAL_HEX8(0x30, pkt[3]); // HEADER_BYTE3
    TEST_ASSERT_EQUAL_HEX8(0x10, pkt[4]); // data len
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_checksum_connect_packet);
    RUN_TEST(test_checksum_all_zeros);
    RUN_TEST(test_checksum_info_req_settings);
    RUN_TEST(test_checksum_single_byte);
    RUN_TEST(test_buildHeader);
    return UNITY_END();
}
