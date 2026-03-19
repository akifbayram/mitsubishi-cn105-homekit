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

    /// Wait for connection with timeout. Returns true if connected.
    bool waitForConnection(uint32_t timeoutMs);

    /// WiFi signal strength (returns 0 if not connected).
    int8_t getRSSI();

    /// Enable AP mode (STA+AP). Creates AP netif lazily on first call.
    void enableAP(const char* apName, const char* apPassword);

    /// Disable AP mode (back to STA only).
    void disableAP();

    /// Returns true if AP mode is currently active.
    bool isAPActive();

    /// Load WiFi credentials from NVS namespace "wifi-creds".
    /// Returns true if credentials were found and loaded.
    bool loadCredentials(char* ssid, size_t ssidLen, char* password, size_t passLen);

    /// Save WiFi credentials to NVS namespace "wifi-creds".
    void saveCredentials(const char* ssid, const char* password);

    /// Erase WiFi credentials from NVS namespace "wifi-creds".
    void eraseCredentials();

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
