#!/usr/bin/env python3
"""Compile the real WiFi trial/join/read functions with simulated SDK and NVS.

The checks catch truncation in initial joins, persisted reconnects and trial
rollback, and prevent a full-width SSID read from spilling into the password.
"""
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "main/wifi_manager.cpp").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:bool|void|WifiManager::WifiTrialState) " + re.escape(name)
                      + r"\([^;]*?\)\s*\n\{.*?^\}", SOURCE, re.MULTILINE | re.DOTALL)
    assert match, f"cannot find {name}"
    return match[0]


PREFIX = r'''
#include "wifi_manager.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
using esp_err_t = int;
using esp_event_base_t = int;
constexpr int ESP_OK = 0, WIFI_IF_STA = 0, WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3;
constexpr int WIFI_EVENT = 1, IP_EVENT = 2, WIFI_EVENT_STA_CONNECTED = 3;
constexpr int WIFI_EVENT_STA_DISCONNECTED = 4, IP_EVENT_STA_GOT_IP = 5, CONNECTED_BIT = 1;
struct wifi_config_t {
    struct {
        uint8_t ssid[32], password[64];
        struct { int authmode; } threshold;
        struct { bool capable, required; } pmf_cfg;
    } sta;
};
struct ip_event_got_ip_t { struct { int ip; } ip_info; };
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#define ESP_ERROR_CHECK(expr) assert((expr) == ESP_OK)
#define IPSTR "%d"
#define IP2STR(addr) (*(addr))
static void silent_log(const char *, ...) {}
#define LOG_INFO(...) silent_log(__VA_ARGS__)
#define LOG_WARN(...) silent_log(__VA_ARGS__)
#define LOG_ERROR(...) silent_log(__VA_ARGS__)

static uint32_t now_ms, s_retryCount, s_disconnects;
static bool s_connected, s_wifiScanning, s_expectDisconnect, s_pendingJoin;
static void *s_wifiEventGroup = nullptr, *s_reconnectTimer = nullptr;
static int writes, connects, saves;
static bool read_ok = true;
static wifi_config_t driver;
static std::string stored_ssid, stored_psk;
static uint32_t uptime_ms() { return now_ms; }
static esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *cfg)
{ assert(iface == WIFI_IF_STA); driver = *cfg; ++writes; return ESP_OK; }
static esp_err_t esp_wifi_get_config(int iface, wifi_config_t *cfg)
{ assert(iface == WIFI_IF_STA); if (!read_ok) return -1; *cfg = driver; return ESP_OK; }
static esp_err_t esp_wifi_connect() { ++connects; return ESP_OK; }
static void wifi_event_handler(void *, esp_event_base_t, int32_t, void *);
static void esp_wifi_disconnect()
{ wifi_event_handler(nullptr, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr); }
static int esp_timer_start_once(void *, uint64_t) { return ESP_OK; }
static void esp_timer_stop(void *) {}
static void xEventGroupClearBits(void *, int) {}
static void xEventGroupSetBits(void *, int) {}
static const char *esp_err_to_name(int) { return "simulated SDK error"; }

bool WifiManager::loadCredentials(char *ssid, size_t sl, char *psk, size_t pl)
{
    if (stored_ssid.empty()) return false;
    assert(sl > stored_ssid.size() && pl > stored_psk.size());
    memcpy(ssid, stored_ssid.c_str(), stored_ssid.size() + 1);
    memcpy(psk, stored_psk.c_str(), stored_psk.size() + 1);
    return true;
}
void WifiManager::saveCredentials(const char *ssid, const char *psk)
{ stored_ssid = ssid; stored_psk = psk; ++saves; }
'''

CHECKS = r'''
static void fresh(const std::string &ssid = "", const std::string &psk = "")
{
    s_trial = {};
    s_connected = s_wifiScanning = s_expectDisconnect = s_pendingJoin = false;
    s_retryCount = s_disconnects = 0;
    now_ms += 60000; // Keep the real loop's private clock gate moving forward.
    writes = connects = saves = 0;
    stored_ssid = ssid; stored_psk = psk;
    memset(&driver, 0xa5, sizeof driver);
    read_ok = true;
}
static void expect_driver(const std::string &ssid, const std::string &psk)
{
    assert(memcmp(driver.sta.ssid, ssid.data(), ssid.size()) == 0);
    assert(memcmp(driver.sta.password, psk.data(), psk.size()) == 0);
    for (size_t i = ssid.size(); i < sizeof driver.sta.ssid; ++i) assert(driver.sta.ssid[i] == 0);
    for (size_t i = psk.size(); i < sizeof driver.sta.password; ++i) assert(driver.sta.password[i] == 0);
    assert(driver.sta.threshold.authmode == (psk.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK));
    assert(driver.sta.pmf_cfg.capable && !driver.sta.pmf_cfg.required);
}
static void got_ip()
{
    ip_event_got_ip_t event = {};
    wifi_event_handler(nullptr, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
    now_ms += 300;
    WifiManager::loop();
}
int main()
{
    const std::string max_ssid(32, 's'), max_key(64, 'a');
    // Initial join -> real IP event -> delayed persistence -> normal reconnect.
    fresh();
    assert(WifiManager::connect(max_ssid.c_str(), max_key.c_str()));
    expect_driver(max_ssid, max_key);
    assert(stored_ssid.empty() && saves == 0);
    got_ip();
    assert(stored_ssid == max_ssid && stored_psk == max_key && saves == 1);
    assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_SUCCESS);
    assert(WifiManager::connect(stored_ssid.c_str(), stored_psk.c_str()));
    expect_driver(max_ssid, max_key);
    assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_IDLE);

    // Failed network change restores every byte of the previous credentials.
    fresh(max_ssid, max_key);
    assert(WifiManager::connect("new-network", "incorrect-passphrase"));
    expect_driver("new-network", "incorrect-passphrase");
    now_ms += 31000;
    WifiManager::loop();
    expect_driver(max_ssid, max_key);
    assert(writes == 2 && connects == 2 && saves == 0);
    assert(stored_ssid == max_ssid && stored_psk == max_key);
    assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_FAILED);

    // Empty/nullable passwords remain open networks; lengths are byte counts.
    for (size_t sl : {size_t(1), size_t(31), size_t(32)}) {
        for (size_t pl : {size_t(0), size_t(8), size_t(63), size_t(64)}) {
            fresh();
            std::string ssid(sl, 's'), psk(pl, 'a');
            assert(WifiManager::connect(ssid.c_str(), psk.c_str()));
            expect_driver(ssid, psk);
        }
    }
    fresh();
    const std::string utf8 = u8"éééééééééééééééé";
    assert(utf8.size() == 32);
    assert(WifiManager::connect(utf8.c_str(), nullptr));
    expect_driver(utf8, "");

    // Oversized input must not change the current trial or saved credentials.
    const std::string long_ssid(33, 's'), long_key(65, 'a');
    assert(!WifiManager::connect(long_ssid.c_str(), "ordinary-passphrase"));
    assert(!WifiManager::connect("Home", long_key.c_str()));
    assert(!WifiManager::connect("", "ordinary-passphrase"));
    assert(!WifiManager::connect(nullptr, "ordinary-passphrase"));
    assert(writes == 1 && connects == 1 && saves == 0);
    got_ip();
    assert(stored_ssid == utf8 && stored_psk.empty());

    // An unterminated full-width driver SSID must never include password bytes.
    fresh();
    assert(WifiManager::connect(max_ssid.c_str(), max_key.c_str()));
    for (size_t cap : {size_t(1), size_t(8), size_t(33), size_t(128)}) {
        char output[129]; memset(output, '!', sizeof output);
        WifiManager::getSSID(output, cap);
        const size_t n = std::min(max_ssid.size(), cap - 1);
        assert(memcmp(output, max_ssid.data(), n) == 0 && output[n] == 0);
        assert(output[cap] == '!');
    }
    char output[10] = "unchanged";
    WifiManager::getSSID(output, 0); assert(strcmp(output, "unchanged") == 0);
    WifiManager::getSSID(nullptr, 10);
    read_ok = false;
    WifiManager::getSSID(output, sizeof output); assert(output[0] == 0);
    puts("Wi-Fi credentials: join, commit, reconnect, rollback and bounded SSID reads passed");
}
'''

state = re.search(r"^static constexpr uint32_t TRIAL_TIMEOUT_MS.*?^\} s_trial;",
                  SOURCE, re.MULTILINE | re.DOTALL)
assert state, "cannot find trial state"
functions = ["wifi_event_handler", "applyStaConfig", "WifiManager::connect",
             "WifiManager::getTrialState", "WifiManager::loop", "WifiManager::getSSID"]
with tempfile.TemporaryDirectory(prefix="homekit-wifi-creds-") as tmp:
    unit = Path(tmp) / "test.cpp"
    unit.write_text(PREFIX + "\n" + state[0] + "\n" + "\n".join(function(n) for n in functions) + CHECKS)
    exe = Path(tmp) / "test"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-g", "-I" + str(ROOT / "main"),
                    str(unit), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
