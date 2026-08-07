#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include "esp_utils.h"
#include <driver/uart.h>
#include <driver/gpio.h>
#include <freertos/task.h>
#include "logging.h"
#include "uart_interface.h"
#ifndef UNIT_TEST
#include "hardware_uart.h"
#endif

// ── CN105 Packet Constants ──────────────────────────────────────────────────
constexpr uint8_t CN105_SYNC           = 0xFC;
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

// ── Sub Mode Values (from 0x09 data[3]) ─────────────────────────────────
constexpr uint8_t CN105_SUB_NORMAL      = 0x00;
constexpr uint8_t CN105_SUB_DEFROST     = 0x02;
constexpr uint8_t CN105_SUB_PREHEAT     = 0x04;
constexpr uint8_t CN105_SUB_STANDBY     = 0x08;

// ── Auto Sub Mode Values (from 0x09 data[5]; meaningful in AUTO mode) ────
constexpr uint8_t CN105_AUTOSUB_OFF     = 0x00;
constexpr uint8_t CN105_AUTOSUB_COOL    = 0x01;
constexpr uint8_t CN105_AUTOSUB_HEAT    = 0x02;
constexpr uint8_t CN105_AUTOSUB_LEADER  = 0x03;

// ── Temperature Encoding ────────────────────────────────────────────────────
constexpr float CN105_TEMP_MIN         = 16.0f;
// 30.5 (not 31.0) so the settable °C range matches the °F range exactly:
// 30.5 °C == 88 °F, the top of the F-table (sl2_proto.h). Keeps the Dial,
// web sliders and HomeKit constraints symmetric across units. The CN105 wire
// encoding still uses a fixed 31 °C origin (cn105_protocol.cpp) — that is the
// protocol datum, not the settable ceiling, and is intentionally left at 31.
constexpr float CN105_TEMP_MAX         = 30.5f;

// ── Timing ──────────────────────────────────────────────────────────────────
// Follows MitsubishiCN105ESPHome reference project timing
constexpr uint32_t CN105_BAUD_RATE         = 2400;
constexpr uint32_t CN105_RESPONSE_TIMEOUT  = 1000;   // ms
constexpr uint32_t CN105_UPDATE_INTERVAL   = 2000;    // ms — matches reference default (2s)
constexpr uint32_t CN105_CONNECT_INTERVAL  = 3000;    // ms between connect retries
constexpr uint32_t CN105_DEFER_DELAY       = 750;     // ms — defer next cycle after set command
constexpr uint8_t  CN105_MAX_CONNECT_RETRIES = 5;

// ── Communication Health ─────────────────────────────────────────────────────
// Floor for the communication-loss timeout. The effective timeout is
// commsTimeoutMs() = max(this, 6 × runtime poll interval); past it the device
// is considered disconnected and HomeKit reports "Not Responding".
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
    uint8_t  subMode     = 0;         // CN105_SUB_* (NORMAL/DEFROST/PREHEAT/STANDBY)
    uint8_t  stage       = 0;         // 0x00=IDLE..0x06=DIFFUSE (actual indoor fan activity)
    uint8_t  autoSubMode = 0;         // CN105_AUTOSUB_* (OFF/COOL/HEAT/LEADER, Auto only)
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
    uint32_t lastChange = 0;    // uptime_ms() of most recent user command
};

// ── CN105 Controller Class ──────────────────────────────────────────────────
// Threading contract: the CN105 task (startTask) owns the UART and all
// protocol state. Other tasks (HomeKit callbacks, web server, main loop) may
// call the const getters and the command methods below — commands only stage
// intent under a spinlock; the actual SET packet is transmitted by the CN105
// task from loop(), between poll cycles. This keeps a single UART writer and
// guarantees a SET can never land mid-poll-cycle (the heat pump silently
// drops those).
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

    /// Get the current state (actual heat pump values, no grace-window
    /// masking). Locked snapshot; safe to call from any task. Use
    /// getEffectiveState() for anything user-facing.
    CN105State getState() const;

    /// Get effective state — substitutes wanted values during grace window.
    /// Use this instead of getState() for UI/HomeKit sync to prevent flicker.
    /// Safe to call from any task.
    CN105State getEffectiveState() const;

    /// ── Commands (safe to call from any task) ───────────────────────────────
    /// Each setX() stages one field of a batch; sendPendingChanges() commits
    /// the batch for transmission by the CN105 task. Nothing is sent until
    /// sendPendingChanges() is called, so a multi-field batch (e.g. power +
    /// mode from a HomeKit write) always goes out as a single SET packet.
    void setPower(bool on);
    void setMode(uint8_t mode);
    void setTargetTemp(float tempC);
    void setFanSpeed(uint8_t speed);
    void setVane(uint8_t position);
    void setWideVane(uint8_t position);

    /// Commit the staged batch. The CN105 task transmits it from loop() as
    /// soon as no poll cycle is active (typically within ~100 ms).
    void sendPendingChanges();

    /// Feed an external room temperature to the heat pump (0x07 packet),
    /// overriding its internal thermistor. Clamped to the protocol's encodable
    /// range (see sendRemoteTempPacket). NAN is rejected.
    /// Uses deferred-send pattern — actual UART write happens between poll cycles.
    void sendRemoteTemperature(float tempC);

    /// Hand temperature control back to the heat pump's internal thermistor
    /// (0x07 packet with the disable marker). Same deferred-send pattern.
    void clearRemoteTemperature();

    /// The 0.5°C wire grid + clamp applied to remote temperatures before
    /// encoding. Public so feeders can compare what the HP will actually see
    /// (e.g. the BLE keepalive's change detection).
    static float quantizeRemoteTemp(float tempC);

    /// The clamp + rounding the SET path applies to a target temperature —
    /// i.e. the setpoint the heat pump will report back after accepting it.
    /// Public for the same reason as quantizeRemoteTemp(): callers that
    /// compare a desired setpoint against the reported one (the HomeKit AUTO
    /// logic) use this instead of hard-coding encoding knowledge. Depends on
    /// the detected tempMode (0.5°C grid vs legacy whole degrees); the latch
    /// is a single bool, safe to read from any task.
    float quantizeSetpoint(float tempC) const;

    static uint8_t calcChecksum(const uint8_t *pkt, uint8_t len);
    static void    buildHeader(uint8_t *buf, uint8_t pktType, uint8_t dataLen);

    /// Runtime-configurable update interval (poll period)
    void setUpdateInterval(uint32_t ms) { _updateInterval = ms; }

    /// Communication-loss timeout: 6 × the runtime poll interval, floored at
    /// the compile-time default. The poll interval is user-settable up to
    /// 30 s; a fixed 12 s timeout would falsely declare the link dead there.
    uint32_t commsTimeoutMs() const {
        uint32_t t = _updateInterval * 6;
        return t > CN105_COMMS_TIMEOUT ? t : CN105_COMMS_TIMEOUT;
    }

#ifndef UNIT_TEST
    /// Start a dedicated FreeRTOS task for UART I/O and protocol management.
    /// After calling this, do not call loop() from other tasks.
    void startTask(int priority = 5, int stackSize = 4096);
#endif

private:
    UartInterface* _uart = nullptr;
#ifndef UNIT_TEST
    HardwareUart* _hwUart = nullptr;  // Owned, created by begin(uart_port_t,...)
    TaskHandle_t _taskHandle = nullptr;
    static void taskFunc(void *arg);
#endif
    CN105State      _state;

    // ── Connection state ────────────────────────────────────────────────────
    uint8_t  _connectRetries     = 0;
    uint32_t _lastConnectAttempt = 0;

    // ── Communication health tracking ────────────────────────────────────────
    // Timestamp (uptime_ms()) of the last valid CN105 response packet received.
    // Used by isHealthy() and the communication-loss detector in loop().
    uint32_t _lastSuccessfulResponse = 0;

    // ── Cycle-based polling state (matches MitsubishiCN105ESPHome approach) ─
    // A "cycle" sends all info requests (0x02, 0x03, 0x06) sequentially,
    // waiting for each response before sending the next request.
    uint32_t _lastCycleEnd    = 0;   // uptime_ms() when last cycle completed
    uint32_t _cycleStartMs    = 0;   // uptime_ms() when current cycle started
    bool     _cycleRunning    = false;
    uint8_t  _pollPhase       = 0;   // index into pollTypes[] within a cycle
    bool     _awaitingResponse = false; // waiting for response to current request

    // ── Staged set command (cross-task mailbox) ─────────────────────────────
    // Producers (any task) stage fields via setX() and commit with
    // sendPendingChanges(); the CN105 task snapshots-and-clears in loop() and
    // transmits from the snapshot. All access under _mux.
    struct PendingCommand {
        uint8_t flags1   = 0;    // SET pkt[6]: power/mode/temp/fan/vane
        uint8_t flags2   = 0;    // SET pkt[7]: wide vane
        bool    power    = false;
        uint8_t mode     = 0;
        float   temp     = 0;
        uint8_t fan      = 0;
        uint8_t vane     = 0;
        uint8_t wideVane = 0;
    };
    PendingCommand _staged;
    bool           _sendRequested = false;  // set by sendPendingChanges(), cleared by loop()
    // Watchdog for a producer that stages fields but never commits them
    // (a caller bug — see the contract above). CN105 task only, no lock.
    uint32_t       _stagedOrphanSince = 0;  // 0 = nothing orphaned

    // ── Wanted settings (anti-flicker) ───────────────────────────────────────
    WantedSettings _wanted;

    // One rule for all cross-task protocol state: _staged/_sendRequested,
    // _wanted, _pendingRemoteTemp*, _state, and _lastSuccessfulResponse are
    // only touched under this lock — writers (the CN105 task) and readers
    // alike. Held only for short copies/flag flips — never across UART I/O
    // or logging.
    mutable portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

    // ── Runtime-configurable timing ─────────────────────────────────────────
    uint32_t _updateInterval = CN105_UPDATE_INTERVAL;

    // ── Temperature encoding mode ────────────────────────────────────────
    bool _tempMode = false;  // true = unit supports enhanced temp byte (data[11])

    // ── Error code polling (0x04) soft timeout ───────────────────────────
    uint8_t  _errorPollFailures = 0;   // consecutive failures
    bool     _errorPollDisabled = false; // true after 3 failures

    // ── Pending remote temperature (NAN = clear/revert to internal) ─────
    bool     _pendingRemoteTemp  = false;
    float    _pendingRemoteTempC = 0.0f;   // only read while _pendingRemoteTemp is set

    // ── RX buffer ───────────────────────────────────────────────────────────
    uint8_t  _rxBuf[32];
    uint8_t  _rxLen           = 0;
    uint32_t _rxLastByte      = 0;

    // ── Internal methods ────────────────────────────────────────────────────
    void sendConnectPacket();
    void sendInfoRequest(uint8_t infoType);
    void sendSetPacket(const PendingCommand &cmd);
    void processPacket(const uint8_t *pkt, uint8_t len);
    void handleInfoResponse(const uint8_t *data, uint8_t dataLen);
    void readSerial();
    void sendRemoteTempPacket(float tempC);
};
