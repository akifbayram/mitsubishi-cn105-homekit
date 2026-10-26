#!/usr/bin/env python3
"""Compile the real WiFi trial/join/read functions with simulated SDK and NVS.

The checks catch truncation in initial joins, persisted reconnects and trial
rollback, prevent a full-width SSID read from spilling into the password, and
block persistable Wi-Fi driver changes once factory reset starts.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "main/wifi_manager.cpp").read_text()


def function(name, source=SOURCE):
    match = re.search(r"^(?:static )?(?:bool|void|WifiManager::WifiTrialState) " + re.escape(name)
                      + r"\([^;]*?\)\s*\n?\{.*?^\}", source, re.MULTILINE | re.DOTALL)
    assert match, f"cannot find {name}"
    return match[0]


PREFIX = r'''
#include "wifi_manager.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <string>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
using StaticSemaphore_t = std::mutex;
using SemaphoreHandle_t = std::mutex*;
constexpr int portMAX_DELAY = -1;
std::mutex coordination;
std::condition_variable progress;
bool block_commit = false, commit_entered = false, release_commit = false;
bool second_started = false, second_finished = false, operation_waiting = false;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
int xSemaphoreTake(SemaphoreHandle_t sem, int) {
    if (!sem->try_lock()) {
        { std::lock_guard<std::mutex> lock(coordination); operation_waiting = true; }
        progress.notify_all();
        sem->lock();
    }
    return 1;
}
void xSemaphoreGive(SemaphoreHandle_t sem) { sem->unlock(); }
#define configASSERT(expr) assert(expr)
using esp_err_t = int;
using esp_event_base_t = int;
constexpr int ESP_OK = 0, WIFI_IF_STA = 0, WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3;
constexpr int WIFI_IF_AP = 1, WIFI_MODE_STA = 1, WIFI_MODE_APSTA = 3;
constexpr int WIFI_EVENT = 1, IP_EVENT = 2, WIFI_EVENT_STA_CONNECTED = 3;
constexpr int WIFI_EVENT_STA_DISCONNECTED = 4, IP_EVENT_STA_GOT_IP = 5, CONNECTED_BIT = 1;
struct wifi_config_t {
    struct {
        uint8_t ssid[32], password[64];
        struct { int authmode; } threshold;
        struct { bool capable, required; } pmf_cfg;
    } sta;
    struct {
        uint8_t ssid[32], password[64], ssid_len;
        int authmode, channel, max_connection, beacon_interval;
    } ap;
};
// Compile the real boot latch below; only its class declaration is a fixture.
class SettingsStore {
public:
    static void beginFactoryReset();
    static bool isFactoryResetting();
};
static SettingsStore settings;
struct esp_netif_t {};
static esp_netif_t *s_apNetif = nullptr;
static bool s_apActive = false;
static int modeWrites, apWrites, driverMode = WIFI_MODE_STA;
static wifi_config_t apDriver;
static esp_netif_t *esp_netif_create_default_wifi_ap() { static esp_netif_t netif; return &netif; }
static int esp_wifi_set_mode(int mode) { ++modeWrites; driverMode = mode; return ESP_OK; }
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
static bool read_ok = true, config_failure = false;
static int nvs_failure = 0, s_haveCreds = -1;
static wifi_config_t driver;
static std::string stored_ssid, stored_psk;
static uint32_t uptime_ms() { return now_ms; }
static esp_err_t esp_wifi_set_config(int iface, const wifi_config_t *cfg)
{
    if (config_failure) return -2;
    if (iface == WIFI_IF_AP) { apDriver = *cfg; ++apWrites; }
    else { assert(iface == WIFI_IF_STA); driver = *cfg; ++writes; }
    return ESP_OK;
}
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
using nvs_handle_t = int;
constexpr int NVS_READWRITE = 1;
static const char *NVS_NAMESPACE = "wifi-creds", *NVS_KEY_SSID = "ssid", *NVS_KEY_PASS = "password";
std::string pending_ssid, pending_psk;
int nvs_open(const char *, int, nvs_handle_t *handle) {
    *handle = 1; return nvs_failure == 1 ? -1 : ESP_OK;
}
int nvs_set_str(nvs_handle_t, const char *key, const char *value) {
    if (strcmp(key, "ssid") == 0) {
        if (nvs_failure == 2) return -1;
        pending_ssid = value;
    } else {
        if (nvs_failure == 3) return -1;
        pending_psk = value;
    }
    return ESP_OK;
}
int nvs_commit(nvs_handle_t) {
    if (block_commit) {
        std::unique_lock<std::mutex> lock(coordination);
        commit_entered = true;
        progress.notify_all();
        progress.wait(lock, [] { return release_commit; });
    }
    if (nvs_failure == 4) return -1;
    stored_ssid = pending_ssid; stored_psk = pending_psk; ++saves;
    return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
'''

CHECKS = r'''
static void fresh(const std::string &ssid = "", const std::string &psk = "")
{
    s_trial = {};
    s_connected = s_wifiScanning = s_expectDisconnect = s_pendingJoin = false;
    s_retryCount = s_disconnects = 0;
    now_ms += 60000; // Keep the real loop's private clock gate moving forward.
    writes = connects = saves = modeWrites = apWrites = 0;
    s_apActive = false; driverMode = WIFI_MODE_STA;
    stored_ssid = ssid; stored_psk = psk;
    memset(&driver, 0xa5, sizeof driver);
    read_ok = true;
    config_failure = false; nvs_failure = 0; s_haveCreds = -1;
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
static void reset_checks(const char *name)
{
    fresh("old-network", "old-password");
    assert(WifiManager::connect("trial-network", "trial-password"));
    if (!strcmp(name, "disable_ap")) WifiManager::enableAP("recovery-ap", "password");
    const int configBefore = writes, apBefore = apWrites, modesBefore = modeWrites, savesBefore = saves;
    settings.beginFactoryReset();
    assert(settings.isFactoryResetting());
    if (!strcmp(name, "expired_trial")) {
        now_ms += 31000;
        WifiManager::loop();
    } else if (!strcmp(name, "trial_commit")) {
        got_ip();
    } else if (!strcmp(name, "sta_config")) {
        assert(!applyStaConfig("late-network", "late-password"));
    } else if (!strcmp(name, "save")) {
        assert(!WifiManager::saveCredentials("late-network", "late-password"));
    } else if (!strcmp(name, "connect")) {
        assert(!WifiManager::connect("late-network", "late-password"));
    } else if (!strcmp(name, "enable_ap")) {
        WifiManager::enableAP("late-ap", "late-password");
    } else if (!strcmp(name, "disable_ap")) {
        WifiManager::disableAP();
    } else { assert(false); }
    assert(writes == configBefore && apWrites == apBefore && modeWrites == modesBefore && saves == savesBefore);
}
int main(int argc, char **argv)
{
    init_test_mutex();
    if (argc > 1) {
        if (!strncmp(argv[1], "factory_reset/", 14)) {
            reset_checks(argv[1] + 14);
            printf("PASS: %s\n", argv[1]);
            return 0;
        }
        return regression(argv[1]);
    }
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
    // Ordinary recovery AP changes must still reach the driver.
    fresh();
    WifiManager::enableAP("recovery-ap", "password");
    assert(s_apActive && modeWrites == 1 && apWrites == 1 && driverMode == WIFI_MODE_APSTA);
    assert(!strcmp((const char*)apDriver.ap.ssid, "recovery-ap"));
    WifiManager::disableAP();
    assert(!s_apActive && modeWrites == 2 && driverMode == WIFI_MODE_STA);
    puts("Wi-Fi credentials: join, commit, reconnect, rollback and bounded SSID reads passed");
}
'''

REGRESSIONS = r'''
static int regression(const std::string &name) {
    fresh("old-network", "old-password");
    if (name == "config_rejected") {
        assert(WifiManager::connect("first-trial", "first-password"));
        config_failure = true;
        assert(!WifiManager::connect("rejected-trial", "second-password"));
        assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_TESTING);
        config_failure = false;
        got_ip();
        assert(stored_ssid == "first-trial");
    } else if (name == "rollback_retries") {
        assert(WifiManager::connect("new-network", "incorrect-password"));
        config_failure = true;
        now_ms += 31000;
        WifiManager::loop();
        assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_TESTING);
        assert(connects == 1);
        config_failure = false;
        now_ms += 300;
        WifiManager::loop();
        assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_FAILED);
        expect_driver("old-network", "old-password");
    } else if (name.rfind("nvs_", 0) == 0) {
        assert(WifiManager::connect("new-network", "new-password"));
        nvs_failure = std::stoi(name.substr(4));
        got_ip();
        assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_TESTING);
        assert(s_haveCreds != 1 && saves == 0);
        nvs_failure = 0;
        now_ms += 1000;
        WifiManager::loop();
        assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_SUCCESS);
        assert(stored_ssid == "new-network" && stored_psk == "new-password");
        assert(saves == 1 && s_haveCreds == 1);
    } else if (name == "concurrent_submission") {
        assert(WifiManager::connect("first-trial", "first-password"));
        ip_event_got_ip_t event = {};
        wifi_event_handler(nullptr, IP_EVENT, IP_EVENT_STA_GOT_IP, &event);
        now_ms += 300;
        block_commit = true;
        std::thread committer([] { WifiManager::loop(); });
        {
            std::unique_lock<std::mutex> lock(coordination);
            assert(progress.wait_for(lock, std::chrono::seconds(3), [] { return commit_entered; }));
        }
        std::thread submitter([] {
            assert(WifiManager::connect("newer-trial", "newer-password"));
            { std::lock_guard<std::mutex> lock(coordination); second_finished = true; }
            progress.notify_all();
        });
        {
            std::unique_lock<std::mutex> lock(coordination);
            assert(progress.wait_for(lock, std::chrono::seconds(3), [] {
                return operation_waiting || second_finished;
            }));
            release_commit = true;
        }
        progress.notify_all();
        committer.join(); submitter.join();
        block_commit = false;
        assert(WifiManager::getTrialState() == WifiManager::WIFI_TRIAL_TESTING);
        assert(stored_ssid == "first-trial");
        got_ip();
        assert(stored_ssid == "newer-trial" && stored_psk == "newer-password");
    } else { assert(false); }
    printf("PASS: %s\n", name.c_str());
    return 0;
}
'''

state = re.search(r"^static constexpr uint32_t TRIAL_TIMEOUT_MS.*?^\} s_trial;",
                  SOURCE, re.MULTILINE | re.DOTALL)
assert state, "cannot find trial state"
settings_source = (ROOT / "main/settings.cpp").read_text()
latch = re.search(r"^static std::atomic<bool> s_factoryResetting[^;]+;", settings_source, re.M)
assert latch, "cannot find factory reset latch"
write_mutex = re.search(r"^static std::mutex s_settingsWriteMutex;", settings_source, re.M)
assert write_mutex, "cannot find settings write/reset mutex"
reset_state = latch[0] + "\n" + write_mutex[0] + "\n" + "\n".join(function(n, settings_source) for n in
    ["SettingsStore::beginFactoryReset", "SettingsStore::isFactoryResetting"])
functions = ["wifi_event_handler", "applyStaConfig", "WifiManager::connect",
             "WifiManager::getTrialState", "WifiManager::loop", "WifiManager::getSSID",
             "WifiManager::enableAP", "WifiManager::disableAP",
             "WifiManager::saveCredentials"]
with tempfile.TemporaryDirectory(prefix="homekit-wifi-creds-") as tmp:
    unit = Path(tmp) / "test.cpp"
    mutex_init = "static void init_test_mutex() { s_trialOps = xSemaphoreCreateMutexStatic(&s_trialOpsStorage); }\n"
    declarations = "static int regression(const std::string &name);\n"
    unit.write_text(PREFIX + "\n" + reset_state + "\n" + state[0] + "\n"
                    + "\n".join(function(n) for n in functions)
                    + mutex_init + declarations + CHECKS + REGRESSIONS)
    exe = Path(tmp) / "test"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-pthread", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-g", "-I" + str(ROOT / "main"),
                    str(unit), "-o", str(exe)], check=True)
    cases = sys.argv[1:] or [
        "baseline", "config_rejected", "rollback_retries", "nvs_1", "nvs_2",
        "nvs_3", "nvs_4", "concurrent_submission",
        "factory_reset/expired_trial", "factory_reset/trial_commit",
        "factory_reset/sta_config", "factory_reset/connect",
        "factory_reset/enable_ap", "factory_reset/disable_ap", "factory_reset/save",
    ]
    results = [subprocess.run([str(exe)] + ([] if case == "baseline" else [case])).returncode
               for case in cases]
    raise SystemExit(any(results))
