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

void CN105Controller::begin(uart_port_t uartNum, int rxPin, int txPin, uint32_t baud) {
#ifndef UNIT_TEST
    _baudRate = baud;
    _hwUart = new HardwareUart(uartNum, rxPin, txPin, baud);
    begin(_hwUart);
#endif
}

void CN105Controller::begin(UartInterface *uart) {
    _uart = uart;
    _uart->flush();
    restartHandshake();
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
    return connected && last > 0 && (uptime_ms() - last) < commsTimeoutMs();
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
    bool inGrace = (uptime_ms() - w.lastChange) < CN105_GRACE_SAFETY_MS;
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

// Transport quantization only; policy validation and compatibility rounding occur upstream.
static float quantizeSetpointFor(float tempC, SetpointRange::Encoding encoding) {
    const float rounded = roundf(tempC * 2.0f) / 2.0f;
    return encoding == SetpointRange::Encoding::Enhanced ? rounded : (float)(int)rounded;
}

CN105RangeCapabilities CN105Controller::rangeCapabilities() const {
    taskENTER_CRITICAL(&_mux);
    const auto caps = _rangeCapabilities;
    taskEXIT_CRITICAL(&_mux);
    return caps;
}

SetpointDelivery CN105Controller::setpointDelivery(float targetC) const {
    if (!std::isfinite(targetC)) return SetpointDelivery::None;
    taskENTER_CRITICAL(&_mux);
    const float q = quantizeSetpointFor(targetC, _state.encoding);
    const bool pending = _wanted.hasTemp && quantizeSetpointFor(_wanted.targetTemp, _state.encoding) == q;
    const bool refused = q == _refusedTemp && _state.targetTemp == _refusedAnswer && _state.mode == _refusedMode;
    taskEXIT_CRITICAL(&_mux);
    return pending ? SetpointDelivery::Pending : refused ? SetpointDelivery::Refused : SetpointDelivery::None;
}

void CN105Controller::clearSetpointRefusal() {
    taskENTER_CRITICAL(&_mux);
    _refusedTemp = _refusedAnswer = _unconfirmedTemp = NAN;
    taskEXIT_CRITICAL(&_mux);
}

// ════════════════════════════════════════════════════════════════════════════
// Main Loop
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::loop() {
    uint32_t now = uptime_ms();

    // ── Baud-rate change (staged by setBaudRate, before any RX at the old rate)
    taskENTER_CRITICAL(&_mux);
    const uint32_t newBaud = _pendingBaud;
    _pendingBaud = 0;
    taskEXIT_CRITICAL(&_mux);
    if (newBaud) applyBaudRate(newBaud);

    // ── Read any incoming bytes ─────────────────────────────────────────────
    readSerial();
    // RX can complete a cycle at a later millisecond than loop entry. Base
    // elapsed-time comparisons on a clock sampled after those timestamps.
    now = uptime_ms();

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

    // Discovery yields immediately to control traffic; its late reply is ignored.
    if (_discovery == Discovery::Waiting &&
        ((uint32_t)(now - _discoverySentMs) >= CN105_RESPONSE_TIMEOUT ||
         _sendRequested || _pendingRemoteTemp)) {
        _discovery = Discovery::Done;
    }

    // ── Send committed changes (priority over polling, only outside a cycle) ──
    // Snapshot-and-clear under the lock, then transmit from the snapshot so a
    // producer staging a new batch mid-TX can neither tear this packet nor
    // lose its own (its flags accumulate in the freshly cleared _staged).
    std::unique_lock<std::mutex> transmitLock(_transmitMutex);
    taskENTER_CRITICAL(&_mux);
    if (_rangeAwareCommand && (_staged.flags1 & CN105_FLAG_TEMP) &&
        (_commandConnection != _state.connectionRevision || _commandEncoding != _state.encoding))
        _commandsHeld = true;
    const bool sendNow = _sendRequested && !_cycleRunning && !_commandsHeld;
    const PendingCommand cmd = _staged;
    if (sendNow) {
        _staged = PendingCommand{};
        _sendRequested = false;
        _wanted.hasBeenSent = true;
        _wanted.sentFlags |= cmd.flags1;
        if (cmd.flags1 & CN105_FLAG_TEMP) _wanted.tempAtSend = _state.targetTemp;
    }
    taskEXIT_CRITICAL(&_mux);
    if (sendNow) {
        LOG_INFO("Sending pending set command (flags=0x%02X flags2=0x%02X)", cmd.flags1, cmd.flags2);
        sendSetPacket(cmd);
        _lastWriteMs = uptime_ms();
        _writeDeferred = true;
        return;
    }

    transmitLock.unlock();

    // ── First connect since boot: one clear unless a source has fed ──────
    // A room source fed before this controller restarted may still be held
    // by the pump. If nothing has written the register since boot (no
    // source is feeding), hand it back to the thermistor once. Checked and
    // staged under one lock, so a source that writes concurrently wins.
    if (_bootClear == BOOT_CLEAR_POLLING && !_cycleRunning) {
        _bootClear = BOOT_CLEAR_DONE;
        taskENTER_CRITICAL(&_mux);
        const bool clear = !_remoteWritten;
        if (clear) {
            _pendingRemoteTemp = true;
            _pendingRemoteTempC = NAN;
        }
        taskEXIT_CRITICAL(&_mux);
        if (clear) LOG_INFO("No room source since boot — clearing any remote temperature the unit kept");
    }

    // ── Send pending remote temperature (same deferred pattern) ──────────
    if (_pendingRemoteTemp && !_cycleRunning) {
        taskENTER_CRITICAL(&_mux);
        const float remoteTempC = _pendingRemoteTempC;
        _pendingRemoteTemp = false;
        taskEXIT_CRITICAL(&_mux);
        sendRemoteTempPacket(remoteTempC);
        _lastWriteMs = uptime_ms();
        _writeDeferred = true;
        return;
    }

    // ── Orphaned-staging watchdog ─────────────────────────────────────────
    // Every setX() must be followed by sendPendingChanges() on the same
    // code path (contract in cn105_protocol.h). A producer that forgets
    // leaves its fields sitting in _staged, silently unsent until an
    // unrelated commit piggybacks them — the user-visible symptom is a
    // command that appears to take, then reverts ~10s later when the
    // anti-flicker grace window expires. Legitimate stage→commit gaps are
    // microseconds, so anything past the threshold is a caller bug.
    {
        constexpr uint32_t ORPHAN_WARN_MS = 5000;
        taskENTER_CRITICAL(&_mux);
        const bool orphaned = !_sendRequested &&
                              (_staged.flags1 != 0 || _staged.flags2 != 0);
        const uint8_t oFlags1 = _staged.flags1, oFlags2 = _staged.flags2;
        taskEXIT_CRITICAL(&_mux);
        if (!orphaned) {
            _stagedOrphanSince = 0;
        } else if (_stagedOrphanSince == 0) {
            _stagedOrphanSince = now;
        } else if (now - _stagedOrphanSince >= ORPHAN_WARN_MS) {
            LOG_WARN("Staged set command (flags=0x%02X flags2=0x%02X) not "
                     "committed for %lus — caller is missing sendPendingChanges()",
                     oFlags1, oFlags2,
                     (unsigned long)((now - _stagedOrphanSince) / 1000));
            _stagedOrphanSince = now;  // re-warn periodically, don't spam every tick
        }
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
        // Writes require a quiet period, but must not restart the poll clock:
        // a 20s sensor keepalive would otherwise starve a 30s poll forever.
        // Both elapsed-time checks remain valid across uptime rollover.
        if (_writeDeferred && (uint32_t)(now - _lastWriteMs) >= CN105_DEFER_DELAY) {
            _writeDeferred = false;
        }
        if (!_writeDeferred && _settingsBaselined && _discovery == Discovery::Idle) {
            static const uint8_t request[] = {0xFC, 0x5B, 0x01, 0x30, 0x01, 0xC9, 0xAA};
            _discovery = Discovery::Waiting;
            _discoverySentMs = now;
            _uart->write(request, sizeof(request));
        }
        if (!_writeDeferred && _discovery != Discovery::Waiting &&
            (uint32_t)(now - _lastCycleEnd) >= _updateInterval) {
            LOG_DEBUG("Starting new poll cycle");
            if (_bootClear == BOOT_CLEAR_ARMED) _bootClear = BOOT_CLEAR_POLLING;
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
        (nowMs - _lastSuccessfulResponse) > commsTimeoutMs()) {
        LOG_ERROR("COMMUNICATION LOST! No response for %lums (timeout=%lums)",
                  (unsigned long)(nowMs - _lastSuccessfulResponse),
                  (unsigned long)commsTimeoutMs());
        restartHandshake();
    }
}

// Back to the connect phase, handshake from attempt 1: the CN105 task's
// answer to a lost link and to a new line rate.
void CN105Controller::restartHandshake() {
    taskENTER_CRITICAL(&_mux);
    if (_rangeAwareCommand) {
        // Keep the same mailbox, but never transmit intent validated for the
        // previous connection until the coordinator has seen the new baseline.
        if (_wanted.hasTemp) { _staged.flags1 |= CN105_FLAG_TEMP; _staged.temp = _wanted.targetTemp; }
        if (_wanted.hasPower) { _staged.flags1 |= CN105_FLAG_POWER; _staged.power = _wanted.power; }
        if (_wanted.hasMode) { _staged.flags1 |= CN105_FLAG_MODE; _staged.mode = _wanted.mode; }
        _commandsHeld = true;
        _sendRequested = _staged.flags1 || _staged.flags2;
        _wanted = WantedSettings{};
    }
    _supersededTemp = NAN;
    ++_state.connectionRevision;
    _state.connected = false;
    _state.encoding = SetpointRange::Encoding::Unknown;
    _settingsBaselined = false;
    _refusedTemp = _refusedAnswer = _unconfirmedTemp = NAN;
    if (_rangeCapabilities.detected.freshMask) {
        _rangeCapabilities.detected.freshMask = 0;
        ++_rangeCapabilities.revision;
    }
    _lastSuccessfulResponse = 0;
    taskEXIT_CRITICAL(&_mux);
    _discovery = Discovery::Idle;
    _connectRetries = 0;
    _cycleRunning = false;
    _awaitingResponse = false;
}

// ════════════════════════════════════════════════════════════════════════════
// Line rate
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::setBaudRate(uint32_t baud) {
    taskENTER_CRITICAL(&_mux);
    _pendingBaud = baud;
    taskEXIT_CRITICAL(&_mux);
}

// CN105 task only. Bytes buffered at the old rate are discarded and the link
// restarts as after a comms loss; encoding and discovery freshness reset.
void CN105Controller::applyBaudRate(uint32_t baud) {
    if (baud == _baudRate) return;
    const uint32_t oldBaud = _baudRate;
    if (!_uart->setBaudRate(baud)) {
        LOG_ERROR("CN105 baud change %lu -> %lu failed; staying at %lu until restart",
                  (unsigned long)oldBaud, (unsigned long)baud, (unsigned long)oldBaud);
        return;
    }
    _baudRate = baud;
    _uart->flush();
    _rxLen = 0;
    restartHandshake();
    LOG_INFO("CN105 baud %lu -> %lu, restarting connect handshake",
             (unsigned long)oldBaud, (unsigned long)baud);
}

// ════════════════════════════════════════════════════════════════════════════
// Commands
// ════════════════════════════════════════════════════════════════════════════

void CN105Controller::queueCommandLocked(const CN105Command& cmd) {
    uint8_t flags = 0;
    if (cmd.power) { flags |= CN105_FLAG_POWER; _staged.power = *cmd.power; _wanted.power = *cmd.power; _wanted.hasPower = true; }
    if (cmd.mode) { flags |= CN105_FLAG_MODE; _staged.mode = *cmd.mode; _wanted.mode = *cmd.mode; _wanted.hasMode = true; }
    if (cmd.targetTemp && _wanted.hasTemp && (_wanted.sentFlags & CN105_FLAG_TEMP) &&
        *cmd.targetTemp != _wanted.targetTemp) {
        _supersededTemp = _wanted.targetTemp;
        _supersededAt = _wanted.lastChange;
    }
    if (cmd.targetTemp) { flags |= CN105_FLAG_TEMP; _staged.temp = *cmd.targetTemp; _wanted.targetTemp = *cmd.targetTemp; _wanted.hasTemp = true; }
    if (cmd.fanSpeed) { flags |= CN105_FLAG_FAN; _staged.fan = *cmd.fanSpeed; _wanted.fanSpeed = *cmd.fanSpeed; _wanted.hasFan = true; }
    if (cmd.vane) { flags |= CN105_FLAG_VANE; _staged.vane = *cmd.vane; _wanted.vane = *cmd.vane; _wanted.hasVane = true; }
    if (cmd.wideVane) { _staged.flags2 |= CN105_FLAG2_WVANE; _staged.wideVane = *cmd.wideVane; _wanted.wideVane = *cmd.wideVane; _wanted.hasWideVane = true; }
    _staged.flags1 |= flags;
    if (flags || cmd.wideVane) {
        _wanted.hasBeenSent = false;
        _wanted.sentFlags &= ~flags;
        _wanted.lastChange = uptime_ms();
        _sendRequested = true;
        _rangeAwareCommand = true;
        _commandConnection = _state.connectionRevision;
        _commandEncoding = _state.encoding;
    }
}

void CN105Controller::cancelWantedLocked(uint8_t flags1, uint8_t flags2) {
    if (flags1 & CN105_FLAG_TEMP) {
        // A transmitted old SET can still be echoed after cancellation. Keep
        // only attribution; canceled intent must not be retried on reconnect.
        if (_wanted.hasTemp && (_wanted.sentFlags & CN105_FLAG_TEMP)) {
            _supersededTemp = _wanted.targetTemp;
            _supersededAt = _wanted.lastChange;
        }
        _wanted.hasTemp = false;
    }
    if (flags1 & CN105_FLAG_POWER) _wanted.hasPower = false;
    if (flags1 & CN105_FLAG_MODE) _wanted.hasMode = false;
    if (flags1 & CN105_FLAG_FAN) _wanted.hasFan = false;
    if (flags1 & CN105_FLAG_VANE) _wanted.hasVane = false;
    if (flags2 & CN105_FLAG2_WVANE) _wanted.hasWideVane = false;
    _wanted.sentFlags &= ~flags1;
}

bool CN105Controller::queueAutoTarget(float targetC, const CN105State& observed,
                                      uint32_t externalRevision) {
    taskENTER_CRITICAL(&_mux);
    const bool current = !_commandsHeld && _state.connected &&
        _state.encoding != SetpointRange::Encoding::Unknown &&
        _state.connectionRevision == observed.connectionRevision &&
        _state.settingsRevision == observed.settingsRevision && _state.encoding == observed.encoding &&
        _externalAutoChanges == externalRevision;
    if (current) {
        CN105Command cmd;
        cmd.targetTemp = targetC;
        queueCommandLocked(cmd);
    }
    taskEXIT_CRITICAL(&_mux);
    return current;
}

void CN105Controller::holdCommands() {
    std::lock_guard<std::mutex> transmitLock(_transmitMutex);
    taskENTER_CRITICAL(&_mux);
    _commandsHeld = true;
    taskEXIT_CRITICAL(&_mux);
}

bool CN105Controller::commandsHeld() const {
    taskENTER_CRITICAL(&_mux);
    const bool held = _commandsHeld;
    taskEXIT_CRITICAL(&_mux);
    return held;
}

CN105Command CN105Controller::heldCommand() const {
    taskENTER_CRITICAL(&_mux);
    CN105Command cmd;
    if (_staged.flags1 & CN105_FLAG_POWER) cmd.power = _staged.power;
    if (_staged.flags1 & CN105_FLAG_MODE) cmd.mode = _staged.mode;
    if (_staged.flags1 & CN105_FLAG_TEMP) cmd.targetTemp = _staged.temp;
    if (_staged.flags1 & CN105_FLAG_FAN) cmd.fanSpeed = _staged.fan;
    if (_staged.flags1 & CN105_FLAG_VANE) cmd.vane = _staged.vane;
    if (_staged.flags2 & CN105_FLAG2_WVANE) cmd.wideVane = _staged.wideVane;
    taskEXIT_CRITICAL(&_mux);
    return cmd;
}

void CN105Controller::resumeCommands(const CN105Command& cmd, const CN105State& validated) {
    taskENTER_CRITICAL(&_mux);
    // Replacing the held batch also retires wanted fields removed from it.
    // In-flight fields not canceled by this command keep their confirmation state.
    uint8_t canceled = 0;
    if (!cmd.power) canceled |= _staged.flags1 & CN105_FLAG_POWER;
    if (!cmd.mode) canceled |= _staged.flags1 & CN105_FLAG_MODE;
    if (!cmd.targetTemp) canceled |= _staged.flags1 & CN105_FLAG_TEMP;
    if (!cmd.fanSpeed) canceled |= _staged.flags1 & CN105_FLAG_FAN;
    if (!cmd.vane) canceled |= _staged.flags1 & CN105_FLAG_VANE;
    if (cmd.power && !*cmd.power) canceled |= CN105_FLAG_POWER | CN105_FLAG_MODE | CN105_FLAG_TEMP;
    if (cmd.mode && *cmd.mode == CN105_MODE_FAN && !cmd.targetTemp) canceled |= CN105_FLAG_TEMP;
    cancelWantedLocked(canceled, cmd.wideVane ? 0 : _staged.flags2);
    _staged = PendingCommand{};
    _sendRequested = false;
    queueCommandLocked(cmd);
    _commandConnection = validated.connectionRevision;
    _commandEncoding = validated.encoding;
    _commandsHeld = cmd.targetTemp && (_state.connectionRevision != validated.connectionRevision ||
        _state.encoding != validated.encoding || validated.encoding == SetpointRange::Encoding::Unknown);
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setPower(bool on) {
    LOG_INFO("CMD: setPower(%s)", on ? "ON" : "OFF");
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_POWER;
    _staged.power = on;
    _wanted.hasPower = true;
    _wanted.power = on;
    _wanted.hasBeenSent = false;
    _wanted.sentFlags &= ~CN105_FLAG_POWER;
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
    _wanted.sentFlags &= ~CN105_FLAG_MODE;
    _wanted.lastChange = now;
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::setTargetTemp(float tempC) {
    if (!std::isfinite(tempC)) return;
    float clamped = std::clamp(tempC, CN105_TEMP_MIN, CN105_TEMP_MAX);
    LOG_INFO("CMD: setTargetTemp(%.1f%sC)", clamped, "\xC2\xB0");
    uint32_t now = uptime_ms();
    taskENTER_CRITICAL(&_mux);
    _staged.flags1 |= CN105_FLAG_TEMP;
    _staged.temp = clamped;
    _wanted.hasTemp = true;
    _wanted.targetTemp = clamped;
    _wanted.hasBeenSent = false;
    _wanted.sentFlags &= ~CN105_FLAG_TEMP;
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
    _wanted.sentFlags &= ~CN105_FLAG_FAN;
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
    _wanted.sentFlags &= ~CN105_FLAG_VANE;
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
    if (!std::isfinite(tempC)) {
        LOG_WARN("sendRemoteTemperature(non-finite) ignored — use clearRemoteTemperature() to clear");
        return;
    }
    taskENTER_CRITICAL(&_mux);
    _pendingRemoteTemp = true;
    _pendingRemoteTempC = tempC;
    _remoteWritten = true;
    taskEXIT_CRITICAL(&_mux);
    LOG_DEBUG("CMD: sendRemoteTemperature(%.1f%sC)", tempC, "\xC2\xB0");
}

void CN105Controller::clearRemoteTemperature() {
    taskENTER_CRITICAL(&_mux);
    _pendingRemoteTemp = true;
    _pendingRemoteTempC = NAN;
    _remoteWritten = true;
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
    taskENTER_CRITICAL(&_mux);
    const auto encoding = _state.encoding;
    taskEXIT_CRITICAL(&_mux);
    return quantizeSetpointFor(std::clamp(tempC, CN105_TEMP_MIN, CN105_TEMP_MAX), encoding);
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
        // Range-aware commands have already been normalized. Retain only
        // wire-grid conversion here; do not reapply the compatibility clamp.
        float q = quantizeSetpointFor(cmd.temp, _state.encoding);
        if (_state.encoding == SetpointRange::Encoding::Enhanced) {
            // Enhanced mode: byte 19 carries 0.5C precision, byte 10 = 0
            pkt[10] = 0x00;
            pkt[19] = (uint8_t)((q * 2.0f) + 128.0f);
        } else {
            // Legacy mode: byte 10 carries integer temp, byte 19 = 0
            pkt[10] = (uint8_t)(31 - (int)q);
        }
        LOG_INFO("SET temp=%.1f%sC (tempMode=%s)", q, "\xC2\xB0", _state.encoding == SetpointRange::Encoding::Enhanced ? "enhanced" : "legacy");
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
            ++_state.connectionRevision;
            _state.connected = true;
            _state.encoding = SetpointRange::Encoding::Unknown;
            if (_rangeCapabilities.detected.freshMask) {
                _rangeCapabilities.detected.freshMask = 0;
                ++_rangeCapabilities.revision;
            }
            _state.lastUpdate = uptime_ms();
            _lastSuccessfulResponse = _state.lastUpdate;
            taskEXIT_CRITICAL(&_mux);
            _discovery = Discovery::Idle;
            _connectRetries = 0;
            _cycleRunning = false;
            _awaitingResponse = false;
            _writeDeferred = false;
            // A new handshake always starts a fresh poll immediately. Do not
            // retain a pre-disconnect cycle timestamp or SET deferral.
            _lastCycleEnd = uptime_ms() - _updateInterval;
            if (_bootClearEnabled && _bootClear != BOOT_CLEAR_DONE) _bootClear = BOOT_CLEAR_ARMED;
            // Whatever changed while we were away cannot be attributed; the
            // first SETTINGS of this connection is a baseline.
            taskENTER_CRITICAL(&_mux);
            _settingsBaselined = false;
            // A refusal was the answer of the unit as it was then; after a
            // reconnect it may answer differently.
            _refusedTemp = _refusedAnswer = _unconfirmedTemp = NAN;
            taskEXIT_CRITICAL(&_mux);
            break;

        case CN105_PKT_SET_ACK:
            LOG_INFO("SET command acknowledged");
            taskENTER_CRITICAL(&_mux);
            _state.lastUpdate = uptime_ms();
            _lastSuccessfulResponse = _state.lastUpdate;
            taskEXIT_CRITICAL(&_mux);
            break;

        case 0x7B:
            if (_discovery == Discovery::Waiting &&
                (uint32_t)(uptime_ms() - _discoverySentMs) < CN105_RESPONSE_TIMEOUT &&
                pkt[4] == 16 && len == 22 && pkt[5] == 0xC9) {
                _discovery = Discovery::Done;
                handleIdentifyResponse(&pkt[5], pkt[4]);
            }
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
                    next.encoding = SetpointRange::Encoding::Enhanced;
                } else {
                    next.targetTemp = 31.0f - (float)data[5];
                    next.encoding = SetpointRange::Encoding::Legacy;
                }
                LOG_DEBUG("SETTINGS: power=%s mode=%s fan=%s vane=%s wvane=%s target=%.1f%sC",
                          next.power ? "ON" : "OFF", modeToLogStr(next.mode),
                          fanToLogStr(next.fanSpeed), vaneToLogStr(next.vane),
                          wideVaneToLogStr(next.wideVane), next.targetTemp, "\xC2\xB0");
                ++next.settingsRevision;
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
    // External Auto change detection runs before the confirmation below
    // clears the wanted flags: those flags are what tells our own SET's
    // echo (or the unit's clamped version of it) apart from a wall
    // controller's write. A field is ours while its latest value has been
    // sent (per field: sentFlags) and is still inside the grace window.
    const bool wasBaselined = _settingsBaselined;
    if (settingsDecoded && (next.targetTemp != _state.targetTemp || next.mode != _state.mode)) {
        _refusedTemp = _refusedAnswer = _unconfirmedTemp = NAN;
    }
    if (settingsDecoded) {
        if (_settingsBaselined) {
            const bool autoNow    = next.power && next.mode == CN105_MODE_AUTO;
            const bool autoBefore = _state.power && _state.mode == CN105_MODE_AUTO;
            const bool tempMoved  = fabsf(next.targetTemp - _state.targetTemp) > 0.01f;
            const bool inGrace    = (now - _wanted.lastChange) < CN105_GRACE_SAFETY_MS;
            const uint8_t sent    = _wanted.sentFlags;
            const bool ourTemp    = (_wanted.hasTemp && (sent & CN105_FLAG_TEMP) && inGrace) ||
                (next.targetTemp == _supersededTemp && (now - _supersededAt) < CN105_GRACE_SAFETY_MS);
            const bool ourEntry   = ((_wanted.hasMode && (sent & CN105_FLAG_MODE)) ||
                                     (_wanted.hasPower && (sent & CN105_FLAG_POWER))) && inGrace;
            if (autoNow && ((!autoBefore && !ourEntry) || (autoBefore && tempMoved && !ourTemp)))
                _externalAutoChanges++;
        }
        _settingsBaselined = true;
    }
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
        if (_wanted.hasFan && next.fanSpeed == _wanted.fanSpeed)
            _wanted.hasFan = false;
        if (_wanted.hasVane && next.vane == _wanted.vane)
            _wanted.hasVane = false;
        if (_wanted.hasWideVane && next.wideVane == _wanted.wideVane)
            _wanted.hasWideVane = false;
    }
    // The setpoint confirms on its own sent flag, so a command for another
    // field staged meanwhile cannot hold a matching answer open (and the
    // refusal check below can never see our own value as a refusal).
    if (settingsDecoded && _wanted.hasTemp && (_wanted.sentFlags & CN105_FLAG_TEMP) &&
        fabsf(next.targetTemp - quantizeSetpointFor(_wanted.targetTemp, next.encoding)) < 0.3f) {
        _wanted.hasTemp = false;
        _unconfirmedTemp = NAN;
    }
    // Grace over and the unit still reports another setpoint: that is its
    // answer. A unit that moved took the SET and settled elsewhere — it will
    // not hold our value, so latch the refusal (refusedSetpoint). One that
    // stayed put may have lost the SET, so the value gets one retry before
    // it is latched. Either way the declined value stops masking the real
    // one. The first answer of a connection has nothing to compare against.
    if (settingsDecoded && _wanted.hasTemp && (_wanted.sentFlags & CN105_FLAG_TEMP) &&
        (now - _wanted.lastChange) >= CN105_GRACE_SAFETY_MS) {
        const float q = quantizeSetpointFor(_wanted.targetTemp, next.encoding);
        const bool moved = fabsf(next.targetTemp - _wanted.tempAtSend) > 0.01f;
        if (wasBaselined) {
            if (moved || q == _unconfirmedTemp) {
                _refusedTemp = q;
                _refusedAnswer = next.targetTemp;
                _refusedMode = next.mode;
                _unconfirmedTemp = NAN;
            } else {
                _unconfirmedTemp = q;
            }
        }
        _wanted.hasTemp = false;
    }
    taskEXIT_CRITICAL(&_mux);
}

void CN105Controller::handleIdentifyResponse(const uint8_t *data, uint8_t dataLen) {
    if (dataLen != 16 || !(data[8] & 0x04)) return;
    // Decode outside the lock. The shared policy decides whether each raw
    // pair has a usable intersection with this connection's encoding/grid.
    SetpointRange::Detection candidates{};
    SetpointRange::Preferences preferences{};
    static const uint8_t offsets[] = {12, 10, 14};  // Heat, Cool/Dry, Auto
    for (uint8_t mode = 0; mode < 3; ++mode) {
        preferences.mode[mode].selection = SetpointRange::Selection::Automatic;
        const uint8_t low = data[offsets[mode]], high = data[offsets[mode] + 1];
        // Enhanced endpoint bytes use 128 as the encoding origin; zero,
        // the disable marker, and 0xFF do not describe an available endpoint.
        if (low <= 128 || high <= 128 || low == 255 || high == 255 || low >= high) continue;
        candidates.mode[mode] = {static_cast<int16_t>(low - 128), static_cast<int16_t>(high - 128)};
        candidates.validMask |= 1 << mode;
        candidates.freshMask |= 1 << mode;
    }
    const auto resolved = SetpointRange::resolve(preferences, candidates, _state.encoding, _state.encoding, 0);
    taskENTER_CRITICAL(&_mux);
    bool changed = false;
    for (uint8_t mode = 0; mode < 3; ++mode) {
        if (resolved.mode[mode].source != SetpointRange::Source::Fresh) continue;
        const uint8_t bit = 1 << mode;
        auto &detected = _rangeCapabilities.detected;
        const auto previous = detected.mode[mode];
        const auto range = candidates.mode[mode];
        if (!(detected.validMask & bit) || !(detected.freshMask & bit) ||
            previous.min2 != range.min2 || previous.max2 != range.max2) changed = true;
        detected.mode[mode] = range;
        detected.validMask |= bit;
        detected.freshMask |= bit;
    }
    if (changed) ++_rangeCapabilities.revision;
    taskEXIT_CRITICAL(&_mux);
}
