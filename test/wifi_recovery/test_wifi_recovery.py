#!/usr/bin/env python3
"""Exercise the complete real recovery implementation with SDK/time doubles.

Only hardware/platform includes are replaced; no recovery methods are copied or
mocked. Each case starts in a fresh process and checks public recovery behavior.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / 'main/wifi_recovery.h').read_text().replace('#include "board_profile.h"', '')
SOURCE = re.sub(r'^#include[^\n]*', '', (ROOT / 'main/wifi_recovery.cpp').read_text(), flags=re.M)
PREFIX = r'''
#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>
#include "wifi_manager.h"
#define PIN_BUTTON -1
#define BRAND_AP_PASSWORD "password"
static void silent_log(const char *, const char *, ...) {}
#define LOG_INFO(...) silent_log(TAG, __VA_ARGS__)
#define LOG_WARN(...) silent_log(TAG, __VA_ARGS__)
#include "sl2_proto.h"
static uint32_t clock_ms = 1000;
uint32_t uptime_ms() { return clock_ms; }
struct Settings {
    struct Data { bool wifiChangePending = false; } data;
    Data &get() { return data; }
    void save() {}
} settings;
namespace WifiManager {
    bool connected = true, joining = false;
    WifiTrialState trial = WIFI_TRIAL_IDLE;
    bool isConnected() { return connected; }
    bool isJoinPending() { return joining; }
    WifiTrialState getTrialState() { return trial; }
    bool hasChipCredentials() { return true; }
    void enableAP(const char *, const char *) {}
    void disableAP() {}
    bool loadCredentials(char *s, size_t, char *p, size_t) {
        strcpy(s, "saved-network"); strcpy(p, "saved-password"); return true;
    }
}
struct esp_netif_t {};
struct esp_ip4_addr_t { uint32_t addr; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip; };
constexpr int ESP_OK = 0;
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *) { return nullptr; }
int esp_netif_get_ip_info(esp_netif_t *, esp_netif_ip_info_t *) { return -1; }
void esp_ip4addr_ntoa(const esp_ip4_addr_t *, char *buf, size_t) { strcpy(buf, "0.0.0.0"); }
void dns_captive_start(uint32_t) {}
void dns_captive_stop() {}
void improv_serial_start(const char *) {}
void improv_serial_stop() {}
'''
CHECKS = r'''
static void tick(WifiRecovery &r, uint32_t elapsed) { clock_ms += elapsed; r.loop(); }
static void start(WifiRecovery &r, bool connected = true) {
    WifiManager::connected = connected;
    r.begin("test-ap", "test"); r.loop(); r.beginChangeWindow();
    assert(r.isAPActive() && settings.get().wifiChangePending);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    const std::string name = argv[1];
    WifiRecovery r;
    if (name == "connected_timeout") {
        start(r); tick(r, 599000); assert(r.isAPActive());
        tick(r, 1000); assert(!r.isAPActive()); assert(!settings.get().wifiChangePending);
    } else if (name == "disconnected_start") {
        start(r, false); WifiManager::connected = true; tick(r, 1000);
        tick(r, 599000); assert(!r.isAPActive()); assert(!settings.get().wifiChangePending);
    } else if (name == "offline_expiry") {
        start(r); WifiManager::connected = false; tick(r, 1000); tick(r, 599000);
        assert(r.isAPActive()); assert(!settings.get().wifiChangePending);
        WifiManager::connected = true; tick(r, 1000); tick(r, 6000); assert(!r.isAPActive());
    } else if (name == "window_wrap" || name == "window_zero") {
        clock_ms = name == "window_wrap" ? UINT32_MAX - 299999 : 0;
        start(r); tick(r, 1000); assert(r.isAPActive());
        tick(r, 598000); assert(r.isAPActive()); tick(r, 1000); assert(!r.isAPActive());
        assert(!settings.get().wifiChangePending);
    } else if (name == "linger_wrap") {
        clock_ms = UINT32_MAX - 2999;
        r.begin("test-ap", "test"); r.activateNow(); r.loop();
        tick(r, 1000); assert(r.isAPActive()); tick(r, 5000); assert(!r.isAPActive());
    } else if (name == "old_network_blip") {
        start(r); WifiManager::connected = false; tick(r, 1000);
        WifiManager::connected = true; tick(r, 1000); tick(r, 6000);
        assert(r.isAPActive() && settings.get().wifiChangePending);
        tick(r, 592000); assert(!r.isAPActive()); assert(!settings.get().wifiChangePending);
    } else if (name == "cancel_connected") {
        start(r); assert(r.cancelChangeWindow() == 0); assert(!r.isAPActive());
        assert(!settings.get().wifiChangePending); assert(r.cancelChangeWindow() == 0);
    } else if (name == "cancel_disconnected" || name == "cancel_join_pending") {
        start(r, name == "cancel_join_pending");
        WifiManager::joining = name == "cancel_join_pending";
        assert(r.cancelChangeWindow() == 2); assert(r.isAPActive()); assert(!settings.get().wifiChangePending);
        WifiManager::connected = true; WifiManager::joining = false;
        tick(r, 1000); tick(r, 6000); assert(!r.isAPActive());
    } else if (name == "initial_ap_preserved") {
        r.begin("test-ap", "test"); r.activateNow();
        assert(r.cancelChangeWindow() == 2); assert(r.isAPActive());
    } else if (name == "trial_success" || name == "trial_rollback" || name == "trial_offline") {
        start(r); r.noteReprovision(); WifiManager::joining = true;
        WifiManager::trial = WifiManager::WIFI_TRIAL_TESTING;
        WifiManager::connected = false;
        assert(r.cancelChangeWindow() == 1); assert(r.isAPActive() && settings.get().wifiChangePending);
        tick(r, 1000);
        // GOT_IP may be visible before the main-task trial verdict/commit.
        WifiManager::connected = true; WifiManager::joining = false; tick(r, 1000);
        assert(r.cancelChangeWindow() == 1); tick(r, 6000);
        assert(r.isAPActive() && settings.get().wifiChangePending);
        WifiManager::trial = name == "trial_success" ? WifiManager::WIFI_TRIAL_SUCCESS : WifiManager::WIFI_TRIAL_FAILED;
        WifiManager::connected = name != "trial_offline";
        assert(r.cancelChangeWindow() == (name == "trial_offline" ? 2 : 0));
        assert(r.isAPActive() == (name == "trial_offline")); assert(!settings.get().wifiChangePending);
    } else if (name == "expiry_during_trial") {
        start(r); tick(r, 599000); r.noteReprovision();
        WifiManager::trial = WifiManager::WIFI_TRIAL_TESTING;
        WifiManager::joining = true; WifiManager::connected = false;
        tick(r, 1000); assert(r.isAPActive() && settings.get().wifiChangePending);
        WifiManager::trial = WifiManager::WIFI_TRIAL_FAILED;
        WifiManager::joining = false; tick(r, 1000);
        assert(r.isAPActive() && !settings.get().wifiChangePending);
        WifiManager::connected = true; tick(r, 1000); tick(r, 6000); assert(!r.isAPActive());
    } else if (name == "fast_reprovision") {
        start(r); r.noteReprovision(); WifiManager::trial = WifiManager::WIFI_TRIAL_SUCCESS;
        tick(r, 1000); assert(!settings.get().wifiChangePending); assert(r.isAPActive());
        tick(r, 6000); assert(!r.isAPActive());
    } else { assert(false); }
    printf("PASS: %s\n", name.c_str());
}
'''
CASES = ['connected_timeout', 'disconnected_start', 'offline_expiry', 'window_wrap',
         'window_zero', 'linger_wrap', 'old_network_blip', 'cancel_connected',
         'cancel_disconnected', 'cancel_join_pending', 'initial_ap_preserved',
         'trial_success', 'trial_rollback', 'trial_offline', 'expiry_during_trial', 'fast_reprovision']
with tempfile.TemporaryDirectory(prefix='wifi-recovery-') as tmp:
    unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    unit.write_text(PREFIX + HEADER.replace('#pragma once', '') + SOURCE + CHECKS)
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I' + str(ROOT / 'main'), str(unit), '-o', str(exe)], check=True)
    results = [subprocess.run([str(exe), case], env=os.environ).returncode for case in CASES]
    raise SystemExit(any(results))
