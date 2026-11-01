#!/usr/bin/env python3
"""Exercise the compiled production DNS builder and Wi-Fi scan HTTP handler."""
import ctypes
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:size_t|esp_err_t) " + re.escape(name) +
                      r"\([^;]*?\)\s*\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match[0]


PREFIX = r'''
#include "json_utils.h"
#include "wifi_manager.h"
#include <string>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
struct httpd_req_t {};
static std::string scan_response;
static bool origin_ok = true;
static WifiManager::ScannedNetwork scan_networks[15];
static int scan_count;
static void httpd_resp_set_type(httpd_req_t *, const char *) {}
static void httpd_resp_set_hdr(httpd_req_t *, const char *, const char *) {}
static int httpd_resp_sendstr(httpd_req_t *, const char *s) {
    scan_response = s; return ESP_OK;
}
class WebUI {
public:
    static bool requireSameOrigin(httpd_req_t *) { return origin_ok; }
    static esp_err_t handleWifiScan(httpd_req_t *);
};
int WifiManager::scanNetworks(ScannedNetwork *out, int cap) {
    const int count = scan_count < cap ? scan_count : cap;
    memcpy(out, scan_networks, count * sizeof(*out));
    return count;
}
'''

WRAPPERS = r'''
extern "C" size_t dns(const uint8_t *query, size_t len, uint8_t *out, size_t cap) {
    uint32_t ip;
    const uint8_t addr[] = {192, 168, 4, 1};
    memcpy(&ip, addr, 4);
    return build_response(query, len, out, cap, ip);
}
extern "C" void scan_reset(bool allow) {
    scan_count = 0; scan_response.clear(); origin_ok = allow;
}
extern "C" void scan_add(const char *ssid, int rssi, bool secure) {
    auto &network = scan_networks[scan_count++];
    snprintf(network.ssid, sizeof(network.ssid), "%s", ssid);
    network.rssi = rssi; network.secure = secure;
}
extern "C" const char *scan() {
    httpd_req_t req;
    WebUI::handleWifiScan(&req);
    return scan_response.c_str();
}
extern "C" const char *display_escape(const char *text) {
    static char escaped[256];
    jsonEscape(text, escaped, sizeof(escaped));
    return escaped;
}
'''


def query(name=b"\x07captive\x05apple\x03com\0", typ=1, extra=b"", ar=0):
    return struct.pack("!HHHHHH", 0x1234, 0x0100, 1, 0, 0, ar) + name + struct.pack("!HH", typ, 1) + extra


def opt(options=b""):
    return b"\0" + struct.pack("!HHIH", 41, 1232, 0, len(options)) + options


def name_end(packet, offset):
    while packet[offset]:
        if packet[offset] & 0xc0 == 0xc0:
            return offset + 2
        offset += 1 + packet[offset]
    return offset + 1


def records(packet):
    header = struct.unpack("!HHHHHH", packet[:12])
    offset = name_end(packet, 12) + 4
    result = []
    for section, count in zip(("answer", "authority", "additional"), header[3:]):
        for _ in range(count):
            offset = name_end(packet, offset)
            typ, cls, ttl, length = struct.unpack("!HHIH", packet[offset:offset + 10])
            offset += 10
            data = packet[offset:offset + length]
            assert len(data) == length
            result.append((section, typ, cls, ttl, data))
            offset += length
    assert offset == len(packet), "unaccounted response bytes"
    return header, result


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="web-network-")
        path = Path(cls.tmp.name)
        dns = (ROOT / "main/dns_server.cpp").read_text()
        constants = dns[dns.index("static constexpr size_t DNS_HEADER_SIZE"):dns.index("// ── Helpers")]
        web = (ROOT / "main/web_server.cpp").read_text()
        unit = path / "test.cpp"
        unit.write_text(PREFIX + constants + function(dns, "build_response") + "\n" +
                        function(web, "WebUI::handleWifiScan") + WRAPPERS)
        lib = path / "test.so"
        subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-fPIC", "-I" + str(ROOT / "main"), str(unit),
                        "-o", str(lib)], check=True)
        cls.native = ctypes.CDLL(str(lib))
        cls.native.dns.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        cls.native.dns.restype = ctypes.c_size_t
        cls.native.scan_reset.argtypes = [ctypes.c_bool]
        cls.native.scan_add.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_bool]
        cls.native.scan.restype = ctypes.c_char_p
        cls.native.display_escape.argtypes = [ctypes.c_char_p]
        cls.native.display_escape.restype = ctypes.c_char_p

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def reply(self, packet, cap=528):
        allocation = max(cap + 64, 600)
        output = ctypes.create_string_buffer(b"\xa5" * allocation, allocation)
        length = self.native.dns(packet, len(packet), ctypes.byref(output, 32), cap)
        self.assertLessEqual(length, cap)
        self.assertEqual(output.raw[:32], b"\xa5" * 32)
        self.assertEqual(output.raw[32 + cap:], b"\xa5" * (allocation - 32 - cap))
        return output.raw[32:32 + length]

    def scan(self, names, allow=True):
        self.native.scan_reset(allow)
        for i, name in enumerate(names):
            self.native.scan_add(name.encode(), -100 + i, bool(i % 2))
        return self.native.scan().decode()

    def test_normal_and_edns_put_redirect_in_answer_section(self):
        for additional in (b"", opt(), opt(struct.pack("!HH4s", 10, 4, b"abcd"))):
            with self.subTest(additional=additional):
                packet = query(extra=additional, ar=bool(additional))
                response = self.reply(packet)
                header, rrs = records(response)
                self.assertEqual(header[0], 0x1234)
                self.assertEqual(header[2:5], (1, 1, 0))
                self.assertTrue(header[1] & 0x8000)
                self.assertTrue(header[1] & 0x0100, "retain recursion-desired flag")
                self.assertEqual(rrs[0], ("answer", 1, 1, 60, bytes([192, 168, 4, 1])))
                self.assertEqual(header[5], bool(additional))
                self.assertEqual(rrs[1:], [("additional", 41, 512, 0, b"")] if additional else [])

    def test_non_address_question_does_not_get_an_unrelated_a_answer(self):
        for typ in (28, 15, 16):
            header, rrs = records(self.reply(query(typ=typ)))
            self.assertEqual(header[3], 0)
            self.assertEqual(rrs, [])

    def test_rejects_malformed_question_counts_names_and_edns_lengths(self):
        valid = query(extra=opt(), ar=1)
        malformed = [valid[:n] for n in range(len(valid))]
        malformed += [query(b"\xc0\x0c"), query(b"\xc0\xff"), query(b"\x40" + b"x" * 64 + b"\0"),
                      query(b"\x3f" + b"a" * 63 + (b"\x3f" + b"b" * 63) * 3 + b"\0"),
                      query() + b"ignored", query(extra=opt() + b"x", ar=1),
                      query(extra=opt(b"\0\x0a\0\x04xx"), ar=1)]
        for index, value in ((2, 0x80), (2, 0x08), (4, 1), (5, 0), (5, 2),
                             (7, 1), (9, 1), (10, 1), (11, 2)):
            packet = bytearray(query())
            packet[index] = value
            malformed.append(bytes(packet))
        for packet in malformed:
            with self.subTest(packet=packet.hex()):
                self.assertEqual(self.reply(packet), b"")

    def test_capacity_bounds_and_maximum_name(self):
        packet = query(b"\x3f" + b"a" * 63 + (b"\x3f" + b"b" * 63) * 2 + b"\x3d" + b"c" * 61 + b"\0")
        expected = len(packet) + 16
        for capacity in (0, 1, 11, 15, expected - 1):
            self.assertEqual(self.reply(packet, capacity), b"")
        self.assertEqual(len(self.reply(packet, expected)), expected)
        rng = random.Random(105)
        for _ in range(500):
            self.reply(rng.randbytes(rng.randrange(513)), rng.randrange(529))

    def test_scan_admits_only_complete_entries_and_preserves_escaped_ssids(self):
        for char in ['"', '\\', ' '] + [chr(i) for i in range(1, 32)]:
            with self.subTest(char=char):
                names = [char * 29 + f"{i:03d}" for i in range(15)]
                body = self.scan(names)
                rows = json.loads(body)
                self.assertGreater(len(rows), 0)
                self.assertLessEqual(len(body.encode()), 1199)
                self.assertEqual([row['ssid'] for row in rows], names[:len(rows)])
                for i, row in enumerate(rows):
                    self.assertEqual(row, {"ssid": names[i], "rssi": -100 + i, "secure": bool(i % 2)})

    def test_scan_preserves_maximum_length_utf8_names_and_existing_display_filtering(self):
        names = ['é' * 16, '🏡' * 8]
        self.assertEqual([row['ssid'] for row in json.loads(self.scan(names))], names)
        self.assertEqual(self.native.display_escape(b"first\r\nsecond"), b"first\\nsecond")

    def test_scan_empty_and_origin_rejection(self):
        self.assertEqual(json.loads(self.scan([])), [])
        self.assertEqual(self.scan(["Home"], allow=False), "")


if __name__ == "__main__":
    unittest.main()
