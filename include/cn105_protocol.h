#pragma once

#include <Arduino.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include "logging.h"
#include "uart_interface.h"
#ifndef UNIT_TEST
#include "hardware_uart.h"
#endif

// ── CN105 Packet Constants ──────────────────────────────────────────────────
constexpr uint8_t CN105_SYNC           = 0xFC;
constexpr uint8_t CN105_PKT_CONNECT    = 0x5A;
constexpr uint8_t CN105_PKT_CONNECT_OK = 0x7A;
constexpr uint8_t CN105_PKT_SET        = 0x41;
constexpr uint8_t CN105_PKT_SET_ACK    = 0x61;
constexpr uint8_t CN105_PKT_INFO_REQ   = 0x42;
constexpr uint8_t CN105_PKT_INFO_RESP  = 0x62;

constexpr uint8_t CN105_HEADER_BYTE2   = 0x01;
constexpr uint8_t CN105_HEADER_BYTE3   = 0x30;
constexpr uint8_t CN105_DATA_LEN       = 0x10;  // 16 bytes data

// ── Info Request Types ──────────────────────────────────────────────────────
constexpr uint8_t CN105_INFO_SETTINGS  = 0x02;
constexpr uint8_t CN105_INFO_ROOMTEMP  = 0x03;
constexpr uint8_t CN105_INFO_STATUS    = 0x06;
constexpr uint8_t CN105_INFO_STANDBY   = 0x09;
constexpr uint8_t CN105_INFO_ERRORCODE = 0x04;

// ── Set Command Flags (byte[6] bitmask) ─────────────────────────────────────
constexpr uint8_t CN105_FLAG_POWER     = 0x01;
constexpr uint8_t CN105_FLAG_MODE      = 0x02;
constexpr uint8_t CN105_FLAG_TEMP      = 0x04;
constexpr uint8_t CN105_FLAG_FAN       = 0x08;
constexpr uint8_t CN105_FLAG_VANE      = 0x10;

// ── Set Command Flags (byte[7] bitmask — second control byte) ────────────
constexpr uint8_t CN105_FLAG2_WVANE    = 0x01;

// ── Power Values ────────────────────────────────────────────────────────────
constexpr uint8_t CN105_POWER_OFF      = 0x00;
constexpr uint8_t CN105_POWER_ON       = 0x01;

// ── Mode Values ─────────────────────────────────────────────────────────────
constexpr uint8_t CN105_MODE_HEAT      = 0x01;
constexpr uint8_t CN105_MODE_DRY       = 0x02;
constexpr uint8_t CN105_MODE_COOL      = 0x03;
constexpr uint8_t CN105_MODE_FAN       = 0x07;
constexpr uint8_t CN105_MODE_AUTO      = 0x08;

// ── Fan Speed Values ────────────────────────────────────────────────────────
constexpr uint8_t CN105_FAN_AUTO       = 0x00;
constexpr uint8_t CN105_FAN_QUIET      = 0x01;
constexpr uint8_t CN105_FAN_1          = 0x02;
constexpr uint8_t CN105_FAN_2          = 0x03;
constexpr uint8_t CN105_FAN_3          = 0x05;
constexpr uint8_t CN105_FAN_4          = 0x06;

// ── Vane (vertical) Position Values ───────────────────────────────────────
constexpr uint8_t CN105_VANE_AUTO       = 0x00;
constexpr uint8_t CN105_VANE_1          = 0x01;  // most horizontal
constexpr uint8_t CN105_VANE_2          = 0x02;
constexpr uint8_t CN105_VANE_3          = 0x03;  // middle
constexpr uint8_t CN105_VANE_4          = 0x04;
constexpr uint8_t CN105_VANE_5          = 0x05;  // most vertical
constexpr uint8_t CN105_VANE_SWING      = 0x07;

// ── Wide Vane (horizontal) Position Values ──────────────────────────────
constexpr uint8_t CN105_WVANE_LEFT_LEFT   = 0x01;
constexpr uint8_t CN105_WVANE_LEFT        = 0x02;
constexpr uint8_t CN105_WVANE_CENTER      = 0x03;
constexpr uint8_t CN105_WVANE_RIGHT       = 0x04;
constexpr uint8_t CN105_WVANE_RIGHT_RIGHT = 0x05;
constexpr uint8_t CN105_WVANE_SPLIT       = 0x08;
constexpr uint8_t CN105_WVANE_SWING       = 0x0C;

// ── Temperature Encoding ────────────────────────────────────────────────────
constexpr float CN105_TEMP_MIN         = 16.0f;
constexpr float CN105_TEMP_MAX         = 31.0f;

// ── Timing ──────────────────────────────────────────────────────────────────
// Follows MitsubishiCN105ESPHome reference project timing
constexpr uint32_t CN105_BAUD_RATE         = 2400;
constexpr uint32_t CN105_CONNECT_TIMEOUT   = 2000;   // ms
constexpr uint32_t CN105_RESPONSE_TIMEOUT  = 1000;   // ms
constexpr uint32_t CN105_UPDATE_INTERVAL   = 2000;    // ms — matches reference default (2s)
constexpr uint32_t CN105_CONNECT_INTERVAL  = 3000;    // ms between connect retries
constexpr uint32_t CN105_DEFER_DELAY       = 750;     // ms — defer next cycle after set command
constexpr uint8_t  CN105_MAX_CONNECT_RETRIES = 5;

// ── Communication Health ─────────────────────────────────────────────────────
// Configurable timeout for detecting CN105 communication failure.
// If no valid response is received within this period, the device is
// considered disconnected and HomeKit will report "Not Responding".
// Default: 6 × update_interval (12s) — more responsive than ESPHome reference.
constexpr uint32_t CN105_COMMS_TIMEOUT     = CN105_UPDATE_INTERVAL * 6;  // 12000 ms

// Number of info request types polled per cycle
constexpr uint8_t  CN105_POLL_PHASE_COUNT  = 5;   // 0x02, 0x03, 0x04, 0x06, 0x09

// ── State structure ─────────────────────────────────────────────────────────
struct CN105State {
    bool     power       = false;
    uint8_t  mode        = CN105_MODE_AUTO;
    float    targetTemp  = 22.0f;
    uint8_t  fanSpeed    = CN105_FAN_AUTO;
    uint8_t  vane        = CN105_VANE_AUTO;
    uint8_t  wideVane    = CN105_WVANE_CENTER;
    float    roomTemp    = 20.0f;
    bool     operating   = false;   // compressor actively running
    uint8_t  compressorHz = 0;
    float    outsideTemp  = 0.0f;   // outside air temperature (from 0x03 data[5])
    bool     outsideTempValid = false; // false if unit doesn't report OAT
    uint8_t  subMode     = 0;         // 0x00=NORMAL, 0x02=DEFROST, 0x04=PREHEAT, 0x08=STANDBY
    uint8_t  stage       = 0;         // 0x00=IDLE..0x06=DIFFUSE (actual indoor fan activity)
    uint8_t  autoSubMode = 0;         // 0x00=OFF, 0x01=COOL, 0x02=HEAT, 0x03=LEADER (Auto only)
    uint8_t  errorCode  = 0x80;       // 0x80 = normal, other = error (from 0x04)
    bool     hasError   = false;
    float    runtimeHours = 0.0f;     // accumulated runtime from 0x03 data[11:13]
    bool     runtimeValid = false;
    bool     connected   = false;
    uint32_t lastUpdate  = 0;
};

// ── Wanted settings (echavet anti-flicker pattern) ────────────────────────
// Tracks user-commanded values per-field. During the grace window after a
// command, getters return wanted values instead of actual heat pump state,
// preventing UI flicker while the heat pump processes the command.
struct WantedSettings {
    bool     power       = false;
    uint8_t  mode        = 0;
    float    targetTemp  = 0;
    uint8_t  fanSpeed    = 0;
    uint8_t  vane        = 0;
    uint8_t  wideVane    = 0;

    // Per-field flags: true = user has requested this value
    bool hasPower    = false;
    bool hasMode     = false;
    bool hasTemp     = false;
    bool hasFan      = false;
    bool hasVane     = false;
    bool hasWideVane = false;

    bool hasBeenSent = false;   // Set packet was transmitted to heat pump
    uint32_t lastChange = 0;    // millis() of most recent user command
};

// ── CN105 Controller Class ──────────────────────────────────────────────────
class CN105Controller {
public:
    CN105Controller();

    /// Initialize UART via ESP-IDF driver (call once in setup)
    void begin(uart_port_t uartNum, int rxPin, int txPin);

    /// Initialize with an injected UART (for testing)
    void begin(UartInterface *uart);

    /// Must be called frequently from loop()
    void loop();

    /// Returns true when communication with the unit is established
    bool isConnected() const { return _state.connected; }

    /// Returns true when connected AND last response was within CN105_COMMS_TIMEOUT.
    /// Use this from HomeKit services to decide whether to report "Not Responding".
    bool isHealthy() const;

    /// Returns milliseconds since the last valid CN105 response, or UINT32_MAX if
    /// no response has ever been received.
    uint32_t getLastResponseAge() const;

    /// Get the current state (read-only — actual heat pump values)
    const CN105State& getState() const { return _state; }

    /// Get effective state — substitutes wanted values during grace window.
    /// Use this instead of getState() for UI/HomeKit sync to prevent flicker.
    CN105State getEffectiveState() const;

    /// Get wanted settings (for grace window substitution)
    const WantedSettings& getWanted() const { return _wanted; }

    /// Check if a field is in grace window (wanted value should be used)
    /// Returns true if the field has a pending/recently-sent wanted value
    bool isFieldInGrace(bool hasField) const;

    /// ── Commands ────────────────────────────────────────────────────────────
    void setPower(bool on);
    void setMode(uint8_t mode);
    void setTargetTemp(float tempC);
    void setFanSpeed(uint8_t speed);
    void setVane(uint8_t position);
    void setWideVane(uint8_t position);

    /// Send all pending changes in a single set packet
    void sendPendingChanges();

    static uint8_t calcChecksum(const uint8_t *pkt, uint8_t len);
    static void    buildHeader(uint8_t *buf, uint8_t pktType, uint8_t dataLen);

    /// Runtime-configurable update interval (poll period)
    void setUpdateInterval(uint32_t ms) { _updateInterval = ms; }
    uint32_t getUpdateInterval() const { return _updateInterval; }

private:
    UartInterface* _uart = nullptr;
#ifndef UNIT_TEST
    HardwareUart* _hwUart = nullptr;  // Owned, created by begin(uart_port_t,...)
#endif
    CN105State      _state;

    // ── Connection state ────────────────────────────────────────────────────
    bool     _initialConnectDone = false;
    uint8_t  _connectRetries     = 0;
    uint32_t _lastConnectAttempt = 0;

    // ── Communication health tracking ────────────────────────────────────────
    // Timestamp (millis()) of the last valid CN105 response packet received.
    // Used by isHealthy() and the communication-loss detector in loop().
    uint32_t _lastSuccessfulResponse = 0;

    // ── Cycle-based polling state (matches MitsubishiCN105ESPHome approach) ─
    // A "cycle" sends all info requests (0x02, 0x03, 0x06) sequentially,
    // waiting for each response before sending the next request.
    uint32_t _lastCycleEnd    = 0;   // millis() when last cycle completed
    uint32_t _cycleStartMs    = 0;   // millis() when current cycle started
    bool     _cycleRunning    = false;
    uint8_t  _pollPhase       = 0;   // index into pollTypes[] within a cycle
    bool     _awaitingResponse = false; // waiting for response to current request

    // ── Pending set command ─────────────────────────────────────────────────
    uint8_t  _setFlags1       = 0;    // SET pkt[6]: power/mode/temp/fan/vane
    uint8_t  _setFlags2       = 0;    // SET pkt[7]: wide vane
    bool     _pendingPower    = false;
    uint8_t  _pendingMode     = 0;
    float    _pendingTemp     = 0;
    uint8_t  _pendingFan      = 0;
    uint8_t  _pendingVane     = 0;
    uint8_t  _pendingWideVane = 0;

    // ── Wanted settings (anti-flicker) ───────────────────────────────────────
    WantedSettings _wanted;

    // ── Runtime-configurable timing ─────────────────────────────────────────
    uint32_t _updateInterval = CN105_UPDATE_INTERVAL;

    // ── Temperature encoding mode ────────────────────────────────────────
    bool _tempMode = false;  // true = unit supports enhanced temp byte (data[11])

    // ── Error code polling (0x04) soft timeout ───────────────────────────
    uint8_t  _errorPollFailures = 0;   // consecutive failures
    bool     _errorPollDisabled = false; // true after 3 failures

    // ── RX buffer ───────────────────────────────────────────────────────────
    uint8_t  _rxBuf[32];
    uint8_t  _rxLen           = 0;
    uint32_t _rxLastByte      = 0;

    // ── Internal methods ────────────────────────────────────────────────────
    void sendConnectPacket();
    void sendInfoRequest(uint8_t infoType);
    void sendSetPacket();
    void processPacket(const uint8_t *pkt, uint8_t len);
    void handleInfoResponse(const uint8_t *data, uint8_t dataLen);
    void readSerial();
};
