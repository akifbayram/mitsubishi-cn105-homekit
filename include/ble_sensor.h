#pragma once

#include <Arduino.h>
#include "ble_decoders.h"

// Maximum number of discovered sensors to hold during a scan
constexpr uint8_t BLE_MAX_DISCOVERED = 10;

// Scan timing
constexpr uint32_t BLE_SCAN_DURATION_S   = 5;     // seconds per scan window
constexpr uint32_t BLE_SCAN_INTERVAL_MS  = 10000; // ms between scans (monitoring mode)

// Stale timeout: revert to internal sensor if no update in this period
constexpr uint32_t BLE_STALE_TIMEOUT_MS  = 90000; // 90 seconds

// Keepalive: re-send remote temp to heat pump at this interval
constexpr uint32_t BLE_KEEPALIVE_MS      = 20000; // 20 seconds

// Detected BLE advertisement format
enum BLESensorFormat : uint8_t {
    BLE_FMT_UNKNOWN = 0,
    BLE_FMT_GOVEE_3BYTE,
    BLE_FMT_GOVEE_2BYTE,
    BLE_FMT_BTHOME,
    BLE_FMT_181A,
};

// Information about a discovered BLE sensor (from scan)
struct BLEDiscoveredSensor {
    char     name[32]  = "";
    char     addr[18]  = "";       // "AA:BB:CC:DD:EE:FF"
    int      rssi      = 0;
    BLESensorFormat format = BLE_FMT_UNKNOWN;
    float    temperature = 0.0f;   // decoded during discovery if possible
    bool     used       = false;   // slot in use
};

class BLESensor {
public:
    /// Initialize NimBLE stack (call once after WiFi is up)
    void begin();

    /// Call from main loop — manages scan cycles and keepalive
    void loop(class CN105Controller &cn105);

    /// Start a discovery scan (results available after BLE_SCAN_DURATION_S)
    void startDiscoveryScan();

    /// Check if discovery scan is still running
    bool isDiscoveryRunning() const { return _discoveryRunning; }

    /// Get discovery results (valid after scan completes)
    const BLEDiscoveredSensor* getDiscoveredSensors() const { return _discovered; }
    uint8_t getDiscoveredCount() const { return _discoveredCount; }

    /// Set the paired sensor by MAC address string ("AA:BB:CC:DD:EE:FF")
    void setSensor(const char *addr, const char *name);

    /// Clear the paired sensor and revert heat pump to internal sensor
    void clearSensor(class CN105Controller &cn105);

    /// Set remote temperature from external API (overrides BLE while fresh)
    void setApiTemperature(float tempC);

    /// Clear API temperature (resume BLE if configured)
    void clearApiTemperature();

    // ── Current readings ──────────────────────────────────────────────────────
    bool    isActive() const;          // sensor paired and receiving data
    bool    isStale() const;           // paired but no recent data
    float   getTemperature() const { return _temperature; }
    float   getHumidity() const { return _humidity; }
    int8_t  getBattery() const { return _battery; }
    int     getRSSI() const { return _rssi; }
    uint32_t getLastUpdate() const { return _lastUpdate; }
    const char* getSensorName() const { return _sensorName; }
    const char* getSensorAddr() const { return _sensorAddr; }
    bool    isEnabled() const { return _enabled; }
    bool    isApiMode() const { return _apiMode; }

    // Made accessible to scan callback
    float    _temperature    = 0.0f;
    float    _humidity       = -1.0f;
    int8_t   _battery        = -1;
    int      _rssi           = 0;
    uint32_t _lastUpdate     = 0;
    bool     _sentRevert     = false;
    bool     _scanning       = false;
    bool     _discoveryRunning = false;
    BLEDiscoveredSensor _discovered[BLE_MAX_DISCOVERED];
    uint8_t  _discoveredCount = 0;

    static const char* formatToStr(BLESensorFormat fmt);

private:
    bool     _initialized    = false;
    bool     _enabled        = false;   // sensor is paired and active
    bool     _apiMode        = false;   // temperature set via API, BLE paused

    // Paired sensor
    char     _sensorAddr[18] = "";
    char     _sensorName[32] = "";
    uint8_t  _targetAddr[6]  = {};      // parsed MAC bytes for fast comparison

    // Scan timing
    uint32_t _lastScanStart  = 0;

    // Keepalive
    uint32_t _lastKeepalive  = 0;

    // Internal
    void startMonitoringScan();
    void parseMacString(const char *str, uint8_t *out);
};

extern BLESensor bleSensor;
