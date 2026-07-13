#include "cn105_protocol.h"
#include "cn105_strings.h"
#include <algorithm>
#include <cmath>
#include <esp_task_wdt.h>

static const char *TAG = "cn105";

// ── Connect packet payload (fixed) ─────────────────────────────────────────
static const uint8_t CONNECT_PKT[] = {
    0xFC, 0x5A, 0x01, 0x30, 0x02, 0xCA, 0x01, 0xA8
};

// ── Debug logging helper ────────────────────────────────────────────────────
static void logHex(char *out, size_t outLen, const uint8_t *buf, uint8_t len) {
    size_t pos = 0;
    for (uint8_t i = 0; i < len && pos + 3 < outLen; i++) {
        pos += snprintf(out + pos, outLen - pos, "%02X ", buf[i]);
    }
    if (pos > 0 && out[pos-1] == ' ') out[pos-1] = '\0';
}

// ── Poll phase ordering (single definition, used by loop + processPacket) ──
static const uint8_t POLL_TYPES[CN105_POLL_PHASE_COUNT] = {
    CN105_INFO_SETTINGS, CN105_INFO_ROOMTEMP, CN105_INFO_ERRORCODE, CN105_INFO_STATUS, CN105_INFO_STANDBY
};

// ════════════════════════════════════════════════════════════════════════════
// Construction / Init
// ════════════════════════════════════════════════════════════════════════════

CN105Controller::CN105Controller() {}

void CN105Controller::begin(uart_port_t uartNum, int rxPin, int txPin) {
#ifndef UNIT_TEST
    _hwUart = new HardwareUart(uartNum, rxPin, txPin, CN105_BAUD_RATE);
    begin(_hwUart);
#endif
}

void CN105Controller::begin(UartInterface *uart) {
    _uart = uart;
    _uart->flush();
    _state.connected = false;
    _initialConnectDone = false;
    _connectRetries = 0;
    _lastSuccessfulResponse = 0;
    LOG_INFO("Controller initialized, waiting for connection...");
}

#ifndef UNIT_TEST
void CN105Controller::startTask(int priority, int stackSize) {
    if (_taskHandle) return;
    xTaskCreate(taskFunc, "cn105", stackSize, this, priority, &_taskHandle);
    LOG_INFO("Started dedicated UART task (priority=%d, stack=%d)", priority, stackSize);
}

void CN105Controller::taskFunc(void *arg) {
    esp_task_wdt_add(NULL);
    auto *self = static_cast<CN105Controller*>(arg);
    while (true) {
        esp_task_wdt_reset();
        self->loop();
        // Block until UART data arrives or 100ms timeout for timer management
        if (self->_uart) {
            self->_uart->waitForData(100);
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
#endif

bool CN105Controller::isHealthy() const {
    taskENTER_CRITICAL(&_mux);
    bool connected = _state.connected;
    uint32_t last = _lastSuccessfulResponse;
    taskEXIT_CRITICAL(&_mux);
    return connected && last > 0 && (uptime_ms() - last) < CN105_COMMS_TIMEOUT;
}

uint32_t CN105Controller::getLastResponseAge() const {
    taskENTER_CRITICAL(&_mux);
    uint32_t last = _lastSuccessfulResponse;
    taskEXIT_CRITICAL(&_mux);
    if (last == 0) return UINT32_MAX;
    return uptime_ms() - last;
}

CN105State CN105Controller::getState() const {
    taskENTER_CRITICAL(&_mux);
    CN105State s = _state;
    taskEXIT_CRITICAL(&_mux);
    return s;
}

CN105State CN105Controller::getEffectiveState() const {
    taskENTER_CRITICAL(&_mux);
    CN105State eff = _state;
    const WantedSettings w = _wanted;
    taskEXIT_CRITICAL(&_mux);
    // Wanted fields mask the actual heat pump values while in the grace
    // window (echavet pattern: suppress until the heat pump confirms). The
    // safety timeout prevents a stuck state if confirmation never arrives
    // (e.g. the heat pump rejects the value or the SET was lost).
    constexpr uint32_t GRACE_SAFETY_TIMEOUT = 10000;  // 10s max
    bool inGrace = (uptime_ms() - w.lastChange) < GRACE_SAFETY_TIMEOUT;
    if (inGrace) {
        if (w.hasPower)    eff.power      = w.power;
        if (w.hasMode)     eff.mode       = w.mode;
        if (w.hasTemp)     eff.targetTemp = w.targetTemp;
        if (w.hasFan)      eff.fanSpeed   = w.fanSpeed;
        if (w.hasVane)     eff.vane       = w.vane;
        if (w.hasWideVane) eff.wideVane   = w.wideVane;
    }
    return eff;
}

// ════════════════════════════════════════════════════════════════════════════
// Main Loop
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::loop() {
    uint32_t now = uptime_ms();

    // ── Read any incoming bytes ─────────────────────────────────────────────
    readSerial();

    // ── Connection phase ────────────────────────────────────────────────────
    if (!_state.connected) {
        if (now - _lastConnectAttempt >= CN105_CONNECT_INTERVAL) {
            if (_connectRetries < CN105_MAX_CONNECT_RETRIES) {
                LOG_INFO("Sending connect packet (attempt %d/%d)",
                         _connectRetries + 1, CN105_MAX_CONNECT_RETRIES);
                sendConnectPacket();
                _lastConnectAttempt = now;
                _connectRetries++;
            } else {
                if (now - _lastConnectAttempt >= CN105_CONNECT_INTERVAL * 3) {
                    LOG_WARN("Max retries reached, resetting connect counter");
                    _connectRetries = 0;
                }
            }
        }
        return;
    }

    // ── Send committed changes (priority over polling, only outside a cycle) ──
    // Snapshot-and-clear under the lock, then transmit from the snapshot so a
    // producer staging a new batch mid-TX can neither tear this packet nor
    // lose its own (its flags accumulate in the freshly cleared _staged).
    if (_sendRequested && !_cycleRunning) {
        taskENTER_CRITICAL(&_mux);
        const PendingCommand cmd = _staged;
        _staged = PendingCommand{};
        _sendRequested = false;
        _wanted.hasBeenSent = true;
        taskEXIT_CRITICAL(&_mux);
        LOG_INFO("Sending pending set command (flags=0x%02X flags2=0x%02X)", cmd.flags1, cmd.flags2);
        sendSetPacket(cmd);
        _lastCycleEnd = now + CN105_DEFER_DELAY;
        return;
    }

    // ── Send pending remote temperature (same deferred pattern) ──────────
    if (_pendingRemoteTemp && !_cycleRunning) {
        taskENTER_CRITICAL(&_mux);
        const float remoteTempC = _pendingRemoteTempC;
        _pendingRemoteTemp = false;
        taskEXIT_CRITICAL(&_mux);
        sendRemoteTempPacket(remoteTempC);
        _lastCycleEnd = now + CN105_DEFER_DELAY;
        return;
    }

    // ── Cycle-based polling ─────────────────────────────────────────────────
    if (_cycleRunning) {
        if (now - _cycleStartMs > (2 * _updateInterval) + 1000) {
            LOG_WARN("Poll cycle TIMEOUT after %lums (phase %d/%d)",
                     (unsigned long)(now - _cycleStartMs), _pollPhase, CN105_POLL_PHASE_COUNT);
            _cycleRunning = false;
            _awaitingResponse = false;
            _lastCycleEnd = now;
            // Track 0x04 failures for soft disable
            if (_pollPhase < CN105_POLL_PHASE_COUNT &&
                POLL_TYPES[_pollPhase] == CN105_INFO_ERRORCODE) {
                _errorPollFailures++;
                if (_errorPollFailures >= 3) {
                    LOG_WARN("Disabling 0x04 error code polling (3 consecutive timeouts)");
                    _errorPollDisabled = true;
                }
            }
        }
    } else {
        if (now >= _lastCycleEnd &&
            (now - _lastCycleEnd) >= _updateInterval) {
            LOG_DEBUG("Starting new poll cycle");
            _cycleRunning = true;
            _cycleStartMs = now;
            _pollPhase = 0;
            _awaitingResponse = false;
            // Skip disabled error code phase
            if (POLL_TYPES[_pollPhase] == CN105_INFO_ERRORCODE && _errorPollDisabled) {
                _pollPhase++;
            }
            sendInfoRequest(POLL_TYPES[_pollPhase]);
            _awaitingResponse = true;
        }
    }

    // ── Detect communication loss ───────────────────────────────────────────
    // Re-read uptime_ms() because readSerial() may have updated _lastSuccessfulResponse
    uint32_t nowMs = uptime_ms();
    if (_lastSuccessfulResponse > 0 &&
        (nowMs - _lastSuccessfulResponse) > CN105_COMMS_TIMEOUT) {
        LOG_ERROR("COMMUNICATION LOST! No response for %lums (timeout=%dms)",
                  (unsigned long)(nowMs - _lastSuccessfulResponse), CN105_COMMS_TIMEOUT);
        taskENTER_CRITICAL(&_mux);
        _state.connected = false;
        taskEXIT_CRITICAL(&_mux);
        _initialConnectDone = false;
        _connectRetries = 0;
        _cycleRunning = false;
        _awaitingResponse = false;
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Commands
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::setPower(bool on) {
    LOG_INFO("CMD: setPower(%s)", on ? "ON" : "OFF");
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_POWER;
    _staged.power = on;
    _wanted.hasPower = true;
    _wanted.power = on;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setMode(uint8_t mode) {
    LOG_INFO("CMD: setMode(%s / 0x%02X)", modeToLogStr(mode), mode);
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_MODE;
    _staged.mode = mode;
    _wanted.hasMode = true;
    _wanted.mode = mode;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setTargetTemp(float tempC) {
    float clamped = std::clamp(tempC, CN105_TEMP_MIN, CN105_TEMP_MAX);
    LOG_INFO("CMD: setTargetTemp(%.1f%sC)", clamped, "\xC2\xB0");
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_TEMP;
    _staged.temp = clamped;
    _wanted.hasTemp = true;
    _wanted.targetTemp = clamped;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setFanSpeed(uint8_t speed) {
    LOG_INFO("CMD: setFanSpeed(%s / 0x%02X)", fanToLogStr(speed), speed);
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_FAN;
    _staged.fan = speed;
    _wanted.hasFan = true;
    _wanted.fanSpeed = speed;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setVane(uint8_t position) {
    LOG_INFO("CMD: setVane(%s / 0x%02X)", vaneToLogStr(position), position);
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_VANE;
    _staged.vane = position;
    _wanted.hasVane = true;
    _wanted.vane = position;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setWideVane(uint8_t position) {
    LOG_INFO("CMD: setWideVane(%s / 0x%02X)", wideVaneToLogStr(position), position);
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags2 |= CN105_FLAG2_WVANE;
    _staged.wideVane = position;
    _wanted.hasWideVane = true;
    _wanted.wideVane = position;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::sendPendingChanges() {
    // Commit only — the CN105 task transmits from loop() once outside a poll
    // cycle (see the threading contract in cn105_protocol.h). Transmitting
    // here, on the caller's task, would race the poll cycle: the heat pump
    // silently drops SETs that arrive mid-cycle.
    taskENTER_CRITICAL(&_mux);
    bool hasPending = (_staged.flags1 != 0 || _staged.flags2 != 0);
    if (hasPending) _sendRequested = true;
    taskEXIT_CRITICAL(&_mux);
    if (hasPending) {
        LOG_DEBUG("Set command committed, CN105 task will transmit");
    }
}

void CN105Controller::sendRemoteTemperature(float tempC) {
    if (std::isnan(tempC)) {
        LOG_WARN("sendRemoteTemperature(NAN) ignored — use clearRemoteTemperature()");
        return;
    }
    taskENTER_CRITICAL(&_mux);
    _pendingRemoteTemp = true;
    _pendingRemoteTempC = tempC;
    taskEXIT_CRITICAL(&_mux);
    LOG_DEBUG("CMD: sendRemoteTemperature(%.1f%sC)", tempC, "\xC2\xB0");
}

void CN105Controller::clearRemoteTemperature() {
    taskENTER_CRITICAL(&_mux);
    _pendingRemoteTemp = true;
    _pendingRemoteTempC = NAN;
    taskEXIT_CRITICAL(&_mux);
    LOG_DEBUG("CMD: clearRemoteTemperature()");
}

float CN105Controller::quantizeRemoteTemp(float tempC) {
    // Round to the protocol's 0.5 C grid, then clamp to what the enhanced byte
    // can encode: 0.0 C would produce 0x80, which doubles as the disable
    // marker, so the floor is 0.5 C. An indoor unit only needs to know "colder
    // than range" — the exact sub-zero value adds nothing.
    return std::clamp(roundf(tempC * 2.0f) / 2.0f, 0.5f, 63.5f);
}

float CN105Controller::quantizeSetpoint(float tempC) const {
    float clamped = std::clamp(tempC, CN105_TEMP_MIN, CN105_TEMP_MAX);
    float rounded = roundf(clamped * 2.0f) / 2.0f;
    // Legacy integer encoding drops the half degree (see sendSetPacket)
    return _tempMode ? rounded : (float)(int)rounded;
}

void CN105Controller::sendRemoteTempPacket(float tempC) {
    uint8_t pkt[22];
    memset(pkt, 0, sizeof(pkt));
    buildHeader(pkt, CN105_PKT_SET, CN105_DATA_LEN);

    // Verified against SwiCago/HeatPump setRemoteTemperature():
    // pkt[0-4] = header (FC 41 01 30 10)
    // pkt[5]   = 0x07 = remote temp command
    // pkt[6]   = 0x01 enable / 0x00 disable
    // pkt[7]   = legacy encoding: 3 + ((temp - 10) * 2)
    // pkt[8]   = enhanced encoding: temp * 2 + 128 (or 0x80 when disabled)
    pkt[5] = 0x07;

    if (!std::isnan(tempC)) {
        float rounded = quantizeRemoteTemp(tempC);
        pkt[6] = 0x01;
        // The legacy byte underflows below 8.5 C; clamp so legacy-tempMode
        // units read 8.5 C instead of a wrapped garbage value.
        pkt[7] = (uint8_t)std::max(3.0f + (rounded - 10.0f) * 2.0f, 0.0f);
        pkt[8] = (uint8_t)(rounded * 2.0f + 128.0f);
        LOG_INFO("Remote temp SET (%.1f%sC)", rounded, "\xC2\xB0");
    } else {
        pkt[6] = 0x00;
        pkt[8] = 0x80;
        LOG_INFO("Remote temp CLEARED — internal thermistor active");
    }

    pkt[21] = calcChecksum(pkt, 21);
    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        char hex[128]; logHex(hex, sizeof(hex), pkt, 22);
        LOG_DEBUG("TX REMOTE_TEMP (%d bytes): %s", 22, hex);
    }
    _uart->write(pkt, 22);
}

// ════════════════════════════════════════════════════════════════════════════
// Packet Building & Sending
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::buildHeader(uint8_t *buf, uint8_t pktType, uint8_t dataLen) {
    buf[0] = CN105_SYNC;
    buf[1] = pktType;
    buf[2] = CN105_HEADER_BYTE2;
    buf[3] = CN105_HEADER_BYTE3;
    buf[4] = dataLen;
}

uint8_t CN105Controller::calcChecksum(const uint8_t *pkt, uint8_t len) {
    uint8_t sum = 0;
    for (uint8_t i = 0; i < len; i++) {
        sum += pkt[i];
    }
    return (0xFC - sum) & 0xFF;
}

void CN105Controller::sendConnectPacket() {
    _uart->flush();
    _rxLen = 0;

    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        char hex[64]; logHex(hex, sizeof(hex), CONNECT_PKT, sizeof(CONNECT_PKT));
        LOG_DEBUG("TX CONNECT (%d bytes): %s", (int)sizeof(CONNECT_PKT), hex);
    }
    _uart->write(CONNECT_PKT, sizeof(CONNECT_PKT));
}

void CN105Controller::sendInfoRequest(uint8_t infoType) {
    uint8_t pkt[22];
    memset(pkt, 0, sizeof(pkt));
    buildHeader(pkt, CN105_PKT_INFO_REQ, CN105_DATA_LEN);
    pkt[5] = infoType;
    pkt[21] = calcChecksum(pkt, 21);

    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        const char *typeStr = "UNKNOWN";
        if (infoType == CN105_INFO_SETTINGS) typeStr = "SETTINGS";
        else if (infoType == CN105_INFO_ROOMTEMP) typeStr = "ROOMTEMP";
        else if (infoType == CN105_INFO_STATUS) typeStr = "STATUS";
        else if (infoType == CN105_INFO_STANDBY) typeStr = "STANDBY";
        else if (infoType == CN105_INFO_ERRORCODE) typeStr = "ERRORCODE";
        char hex[128]; logHex(hex, sizeof(hex), pkt, 22);
        LOG_DEBUG("TX INFO_REQ type=%s phase=%d/%d (%d bytes): %s",
                  typeStr, _pollPhase + 1, CN105_POLL_PHASE_COUNT, 22, hex);
    }
    _uart->write(pkt, 22);
}

void CN105Controller::sendSetPacket(const PendingCommand &cmd) {
    uint8_t pkt[22];
    memset(pkt, 0, sizeof(pkt));
    buildHeader(pkt, CN105_PKT_SET, CN105_DATA_LEN);

    pkt[5] = 0x01;
    pkt[6] = cmd.flags1;

    if (cmd.flags1 & CN105_FLAG_POWER) {
        pkt[8] = cmd.power ? CN105_POWER_ON : CN105_POWER_OFF;
        LOG_INFO("SET power=%s", cmd.power ? "ON" : "OFF");
    }

    if (cmd.flags1 & CN105_FLAG_MODE) {
        pkt[9] = cmd.mode;
        LOG_INFO("SET mode=%s (0x%02X)", modeToLogStr(cmd.mode), cmd.mode);
    }

    if (cmd.flags1 & CN105_FLAG_TEMP) {
        // quantizeSetpoint is the single source of truth for what the unit
        // will end up at (and report back) — keep the encoding below in step
        // with it.
        float q = quantizeSetpoint(cmd.temp);
        if (_tempMode) {
            // Enhanced mode: byte 19 carries 0.5C precision, byte 10 = 0
            pkt[10] = 0x00;
            pkt[19] = (uint8_t)((q * 2.0f) + 128.0f);
        } else {
            // Legacy mode: byte 10 carries integer temp, byte 19 = 0
            pkt[10] = (uint8_t)(31 - (int)q);
        }
        LOG_INFO("SET temp=%.1f%sC (tempMode=%s)", q, "\xC2\xB0", _tempMode ? "enhanced" : "legacy");
    }

    if (cmd.flags1 & CN105_FLAG_FAN) {
        pkt[11] = cmd.fan;
        LOG_INFO("SET fan=%s (0x%02X)", fanToLogStr(cmd.fan), cmd.fan);
    }

    if (cmd.flags1 & CN105_FLAG_VANE) {
        pkt[12] = cmd.vane;
        LOG_INFO("SET vane=%s (0x%02X)", vaneToLogStr(cmd.vane), cmd.vane);
    }

    // Wide vane (horizontal) — uses second control flag byte (packet[7])
    pkt[7] = cmd.flags2;
    if (cmd.flags2 & CN105_FLAG2_WVANE) {
        pkt[18] = cmd.wideVane;
        LOG_INFO("SET wideVane=%s (0x%02X)", wideVaneToLogStr(cmd.wideVane), cmd.wideVane);
    }

    pkt[21] = calcChecksum(pkt, 21);
    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        char hex[128]; logHex(hex, sizeof(hex), pkt, 22);
        LOG_DEBUG("TX SET (%d bytes): %s", 22, hex);
    }
    _uart->write(pkt, 22);
}

// ════════════════════════════════════════════════════════════════════════════
// Packet Reception & Parsing
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::readSerial() {
    uint32_t now = uptime_ms();

    // Non-blocking bulk read via UART interface
    size_t available = _uart->available();

    if (available > 0) {
        uint8_t tmpBuf[128];
        int toRead = (available > sizeof(tmpBuf)) ? sizeof(tmpBuf) : available;
        int bytesRead = _uart->read(tmpBuf, toRead);

        if (bytesRead > 0) {
            _rxLastByte = now;

            for (int i = 0; i < bytesRead; i++) {
                uint8_t b = tmpBuf[i];

                // Sync on header byte
                if (_rxLen == 0 && b != CN105_SYNC) {
                    continue;
                }

                _rxBuf[_rxLen++] = b;

                // Validate the fixed header bytes as they arrive. Positions 2
                // and 3 are always 0x01 0x30;
                // rejecting here stops a noise-triggered false sync on 0xFC from
                // being mistaken for a packet. Position 1 (packet type) varies.
                if (_rxLen == 3 && _rxBuf[2] != CN105_HEADER_BYTE2) {
                    _rxLen = 0;
                    continue;
                }
                if (_rxLen == 4 && _rxBuf[3] != CN105_HEADER_BYTE3) {
                    _rxLen = 0;
                    continue;
                }

                // Once the length byte (position 4) is in, bound-check it before
                // trusting it. Computed in int to avoid the uint8_t wraparound a
                // length byte >= 250 would otherwise cause (5 + 250 + 1 == 256
                // -> 0), which led to out-of-bounds reads in calcChecksum. Frames
                // larger than the buffer are rejected up front.
                if (_rxLen >= 5) {
                    int expectedLen = 5 + (int)_rxBuf[4] + 1;  // header + data + checksum
                    if (expectedLen > (int)sizeof(_rxBuf)) {
                        LOG_WARN("RX invalid length 0x%02X (frame=%d > buf=%d), resetting",
                                 _rxBuf[4], expectedLen, (int)sizeof(_rxBuf));
                        _rxLen = 0;
                        continue;
                    }
                    if (_rxLen >= expectedLen) {
                        uint8_t chk = calcChecksum(_rxBuf, expectedLen - 1);
                        if (chk == _rxBuf[expectedLen - 1]) {
                            if (currentLogLevel >= LOG_LEVEL_DEBUG) {
                                char hex[128]; logHex(hex, sizeof(hex), _rxBuf, expectedLen);
                                LOG_DEBUG("RX VALID (%d bytes): %s", expectedLen, hex);
                            }
                            processPacket(_rxBuf, (uint8_t)expectedLen);
                        } else {
                            LOG_ERROR("RX CHECKSUM FAIL: expected=0x%02X got=0x%02X",
                                      chk, _rxBuf[expectedLen - 1]);
                        }
                        _rxLen = 0;
                    }
                }

                if (_rxLen >= sizeof(_rxBuf)) {
                    LOG_ERROR("RX buffer overflow, resetting");
                    _rxLen = 0;
                }
            }
        }
    }

    // Reset RX buffer on timeout (incomplete packet)
    if (_rxLen > 0 && now - _rxLastByte > CN105_RESPONSE_TIMEOUT) {
        LOG_WARN("RX timeout (%d bytes incomplete), resetting buffer", _rxLen);
        _rxLen = 0;
    }
}

void CN105Controller::processPacket(const uint8_t *pkt, uint8_t len) {
    uint8_t pktType = pkt[1];

    switch (pktType) {
        case CN105_PKT_CONNECT_OK:
            LOG_INFO("Connected to heat pump");
            taskENTER_CRITICAL(&_mux);
            _state.connected = true;
            _state.lastUpdate = uptime_ms();
            _lastSuccessfulResponse = _state.lastUpdate;
            taskEXIT_CRITICAL(&_mux);
            _initialConnectDone = true;
            _connectRetries = 0;
            break;

        case CN105_PKT_SET_ACK:
            LOG_INFO("SET command acknowledged");
            taskENTER_CRITICAL(&_mux);
            _state.lastUpdate = uptime_ms();
            _lastSuccessfulResponse = _state.lastUpdate;
            taskEXIT_CRITICAL(&_mux);
            break;

        case CN105_PKT_INFO_RESP: {
            // data[0] (== pkt[5]) echoes the info type the unit is responding to.
            uint8_t respType = (len >= 6) ? pkt[5] : 0xFF;

            if (len >= 7) {
                handleInfoResponse(&pkt[5], pkt[4]);
            }

            // Only advance the poll cycle when the response matches the request
            // we're actually waiting on. echavet's RequestScheduler matches
            // responses to requests by code; advancing on any INFO_RESP lets a
            // stale/duplicate/out-of-order packet desync the phase sequence. A
            // genuinely missing response is still handled by the cycle timeout
            // in loop() (which also drives the 0x04 soft-disable).
            bool awaitingExpected = _cycleRunning && _awaitingResponse &&
                                    _pollPhase < CN105_POLL_PHASE_COUNT &&
                                    respType == POLL_TYPES[_pollPhase];
            if (awaitingExpected) {
                _awaitingResponse = false;
                _pollPhase++;

                // Skip disabled error code phase
                if (_pollPhase < CN105_POLL_PHASE_COUNT &&
                    POLL_TYPES[_pollPhase] == CN105_INFO_ERRORCODE && _errorPollDisabled) {
                    _pollPhase++;
                }

                if (_pollPhase < CN105_POLL_PHASE_COUNT) {
                    sendInfoRequest(POLL_TYPES[_pollPhase]);
                    _awaitingResponse = true;
                } else {
                    LOG_DEBUG("Poll cycle complete (%lums)", (unsigned long)(uptime_ms() - _cycleStartMs));
                    _cycleRunning = false;
                    _lastCycleEnd = uptime_ms();
                }
            } else if (_cycleRunning && _awaitingResponse) {
                LOG_DEBUG("RX INFO_RESP type 0x%02X != expected 0x%02X (phase %d/%d), not advancing",
                          respType,
                          _pollPhase < CN105_POLL_PHASE_COUNT ? POLL_TYPES[_pollPhase] : 0xFF,
                          _pollPhase + 1, CN105_POLL_PHASE_COUNT);
            }
            break;
        }

        default:
            LOG_WARN("RX unknown packet type 0x%02X", pktType);
            break;
    }
}

void CN105Controller::handleInfoResponse(const uint8_t *data, uint8_t dataLen) {
    if (dataLen < 6) {
        LOG_WARN("INFO_RESP too short (dataLen=%d, need >=6)", dataLen);
        return;
    }

    uint32_t now = uptime_ms();

    // Decode into a local copy, then commit under _mux in one place at the
    // end — cross-task readers never observe a half-decoded response.
    // Reading _state without the lock here is fine: this task is its only
    // writer.
    CN105State next = _state;
    next.lastUpdate = now;
    bool settingsDecoded = false;

    switch (data[0]) {
        case CN105_INFO_SETTINGS:
            if (dataLen >= 8) {
                next.power = (data[3] != 0);
                uint8_t rawMode = data[4];
                if (rawMode > 0x08) {
                    LOG_DEBUG("SETTINGS: stripping iSee flag (0x%02X -> 0x%02X)",
                              rawMode, rawMode - 0x08);
                    rawMode -= 0x08;
                }
                next.mode = rawMode;
                next.fanSpeed = data[6];
                next.vane = data[7];

                // Wide vane (horizontal): lower nibble of data[10]
                if (dataLen >= 11) {
                    next.wideVane = data[10] & 0x0F;
                }

                if (dataLen >= 12 && data[11] != 0) {
                    next.targetTemp = ((float)data[11] - 128.0f) / 2.0f;
                    _tempMode = true;  // Unit supports enhanced temp encoding
                } else {
                    next.targetTemp = 31.0f - (float)data[5];
                }
                LOG_DEBUG("SETTINGS: power=%s mode=%s fan=%s vane=%s wvane=%s target=%.1f%sC",
                          next.power ? "ON" : "OFF", modeToLogStr(next.mode),
                          fanToLogStr(next.fanSpeed), vaneToLogStr(next.vane),
                          wideVaneToLogStr(next.wideVane), next.targetTemp, "\xC2\xB0");
                settingsDecoded = true;
            }
            break;

        case CN105_INFO_ROOMTEMP:
            if (dataLen >= 7 && data[6] != 0) {
                next.roomTemp = ((float)data[6] - 128.0f) / 2.0f;
            } else if (dataLen >= 4) {
                next.roomTemp = (float)data[3] + 10.0f;
            }
            // Outside air temperature: data[5], formula (val - 128) / 2.0
            // Valid when > 1; some units report 0x01 (-63.5C) when idle = invalid
            if (dataLen >= 6 && data[5] > 1) {
                next.outsideTemp = ((float)data[5] - 128.0f) / 2.0f;
                next.outsideTempValid = true;
            } else {
                next.outsideTempValid = false;
            }
            // Runtime hours: data[11:13] as 24-bit value, divided by 60
            if (dataLen >= 14 && (data[11] | data[12] | data[13]) != 0) {
                uint32_t rawMinutes = ((uint32_t)data[11] << 16) |
                                      ((uint32_t)data[12] << 8) |
                                       (uint32_t)data[13];
                next.runtimeHours = (float)rawMinutes / 60.0f;
                next.runtimeValid = true;
            }
            if (currentLogLevel >= LOG_LEVEL_DEBUG) {
                char outsideStr[8] = "N/A", runtimeStr[12] = "N/A";
                if (next.outsideTempValid) snprintf(outsideStr, sizeof(outsideStr), "%.1f", next.outsideTemp);
                if (next.runtimeValid) snprintf(runtimeStr, sizeof(runtimeStr), "%.1f", next.runtimeHours);
                LOG_DEBUG("ROOMTEMP: %.1f\xC2\xB0""C  outside=%s  runtime=%s",
                          next.roomTemp, outsideStr, runtimeStr);
            }
            break;

        case CN105_INFO_STATUS:
            if (dataLen >= 5) {
                next.compressorHz = data[3];
                next.operating = (data[4] != 0);
                LOG_DEBUG("STATUS: compressor=%dHz operating=%s",
                          next.compressorHz, next.operating ? "YES" : "NO");
            }
            break;

        case CN105_INFO_STANDBY:
            if (dataLen >= 6) {
                next.subMode = data[3];
                next.stage = data[4];
                next.autoSubMode = data[5];
                LOG_DEBUG("STANDBY: subMode=%s stage=%s autoSub=%s",
                          subModeToLogStr(next.subMode),
                          stageToLogStr(next.stage),
                          autoSubModeToLogStr(next.autoSubMode));
            }
            break;

        case CN105_INFO_ERRORCODE:
            if (dataLen >= 5) {
                next.errorCode = data[4];
                next.hasError = (data[4] != 0x80);
                _errorPollFailures = 0;  // Reset failure counter on success
                if (next.hasError) {
                    LOG_WARN("ERROR CODE: 0x%02X", data[4]);
                } else {
                    LOG_DEBUG("ERROR: normal (0x80)");
                }
            }
            break;

        default:
            LOG_WARN("INFO_RESP unknown type 0x%02X", data[0]);
            break;
    }

    taskENTER_CRITICAL(&_mux);
    _state = next;
    _lastSuccessfulResponse = now;
    // Clear wanted flags when the heat pump confirms matching values — only
    // meaningful on a SETTINGS response, and only after the command has been
    // sent (echavet pattern). Under the lock: a producer staging a new
    // command mid-check must not have its fresh has* flag cleared by a stale
    // match.
    if (settingsDecoded && _wanted.hasBeenSent) {
        if (_wanted.hasPower && next.power == _wanted.power)
            _wanted.hasPower = false;
        if (_wanted.hasMode && next.mode == _wanted.mode)
            _wanted.hasMode = false;
        if (_wanted.hasTemp && fabsf(next.targetTemp - _wanted.targetTemp) < 0.3f)
            _wanted.hasTemp = false;
        if (_wanted.hasFan && next.fanSpeed == _wanted.fanSpeed)
            _wanted.hasFan = false;
        if (_wanted.hasVane && next.vane == _wanted.vane)
            _wanted.hasVane = false;
        if (_wanted.hasWideVane && next.wideVane == _wanted.wideVane)
            _wanted.hasWideVane = false;
    }
    taskEXIT_CRITICAL(&_mux);
}
