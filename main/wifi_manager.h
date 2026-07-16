#pragma once

#include <cstdint>
#include <cstddef>

namespace WifiManager {

    /// Initialize WiFi subsystem (esp_netif, esp_wifi, event loop).
    /// Stores apName/apPassword for later enableAP() calls.
    void init(const char* hostname, const char* apName, const char* apPassword);

    /// Connect to WiFi with given credentials, stores in NVS "wifi-creds".
    /// Non-blocking: starts connection, returns true if initiated successfully.
    bool connect(const char* ssid, const char* password);

    /// Connection state (set by event handler, non-blocking).
    bool isConnected();

    /// True while connect() has applied credentials and no GOT_IP has landed
    /// since. isConnected() && !isJoinPending() == "connected on the
    /// credentials last applied" — the recovery portal's success signal
    /// (the bare connected level stays true for the OLD network throughout
    /// a dial-initiated change window).
    bool isJoinPending();

    /// Trial-connect state. connect() with credentials that differ from NVS
    /// enters a trial: nothing is persisted until GOT_IP lands (SUCCESS);
    /// if the join hasn't landed after 30s, the previous credentials are
    /// restored and the STA reconnects to the old network (FAILED, latched
    /// until the next connect()). Same-as-stored credentials skip the trial.
    enum WifiTrialState : uint8_t {
        WIFI_TRIAL_IDLE, WIFI_TRIAL_TESTING, WIFI_TRIAL_SUCCESS, WIFI_TRIAL_FAILED
    };
    WifiTrialState getTrialState();

    /// Main-task tick: commits trial credentials after a successful join,
    /// reverts to the previous network on trial deadline. Call from the
    /// app_main() loop (NVS writes must stay off the 4KB event task).
    void loop();

    /// Wait for connection with timeout. Returns true if connected.
    bool waitForConnection(uint32_t timeoutMs);

    /// WiFi signal strength (returns 0 if not connected).
    int8_t getRSSI();

    /// Times an established connection was lost this boot (reconnect attempts
    /// that fail again don't count — only connected→disconnected transitions).
    uint32_t getDisconnectCount();

    /// The hostname passed to init() (also the mDNS name and AP SSID).
    const char* getHostname();

    /// Current STA SSID into out (NUL-terminated; empty if unknown).
    void getSSID(char* out, size_t len);
    /// Current STA IPv4 as a dotted string into out (empty if no IP).
    void getIP(char* out, size_t len);

    /// Enable AP mode (STA+AP). Creates AP netif lazily on first call.
    void enableAP(const char* apName, const char* apPassword);

    /// Disable AP mode (back to STA only).
    void disableAP();

    /// Load WiFi credentials from NVS namespace "wifi-creds".
    /// Returns true if credentials were found and loaded.
    bool loadCredentials(char* ssid, size_t ssidLen, char* password, size_t passLen);

    /// Save WiFi credentials to NVS namespace "wifi-creds".
    void saveCredentials(const char* ssid, const char* password);

    /// Erase WiFi credentials from NVS namespace "wifi-creds".
    void eraseCredentials();

    /// True if NVS "wifi-creds" holds credentials. Cached (updated by
    /// saveCredentials/eraseCredentials; first call reads NVS once) so the
    /// 1 Hz ESP-NOW STATE builder can call it without an NVS read per tick.
    bool hasCredentials();

    /// WiFi network found during scan
    struct ScannedNetwork {
        char ssid[33];
        int8_t rssi;
        bool secure;
    };

    /// Perform a blocking WiFi scan. Returns number of unique networks found
    /// (up to maxResults). Deduplicates by SSID and sorts by signal strength.
    int scanNetworks(ScannedNetwork* results, int maxResults);

} // namespace WifiManager
