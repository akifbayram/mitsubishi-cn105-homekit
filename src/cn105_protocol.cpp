#include "cn105_protocol.h"
#include "cn105_strings.h"
#include <cmath>

// ── Connect packet payload (fixed) ─────────────────────────────────────────
static const uint8_t CONNECT_PKT[] = {
    0xFC, 0x5A, 0x01, 0x30, 0x02, 0xCA, 0x01, 0xA8
};

// ── Debug logging helper ────────────────────────────────────────────────────
static void logHex(const uint8_t *buf, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        DebugLog.printf("%02X ", buf[i]);
    }
    DebugLog.println();
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
    _uartNum = uartNum;

    // ── Configure UART using ESP-IDF driver ───────────────────────────────
    uart_config_t uart_config = {};
    uart_config.baud_rate = CN105_BAUD_RATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_EVEN;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_XTAL;  // Use crystal for precise low baud

    esp_err_t err;
    err = uart_param_config(_uartNum, &uart_config);
    LOG_INFO("[CN105] uart_param_config: %s", esp_err_to_name(err));

    err = uart_set_pin(_uartNum, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    LOG_INFO("[CN105] uart_set_pin(TX=%d, RX=%d): %s", txPin, rxPin, esp_err_to_name(err));

    err = uart_driver_install(_uartNum, 256, 256, 0, NULL, 0);
    LOG_INFO("[CN105] uart_driver_install: %s", esp_err_to_name(err));

    // ── CRITICAL: Enable pull-up on RX pin ────────────────────────────────
    // ESP32-C6 GPIOs float by default. Without pull-up, the UART RX line
    // reads as LOW (= continuous start bits), producing streams of 0x00.
    gpio_set_pull_mode((gpio_num_t)rxPin, GPIO_PULLUP_ONLY);

    // Verify config
    uint32_t baud;
    uart_get_baudrate(_uartNum, &baud);
    uart_word_length_t data_bits;
    uart_get_word_length(_uartNum, &data_bits);
    uart_parity_t parity;
    uart_get_parity(_uartNum, &parity);
    LOG_INFO("[CN105] Verified: baud=%lu, data_bits=%d, parity=%d", baud, data_bits, parity);

    // Flush any stale bytes
    uart_flush_input(_uartNum);

    _state.connected = false;
    _initialConnectDone = false;
    _connectRetries = 0;
    _lastSuccessfulResponse = 0;
    LOG_INFO("[CN105] Controller initialized, waiting for connection...");
}

bool CN105Controller::isHealthy() const {
    return _state.connected &&
           _lastSuccessfulResponse > 0 &&
           (millis() - _lastSuccessfulResponse) < CN105_COMMS_TIMEOUT;
}

uint32_t CN105Controller::getLastResponseAge() const {
    if (_lastSuccessfulResponse == 0) return UINT32_MAX;
    return millis() - _lastSuccessfulResponse;
}

bool CN105Controller::isFieldInGrace(bool hasField) const {
    if (!hasField) return false;
    // Always use wanted value while flag is set (echavet pattern: suppress
    // until heat pump confirms). Safety timeout prevents stuck state if
    // confirmation never arrives (e.g. heat pump rejects the value).
    constexpr uint32_t GRACE_SAFETY_TIMEOUT = 10000;  // 10s max
    return (millis() - _wanted.lastChange) < GRACE_SAFETY_TIMEOUT;
}

CN105State CN105Controller::getEffectiveState() const {
    CN105State eff = _state;
    if (isFieldInGrace(_wanted.hasPower))    eff.power     = _wanted.power;
    if (isFieldInGrace(_wanted.hasMode))     eff.mode      = _wanted.mode;
    if (isFieldInGrace(_wanted.hasTemp))     eff.targetTemp = _wanted.targetTemp;
    if (isFieldInGrace(_wanted.hasFan))      eff.fanSpeed  = _wanted.fanSpeed;
    if (isFieldInGrace(_wanted.hasVane))     eff.vane      = _wanted.vane;
    if (isFieldInGrace(_wanted.hasWideVane)) eff.wideVane  = _wanted.wideVane;
    return eff;
}

// ════════════════════════════════════════════════════════════════════════════
// Main Loop
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::loop() {
    uint32_t now = millis();

    // ── Read any incoming bytes ─────────────────────────────────────────────
    readSerial();

    // ── Connection phase ────────────────────────────────────────────────────
    if (!_state.connected) {
        if (now - _lastConnectAttempt >= CN105_CONNECT_INTERVAL) {
            if (_connectRetries < CN105_MAX_CONNECT_RETRIES) {
                LOG_INFO("[CN105] Sending connect packet (attempt %d/%d)",
                         _connectRetries + 1, CN105_MAX_CONNECT_RETRIES);
                sendConnectPacket();
                _lastConnectAttempt = now;
                _connectRetries++;
            } else {
                if (now - _lastConnectAttempt >= CN105_CONNECT_INTERVAL * 3) {
                    LOG_WARN("[CN105] Max retries reached, resetting connect counter");
                    _connectRetries = 0;
                }
            }
        }
        return;
    }

    // ── Send pending changes (priority over polling, only outside a cycle) ──
    if ((_setFlags1 != 0 || _setFlags2 != 0) && !_cycleRunning) {
        LOG_INFO("[CN105] Sending pending set command (flags=0x%02X flags2=0x%02X)", _setFlags1, _setFlags2);
        sendSetPacket();
        _setFlags1 = 0;
        _setFlags2 = 0;
        _lastCycleEnd = now + CN105_DEFER_DELAY;
        return;
    }

    // ── Cycle-based polling ─────────────────────────────────────────────────
    if (_cycleRunning) {
        if (now - _cycleStartMs > (2 * _updateInterval) + 1000) {
            LOG_WARN("[CN105] Poll cycle TIMEOUT after %lums (phase %d/%d)",
                     now - _cycleStartMs, _pollPhase, CN105_POLL_PHASE_COUNT);
            _cycleRunning = false;
            _awaitingResponse = false;
            _lastCycleEnd = now;
            // Track 0x04 failures for soft disable
            if (_pollPhase < CN105_POLL_PHASE_COUNT &&
                POLL_TYPES[_pollPhase] == CN105_INFO_ERRORCODE) {
                _errorPollFailures++;
                if (_errorPollFailures >= 3) {
                    LOG_WARN("[CN105] Disabling 0x04 error code polling (3 consecutive timeouts)");
                    _errorPollDisabled = true;
                }
            }
        }
    } else {
        if (now >= _lastCycleEnd &&
            (now - _lastCycleEnd) >= _updateInterval) {
            LOG_DEBUG("[CN105] Starting new poll cycle");
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
    // Re-read millis() because readSerial() may have updated _lastSuccessfulResponse
    uint32_t nowMs = millis();
    if (_lastSuccessfulResponse > 0 &&
        (nowMs - _lastSuccessfulResponse) > CN105_COMMS_TIMEOUT) {
        LOG_ERROR("[CN105] COMMUNICATION LOST! No response for %lums (timeout=%dms)",
                  nowMs - _lastSuccessfulResponse, CN105_COMMS_TIMEOUT);
        _state.connected = false;
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
    LOG_INFO("[CN105] CMD: setPower(%s)", on ? "ON" : "OFF");
    _setFlags1 |= CN105_FLAG_POWER;
    _pendingPower = on;
    _wanted.hasPower = true;
    _wanted.power = on;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = millis();

}

void CN105Controller::setMode(uint8_t mode) {
    LOG_INFO("[CN105] CMD: setMode(%s / 0x%02X)", modeToLogStr(mode), mode);
    _setFlags1 |= CN105_FLAG_MODE;
    _pendingMode = mode;
    _wanted.hasMode = true;
    _wanted.mode = mode;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = millis();

}

void CN105Controller::setTargetTemp(float tempC) {
    float clamped = constrain(tempC, CN105_TEMP_MIN, CN105_TEMP_MAX);
    LOG_INFO("[CN105] CMD: setTargetTemp(%.1f°C)", clamped);
    _setFlags1 |= CN105_FLAG_TEMP;
    _pendingTemp = clamped;
    _wanted.hasTemp = true;
    _wanted.targetTemp = clamped;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = millis();

}

void CN105Controller::setFanSpeed(uint8_t speed) {
    LOG_INFO("[CN105] CMD: setFanSpeed(%s / 0x%02X)", fanToLogStr(speed), speed);
    _setFlags1 |= CN105_FLAG_FAN;
    _pendingFan = speed;
    _wanted.hasFan = true;
    _wanted.fanSpeed = speed;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = millis();

}

void CN105Controller::setVane(uint8_t position) {
    LOG_INFO("[CN105] CMD: setVane(%s / 0x%02X)", vaneToLogStr(position), position);
    _setFlags1 |= CN105_FLAG_VANE;
    _pendingVane = position;
    _wanted.hasVane = true;
    _wanted.vane = position;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = millis();

}

void CN105Controller::setWideVane(uint8_t position) {
    LOG_INFO("[CN105] CMD: setWideVane(%s / 0x%02X)", wideVaneToLogStr(position), position);
    _setFlags2 |= CN105_FLAG2_WVANE;
    _pendingWideVane = position;
    _wanted.hasWideVane = true;
    _wanted.wideVane = position;
    _wanted.hasBeenSent = false;
    _wanted.lastChange = millis();

}

void CN105Controller::sendPendingChanges() {
    if ((_setFlags1 == 0 && _setFlags2 == 0) || !_state.connected) return;

    // Don't send SET during an active poll cycle — the heat pump may drop
    // commands while it's processing INFO_REQ/RESP exchanges.  Leave the
    // pending flags set; cn105.loop() will send once the cycle completes.
    if (_cycleRunning) {
        LOG_DEBUG("[CN105] Deferring pending changes (cycle running, flags=0x%02X flags2=0x%02X)",
                  _setFlags1, _setFlags2);
        return;
    }

    LOG_INFO("[CN105] Flushing pending changes (flags=0x%02X flags2=0x%02X)", _setFlags1, _setFlags2);
    sendSetPacket();
    _setFlags1 = 0;
    _setFlags2 = 0;
    _lastCycleEnd = millis() + CN105_DEFER_DELAY;
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
    uart_flush_input(_uartNum);
    _rxLen = 0;

    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        DebugLog.printf("[CN105] TX CONNECT (%d bytes): ", (int)sizeof(CONNECT_PKT));
        logHex(CONNECT_PKT, sizeof(CONNECT_PKT));
    }
    uart_write_bytes(_uartNum, CONNECT_PKT, sizeof(CONNECT_PKT));
    uart_wait_tx_done(_uartNum, pdMS_TO_TICKS(100));
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
        DebugLog.printf("[CN105] TX INFO_REQ type=%s phase=%d/%d (%d bytes): ",
                        typeStr, _pollPhase + 1, CN105_POLL_PHASE_COUNT, 22);
        logHex(pkt, 22);
    }

    uart_write_bytes(_uartNum, pkt, 22);
    uart_wait_tx_done(_uartNum, pdMS_TO_TICKS(200));
}

void CN105Controller::sendSetPacket() {
    uint8_t pkt[22];
    memset(pkt, 0, sizeof(pkt));
    buildHeader(pkt, CN105_PKT_SET, CN105_DATA_LEN);

    pkt[5] = 0x01;
    pkt[6] = _setFlags1;

    if (_setFlags1 & CN105_FLAG_POWER) {
        pkt[8] = _pendingPower ? CN105_POWER_ON : CN105_POWER_OFF;
        LOG_INFO("[CN105] SET power=%s", _pendingPower ? "ON" : "OFF");
    }

    if (_setFlags1 & CN105_FLAG_MODE) {
        pkt[9] = _pendingMode;
        LOG_INFO("[CN105] SET mode=%s (0x%02X)", modeToLogStr(_pendingMode), _pendingMode);
    }

    if (_setFlags1 & CN105_FLAG_TEMP) {
        float clamped = constrain(_pendingTemp, CN105_TEMP_MIN, CN105_TEMP_MAX);
        float rounded = round(clamped * 2.0f) / 2.0f;
        if (_tempMode) {
            // Enhanced mode: byte 19 carries 0.5°C precision, byte 10 = 0
            pkt[10] = 0x00;
            pkt[19] = (uint8_t)((rounded * 2.0f) + 128.0f);
        } else {
            // Legacy mode: byte 10 carries integer temp, byte 19 = 0
            pkt[10] = (uint8_t)(31 - (int)rounded);
        }
        LOG_INFO("[CN105] SET temp=%.1f°C (tempMode=%s)", rounded, _tempMode ? "enhanced" : "legacy");
    }

    if (_setFlags1 & CN105_FLAG_FAN) {
        pkt[11] = _pendingFan;
        LOG_INFO("[CN105] SET fan=%s (0x%02X)", fanToLogStr(_pendingFan), _pendingFan);
    }

    if (_setFlags1 & CN105_FLAG_VANE) {
        pkt[12] = _pendingVane;
        LOG_INFO("[CN105] SET vane=%s (0x%02X)", vaneToLogStr(_pendingVane), _pendingVane);
    }

    // Wide vane (horizontal) — uses second control flag byte (packet[7])
    pkt[7] = _setFlags2;
    if (_setFlags2 & CN105_FLAG2_WVANE) {
        pkt[18] = _pendingWideVane;
        LOG_INFO("[CN105] SET wideVane=%s (0x%02X)", wideVaneToLogStr(_pendingWideVane), _pendingWideVane);
    }

    pkt[21] = calcChecksum(pkt, 21);
    if (currentLogLevel >= LOG_LEVEL_DEBUG) {
        DebugLog.printf("[CN105] TX SET (%d bytes): ", 22);
        logHex(pkt, 22);
    }
    uart_write_bytes(_uartNum, pkt, 22);
    uart_wait_tx_done(_uartNum, pdMS_TO_TICKS(200));
    _wanted.hasBeenSent = true;
}

// ════════════════════════════════════════════════════════════════════════════
// Packet Reception & Parsing
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::readSerial() {
    uint32_t now = millis();

    // Non-blocking bulk read from ESP-IDF UART
    size_t available = 0;
    uart_get_buffered_data_len(_uartNum, &available);

    if (available > 0) {
        uint8_t tmpBuf[128];
        int toRead = (available > sizeof(tmpBuf)) ? sizeof(tmpBuf) : available;
        int bytesRead = uart_read_bytes(_uartNum, tmpBuf, toRead, 0);

        if (bytesRead > 0) {
            _rxLastByte = now;

            for (int i = 0; i < bytesRead; i++) {
                uint8_t b = tmpBuf[i];

                // Sync on header byte
                if (_rxLen == 0 && b != CN105_SYNC) {
                    continue;
                }

                _rxBuf[_rxLen++] = b;

                // If we have the header, check if we have a complete packet
                if (_rxLen >= 5) {
                    uint8_t expectedLen = 5 + _rxBuf[4] + 1;
                    if (_rxLen >= expectedLen) {
                        uint8_t chk = calcChecksum(_rxBuf, expectedLen - 1);
                        if (chk == _rxBuf[expectedLen - 1]) {
                            if (currentLogLevel >= LOG_LEVEL_DEBUG) {
                                DebugLog.printf("[CN105] RX VALID (%d bytes): ", expectedLen);
                                logHex(_rxBuf, expectedLen);
                            }
                            processPacket(_rxBuf, expectedLen);
                        } else {
                            LOG_ERROR("[CN105] RX CHECKSUM FAIL: expected=0x%02X got=0x%02X",
                                      chk, _rxBuf[expectedLen - 1]);
                        }
                        _rxLen = 0;
                    }
                }

                if (_rxLen >= sizeof(_rxBuf)) {
                    LOG_ERROR("[CN105] RX buffer overflow, resetting");
                    _rxLen = 0;
                }
            }
        }
    }

    // Reset RX buffer on timeout (incomplete packet)
    if (_rxLen > 0 && now - _rxLastByte > CN105_RESPONSE_TIMEOUT) {
        LOG_WARN("[CN105] RX timeout (%d bytes incomplete), resetting buffer", _rxLen);
        _rxLen = 0;
    }
}

void CN105Controller::processPacket(const uint8_t *pkt, uint8_t len) {
    uint8_t pktType = pkt[1];

    switch (pktType) {
        case CN105_PKT_CONNECT_OK:
            LOG_INFO("[CN105] Connected to heat pump");
            _state.connected = true;
            _state.lastUpdate = millis();
            _lastSuccessfulResponse = _state.lastUpdate;
            _initialConnectDone = true;
            _connectRetries = 0;
            break;

        case CN105_PKT_SET_ACK:
            LOG_INFO("[CN105] SET command acknowledged");
            _state.lastUpdate = millis();
            _lastSuccessfulResponse = _state.lastUpdate;
            break;

        case CN105_PKT_INFO_RESP:
            if (len >= 7) {
                handleInfoResponse(&pkt[5], pkt[4]);
            }

            if (_cycleRunning && _awaitingResponse) {
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
                    LOG_DEBUG("[CN105] Poll cycle complete (%lums)", millis() - _cycleStartMs);
                    _cycleRunning = false;
                    _lastCycleEnd = millis();
                }
            }
            break;

        default:
            LOG_WARN("[CN105] RX unknown packet type 0x%02X", pktType);
            break;
    }
}

void CN105Controller::handleInfoResponse(const uint8_t *data, uint8_t dataLen) {
    if (dataLen < 6) {
        LOG_WARN("[CN105] INFO_RESP too short (dataLen=%d, need >=6)", dataLen);
        return;
    }

    uint32_t now = millis();
    _state.lastUpdate = now;
    _lastSuccessfulResponse = now;

    switch (data[0]) {
        case CN105_INFO_SETTINGS:
            if (dataLen >= 8) {
                _state.power = (data[3] != 0);
                uint8_t rawMode = data[4];
                if (rawMode > 0x08) {
                    LOG_DEBUG("[CN105] SETTINGS: stripping iSee flag (0x%02X -> 0x%02X)",
                              rawMode, rawMode - 0x08);
                    rawMode -= 0x08;
                }
                _state.mode = rawMode;
                _state.fanSpeed = data[6];
                _state.vane = data[7];

                // Wide vane (horizontal): lower nibble of data[10]
                if (dataLen >= 11) {
                    _state.wideVane = data[10] & 0x0F;
                }

                if (dataLen >= 12 && data[11] != 0) {
                    _state.targetTemp = ((float)data[11] - 128.0f) / 2.0f;
                    _tempMode = true;  // Unit supports enhanced temp encoding
                } else {
                    _state.targetTemp = 31.0f - (float)data[5];
                }
                LOG_DEBUG("[CN105] SETTINGS: power=%s mode=%s fan=%s vane=%s wvane=%s target=%.1f%sC",
                          _state.power ? "ON" : "OFF", modeToLogStr(_state.mode),
                          fanToLogStr(_state.fanSpeed), vaneToLogStr(_state.vane),
                          wideVaneToLogStr(_state.wideVane), _state.targetTemp, "\xC2\xB0");

                // ── Clear wanted flags when heat pump confirms matching values ──
                // Only check after the command has been sent (echavet pattern)
                if (_wanted.hasBeenSent) {
                    if (_wanted.hasPower && _state.power == _wanted.power)
                        _wanted.hasPower = false;
                    if (_wanted.hasMode && _state.mode == _wanted.mode)
                        _wanted.hasMode = false;
                    if (_wanted.hasTemp && fabsf(_state.targetTemp - _wanted.targetTemp) < 0.3f)
                        _wanted.hasTemp = false;
                    if (_wanted.hasFan && _state.fanSpeed == _wanted.fanSpeed)
                        _wanted.hasFan = false;
                    if (_wanted.hasVane && _state.vane == _wanted.vane)
                        _wanted.hasVane = false;
                    if (_wanted.hasWideVane && _state.wideVane == _wanted.wideVane)
                        _wanted.hasWideVane = false;
                }
            }
            break;

        case CN105_INFO_ROOMTEMP:
            if (dataLen >= 7 && data[6] != 0) {
                _state.roomTemp = ((float)data[6] - 128.0f) / 2.0f;
            } else if (dataLen >= 4) {
                _state.roomTemp = (float)data[3] + 10.0f;
            }
            // Outside air temperature: data[5], formula (val - 128) / 2.0
            // Valid when > 1; some units report 0x01 (-63.5C) when idle = invalid
            if (dataLen >= 6 && data[5] > 1) {
                _state.outsideTemp = ((float)data[5] - 128.0f) / 2.0f;
                _state.outsideTempValid = true;
            } else {
                _state.outsideTempValid = false;
            }
            // Runtime hours: data[11:13] as 24-bit value, divided by 60
            if (dataLen >= 14 && (data[11] | data[12] | data[13]) != 0) {
                uint32_t rawMinutes = ((uint32_t)data[11] << 16) |
                                      ((uint32_t)data[12] << 8) |
                                       (uint32_t)data[13];
                _state.runtimeHours = (float)rawMinutes / 60.0f;
                _state.runtimeValid = true;
            }
            if (currentLogLevel >= LOG_LEVEL_DEBUG) {
                char outsideStr[8] = "N/A", runtimeStr[12] = "N/A";
                if (_state.outsideTempValid) snprintf(outsideStr, sizeof(outsideStr), "%.1f", _state.outsideTemp);
                if (_state.runtimeValid) snprintf(runtimeStr, sizeof(runtimeStr), "%.1f", _state.runtimeHours);
                LOG_DEBUG("[CN105] ROOMTEMP: %.1f\xC2\xB0""C  outside=%s  runtime=%s",
                          _state.roomTemp, outsideStr, runtimeStr);
            }
            break;

        case CN105_INFO_STATUS:
            if (dataLen >= 5) {
                _state.compressorHz = data[3];
                _state.operating = (data[4] != 0);
                LOG_DEBUG("[CN105] STATUS: compressor=%dHz operating=%s",
                          _state.compressorHz, _state.operating ? "YES" : "NO");
            }
            break;

        case CN105_INFO_STANDBY:
            if (dataLen >= 6) {
                _state.subMode = data[3];
                _state.stage = data[4];
                _state.autoSubMode = data[5];
                LOG_DEBUG("[CN105] STANDBY: subMode=%s stage=%s autoSub=%s",
                          subModeToLogStr(_state.subMode),
                          stageToLogStr(_state.stage),
                          autoSubModeToLogStr(_state.autoSubMode));
            }
            break;

        case CN105_INFO_ERRORCODE:
            if (dataLen >= 5) {
                _state.errorCode = data[4];
                _state.hasError = (data[4] != 0x80);
                _errorPollFailures = 0;  // Reset failure counter on success
                if (_state.hasError) {
                    LOG_WARN("[CN105] ERROR CODE: 0x%02X", data[4]);
                } else {
                    LOG_DEBUG("[CN105] ERROR: normal (0x80)");
                }
            }
            break;

        default:
            LOG_WARN("[CN105] INFO_RESP unknown type 0x%02X", data[0]);
            break;
    }
}
