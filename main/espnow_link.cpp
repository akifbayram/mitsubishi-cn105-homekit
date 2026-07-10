#include "espnow_link.h"
#if ESPNOW_REMOTE_ENABLE

#include <cstring>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_console.h>
#include <esp_mac.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <cmath>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_utils.h"
#include "logging.h"
#include "espnow_proto.h"
#include "espnow_crypto.h"
#include "espnow_bond.h"
#include "cn105_protocol.h"
#include "wifi_manager.h"
#include "homekit_setup.h"
#include "settings.h"
#include "ble_config.h"
#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif

static const char *TAG = "espnow_link";

#define ESPNOW_PAIR_WINDOW_MS 120000
#define STATE_MIN_INTERVAL  250     // ms: floor between event-driven STATEs
#define STATE_HEARTBEAT_MS  10000   // ms: max gap between STATEs while peer live
#define INFO_MIN_INTERVAL   2000    // ms: INFO cadence while the Dial asks (want_info)

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

EspnowLink espnowLink;

static uint8_t s_ownMac[6];

// Latest decoded command, handed from the RX callback to loop() to apply on the
// main task (callbacks run on the WiFi task — keep them short).
static portMUX_TYPE           s_cmdMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool          s_haveCmd = false;
static struct espnow_cmd_pkt  s_cmd;
static volatile uint32_t      s_lastProbeMs = 0;
static volatile uint8_t       s_wantInfo = 0;
static volatile uint8_t       s_wantIdent = 0;
static volatile bool          s_haveWifiReq = false;   // Link OTA: creds request pending
static volatile bool          s_haveScanReq = false;
static volatile bool          s_haveWifiSet = false;
static struct espnow_wifi_set_pkt s_wifiSet;   // guarded by s_cmdMux
// Protocol version of the last accepted peer packet. Lets the unit notice an
// out-of-date (or ahead-of-us) Dial and warn instead of silently ignoring it.
static volatile uint8_t       s_peerVer = 0;

// Pairing packets handed from the RX callback to pairLoop() (main task) so the
// WiFi-task callback never runs crypto / esp_now_send / esp_restart (ESP-NOW
// docs: "do not do lengthy operations in the callback").
static volatile bool          s_havePairReq = false;
static volatile bool          s_havePairProbe = false;
static struct espnow_pair_req_pkt s_pendReq;

static EspnowLink *s_self = nullptr;

static void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!s_self) return;
    if (len < 2) return;
    const uint8_t type = data[0];
    const uint8_t ver  = data[1];
    // Version gate (see ESPNOW_PROTO_MIN_COMPAT). Accept anything at or above the
    // compat floor — a newer peer's additively-grown packet parses fine because
    // we read only the prefix we know and the `len >=` checks below tolerate the
    // extra tail. PROBE and PAIR bypass the floor so an out-of-date peer is still
    // *seen* (version recorded, warned in loop()) rather than vanishing, which is
    // what lets a stale Dial be told to update instead of just going dark.
    const bool negotiation = (type == ESPNOW_PKT_PROBE ||
                              type == ESPNOW_PKT_PAIR_REQ ||
                              type == ESPNOW_PKT_PAIR_RESP);
    if (!negotiation && ver < ESPNOW_PROTO_MIN_COMPAT) return;

    // Pairing path: PAIR_REQ on broadcast, and the confirming PROBE.
    if (s_self->pairingActive()) {
        s_self->onPairRecv(info->src_addr, data, len);
        // pairing packets are fully handled in onPairRecv; the bonded path below
        // stays inert until a bond is persisted (_bonded is false during PAIR_CONFIRM)
    }
    if (!s_self->isBonded()) return;
    uint8_t peer[6]; s_self->getPeerMac(peer);
    if (memcmp(info->src_addr, peer, 6) != 0) return;            // allow-list
    if (memcmp(info->des_addr, s_ownMac, 6) != 0) return;        // unicast only
    // Record the bonded peer's protocol version for the skew warning in loop().
    // An old bonded Dial whose STATE/CMD fail the floor still gets here via its
    // PROBE (which bypasses the floor), so a real breaking skew is still seen.
    s_peerVer = ver;
    switch (data[0]) {
        case ESPNOW_PKT_PROBE:
            s_lastProbeMs = uptime_ms();
            // MIN_LEN, not sizeof: a MIN_COMPAT-era probe is shorter than the
            // current struct (v6 added a reserved tail) and must still count.
            if (len >= ESPNOW_PROBE_MIN_LEN) s_wantInfo = data[2];
            s_wantIdent = (len >= 4) ? data[3] : 0;   // v11; v<=10 peers send 0
            break;
        case ESPNOW_PKT_CMD:
            // MIN_LEN + tolerant decode: a MIN_COMPAT-era Dial sends a shorter
            // CMD (missing tail decodes as zeros); a newer Dial may send a
            // longer one (unknown tail ignored).
            if (len >= ESPNOW_CMD_MIN_LEN) {
                struct espnow_cmd_pkt c;
                espnow_decode_pkt(&c, sizeof(c), data, len);
                portENTER_CRITICAL(&s_cmdMux);
                memcpy((void*)&s_cmd, &c, sizeof(s_cmd)); s_haveCmd = true;
                portEXIT_CRITICAL(&s_cmdMux);
                s_lastProbeMs = uptime_ms();
            }
            break;
        case ESPNOW_PKT_WIFI_REQ:
            // Link OTA: reply is deferred to loop() (NVS read + send off the WiFi task).
            if (len >= ESPNOW_WIFI_REQ_MIN_LEN) {
                s_haveWifiReq = true;
                s_lastProbeMs = uptime_ms();
            }
            break;
        case ESPNOW_PKT_WIFI_SCAN_REQ:
            // Dial-driven provisioning: scan is deferred to loop() (blocking scan
            // must not run on the WiFi task).
            if (len >= ESPNOW_WIFI_SCAN_REQ_MIN_LEN) {
                s_haveScanReq = true;
                s_lastProbeMs = uptime_ms();
            }
            break;
        case ESPNOW_PKT_WIFI_SET:
            if (len >= ESPNOW_WIFI_SET_MIN_LEN) {
                struct espnow_wifi_set_pkt w;
                espnow_decode_pkt(&w, sizeof(w), data, len);
                portENTER_CRITICAL(&s_cmdMux);
                memcpy((void*)&s_wifiSet, &w, sizeof(s_wifiSet)); s_haveWifiSet = true;
                portEXIT_CRITICAL(&s_cmdMux);
                memset(&w, 0, sizeof(w));   // no PSK copy left on this stack
                s_lastProbeMs = uptime_ms();
            }
            break;
        default: break;
    }
}

void EspnowLink::ensureEspnowInit() {
    if (_espnowReady) return;
    if (esp_now_init() != ESP_OK) { LOG_ERROR("esp_now_init failed"); return; }
    esp_wifi_get_mac(WIFI_IF_STA, s_ownMac);
    _havePmk = espnow_pmk_load(_pmk);
    if (_havePmk) {
        if (esp_now_set_pmk(_pmk) != ESP_OK) { LOG_ERROR("esp_now_set_pmk failed"); return; }
    } else {
        LOG_WARN("no PMK provisioned — pairing disabled (console: espnow-pmk <32-hex>)");
    }
    esp_now_register_recv_cb(onRecv);
    _espnowReady = true;
}

void EspnowLink::begin(CN105Controller *ctrl) {
    _ctrl = ctrl;
    s_self = this;
    uint8_t lmk[16];
    if (!espnow_bond_load(_peer, lmk)) {
        LOG_INFO("ESP-NOW remote: no bond, link idle");
        return;
    }
    ensureEspnowInit();
    if (!_espnowReady) return;
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, _peer, 6);
    pi.ifidx = WIFI_IF_STA; pi.channel = 0; pi.encrypt = true;
    memcpy(pi.lmk, lmk, 16);
    if (esp_now_add_peer(&pi) != ESP_OK) { LOG_ERROR("esp_now_add_peer failed"); return; }
    _bonded = true;
    if (!_havePmk) {
        LOG_WARN("bonded but no PMK in NVS — the encrypted link will NOT work; "
                 "provision with 'espnow-pmk <32-hex>' and reboot");
    }
    LOG_INFO("ESP-NOW remote bonded to %02X:%02X:%02X:%02X:%02X:%02X",
             _peer[0],_peer[1],_peer[2],_peer[3],_peer[4],_peer[5]);
}

void EspnowLink::getPeerMac(uint8_t out[6]) const { memcpy(out, _peer, 6); }
bool EspnowLink::isPeerLive() const {
    return _bonded && s_lastProbeMs != 0 && (uptime_ms() - s_lastProbeMs) < 6000;
}

void EspnowLink::buildState(struct espnow_state_pkt *p) {
    memset(p, 0, sizeof(*p));
    const CN105State st = _ctrl->getEffectiveState();
    p->type = ESPNOW_PKT_STATE; p->version = ESPNOW_PROTO_VERSION;
    p->flags = espnow_make_state_flags(
        st.power, _ctrl->isConnected(), st.operating, WifiManager::isConnected(),
        homekit_get_controller_count() > 0, settings.get().useFahrenheit,
        settings.get().vaneConfig);
    p->mode = st.mode; p->fan = st.fanSpeed; p->vane = st.vane; p->wide_vane = st.wideVane;
    p->error_code = st.errorCode;
    p->room_dc = espnow_c_to_dc(st.roomTemp);
    p->set_dc  = espnow_c_to_dc(st.targetTemp);
    if (WifiManager::hasCredentials()) p->flags2 |= ESPNOW_SF2_WIFI_PROVISIONED;
#ifdef BLE_ENABLE
    /* Remote-sensor low-battery latch: ON at <=10%, clear only at >=15% so a
     * cell hovering at the threshold doesn't flap the home-face chip. A stale
     * sensor (isActive()==false) clears it — a dead sensor already surfaces via
     * the room-temp fallback; this chip means "sensor alive but running out". */
    static bool s_battLatch = false;
    if (BleSensor::isEnabled() && BleSensor::isActive()) {
        int8_t b = BleSensor::battery();
        if (b >= 0) {
            if (b <= 10)      s_battLatch = true;
            else if (b >= 15) s_battLatch = false;
        }
    } else {
        s_battLatch = false;
    }
    if (s_battLatch) p->flags2 |= ESPNOW_SF2_SENSOR_BATT_LOW;
#endif
}

void EspnowLink::buildInfo(struct espnow_info_pkt *p) {
    memset(p, 0, sizeof(*p));
    const CN105State st = _ctrl->getEffectiveState();
    p->type = ESPNOW_PKT_INFO; p->version = ESPNOW_PROTO_VERSION;
    p->iflags = espnow_make_info_flags(st.outsideTempValid);
    p->compressor_hz = st.compressorHz;
    p->sub_mode = st.subMode; p->stage = st.stage;
    p->outside_dc = espnow_c_to_dc(st.outsideTemp);
    p->hk_paired = (uint8_t)homekit_get_controller_count();
    p->wifi_rssi = WifiManager::getRSSI();
    WifiManager::getSSID((char*)p->ssid, sizeof(p->ssid));
    WifiManager::getIP((char*)p->ip, sizeof(p->ip));
    strncpy((char*)p->hk_code, homekit_get_setup_code(), sizeof(p->hk_code) - 1);
    const char *payload = homekit_get_setup_payload();
    if (payload) strncpy((char*)p->hk_payload, payload, sizeof(p->hk_payload) - 1);
#ifdef BLE_ENABLE
    if (BleSensor::isEnabled()) {
        int8_t b = BleSensor::battery();
        if (b >= 0) {
            p->sensor_batt_pct = (uint8_t)b;
            p->iflags |= ESPNOW_IF_SENSORBATT_VALID;
        }
    }
#endif
}

void EspnowLink::buildDiag(struct espnow_diag_pkt *p) {
    memset(p, 0, sizeof(*p));
    p->type = ESPNOW_PKT_DIAG; p->version = ESPNOW_PROTO_VERSION;
    const CN105State st = _ctrl->getEffectiveState();
#ifdef FW_VERSION
    /* bounded memcpy, not strncpy: FW_VERSION can be >= sizeof(fw_ver), which
       trips -Werror=stringop-truncation; memset above leaves it null-terminated */
    memcpy(p->fw_ver, FW_VERSION, strnlen(FW_VERSION, sizeof(p->fw_ver) - 1));
#endif
    /* bounded memcpy, not strncpy: date[16] > build_date[12] trips -Werror=stringop-truncation */
    const char *date = esp_app_get_description()->date;
    memcpy(p->build_date, date, strnlen(date, sizeof(p->build_date) - 1));
    /* esp_timer (64-bit µs), not uptime_ms(): the 32-bit ms tick wraps at ~49.7 days */
    p->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    p->reset_reason = (uint8_t)esp_reset_reason();
    uint8_t ch = 0; wifi_second_chan_t sc;
    if (esp_wifi_get_channel(&ch, &sc) == ESP_OK && WifiManager::isConnected())
        p->wifi_channel = ch;
    p->auto_sub_mode = st.autoSubMode;
    if (st.runtimeValid) {
        p->dflags |= ESPNOW_DF_RUNTIME_VALID;
        p->runtime_h = (uint32_t)st.runtimeHours;
    }
#ifdef BLE_ENABLE
    if (BleSensor::isEnabled() && BleSensor::isActive()) {
        float t = BleSensor::temperature();
        if (!std::isnan(t)) {
            p->dflags |= ESPNOW_DF_BLE_VALID;
            p->ble_temp_dc = espnow_c_to_dc(t);
            float h = BleSensor::humidity();
            p->ble_hum_pct = std::isnan(h) ? 0xFF : (uint8_t)lroundf(h);
            p->ble_rssi = (int8_t)BleSensor::rssi();
            /* "in control" = the feed is on and alive, so the heat pump is
             * running off REMOTE_TEMP keepalives (see BleSensor::loop). */
            if (settings.get().bleFeedEnabled) p->dflags |= ESPNOW_DF_TEMP_SRC_REMOTE;
        }
    }
#endif
}

void EspnowLink::startPairing() {
    ensureEspnowInit();
    if (!_espnowReady) { _pairResult = "timeout"; return; }
    if (!esp_now_is_peer_exist(BCAST)) {
        esp_now_peer_info_t bp = {};
        memcpy(bp.peer_addr, BCAST, 6);
        bp.ifidx = WIFI_IF_STA; bp.channel = 0; bp.encrypt = false;
        esp_now_add_peer(&bp);
    }
    if (espnow_crypto_keypair(_ownPriv, _ownPub) != 0) { _pairResult = "timeout"; return; }
    _pair = PAIR_LISTEN;
    _pairStartMs = uptime_ms();
    _pairResult = "listening";
    LOG_INFO("ESP-NOW pairing window opened (%d s)", ESPNOW_PAIR_WINDOW_MS / 1000);
}

void EspnowLink::cancelPairing() {
    if (_pair == PAIR_OFF) return;
    _pair = PAIR_OFF;
    if (esp_now_is_peer_exist(BCAST)) esp_now_del_peer(BCAST);
    if (!_bonded && esp_now_is_peer_exist(_candMac)) esp_now_del_peer(_candMac);
    if (strcmp(_pairResult, "paired") != 0 && strcmp(_pairResult, "timeout") != 0 &&
        strcmp(_pairResult, "error") != 0)
        _pairResult = "idle";
}

bool EspnowLink::pairingActive() const { return _pair != PAIR_OFF; }
int  EspnowLink::pairingSecondsLeft() const {
    if (_pair == PAIR_OFF) return 0;
    uint32_t el = uptime_ms() - _pairStartMs;
    return el >= ESPNOW_PAIR_WINDOW_MS ? 0 : (int)((ESPNOW_PAIR_WINDOW_MS - el) / 1000);
}
const char *EspnowLink::pairResult() const { return _pairResult; }

void EspnowLink::onPairRecv(const uint8_t src[6], const uint8_t *data, int len) {
    // Capture only. All heavy work (verify, derive, send, restart) happens in
    // pairLoop() on the main task — this runs on the WiFi task and must stay short.
    if (data[0] == ESPNOW_PKT_PAIR_REQ &&
        len == (int)sizeof(struct espnow_pair_req_pkt) &&
        (_pair == PAIR_LISTEN || _pair == PAIR_CONFIRM)) {
        portENTER_CRITICAL(&s_cmdMux);
        memcpy((void *)&s_pendReq, data, sizeof(s_pendReq));
        s_havePairReq = true;
        portEXIT_CRITICAL(&s_cmdMux);
        return;
    }
    if (_pair == PAIR_CONFIRM && data[0] == ESPNOW_PKT_PROBE &&
        memcmp(src, _candMac, 6) == 0) {
        s_havePairProbe = true;
    }
}

void EspnowLink::pairLoop() {
    if (_restartAtMs) {
        if ((int32_t)(uptime_ms() - _restartAtMs) >= 0) esp_restart();
        return;
    }
    if (_pair == PAIR_OFF) return;

    if (uptime_ms() - _pairStartMs >= ESPNOW_PAIR_WINDOW_MS) {
        LOG_WARN("ESP-NOW pairing window timed out");
        _pairResult = "timeout";
        cancelPairing();
        return;
    }

    // Process a captured PAIR_REQ: verify, (re)send RESP, and on the first valid
    // REQ derive the LMK + install the encrypted peer. Re-replying on every REQ
    // lets the channel-sweeping dial catch a RESP even if it missed earlier ones.
    bool haveReq = false;
    struct espnow_pair_req_pkt req;
    portENTER_CRITICAL(&s_cmdMux);
    if (s_havePairReq) { memcpy(&req, (const void *)&s_pendReq, sizeof(req)); s_havePairReq = false; haveReq = true; }
    portEXIT_CRITICAL(&s_cmdMux);
    if (haveReq) {
        uint8_t tr[40]; espnow_pair_req_transcript(&req, tr);
        uint8_t tag[16];
        espnow_crypto_auth_tag(_pmk, 16, tr, sizeof(tr), tag);
        bool ok = _havePmk && espnow_crypto_tag_ok(tag, req.tag) &&
                  !(_pair == PAIR_CONFIRM && memcmp(req.src_mac, _candMac, 6) != 0);
        if (ok) {
            struct espnow_pair_resp_pkt resp; memset(&resp, 0, sizeof(resp));
            resp.type = ESPNOW_PKT_PAIR_RESP; resp.version = ESPNOW_PROTO_VERSION;
            memcpy(resp.src_mac, s_ownMac, 6);
            memcpy(resp.pub, _ownPub, 32);
            uint8_t rtr[72]; espnow_pair_resp_transcript(&resp, req.pub, rtr);
            espnow_crypto_auth_tag(_pmk, 16, rtr, sizeof(rtr), resp.tag);
            // Burst the RESP: the sweeping Dial parks on this channel only briefly
            // and our reply is deferred to the main loop, so a single send easily
            // misses its listen window. The spacing keeps ESP-NOW's async TX queue
            // from overflowing (ESP_ERR_ESPNOW_NO_MEM).
            for (int i = 0; i < 3; i++) {
                esp_now_send(BCAST, (const uint8_t *)&resp, sizeof(resp));
                vTaskDelay(pdMS_TO_TICKS(10));  // 1 tick @100Hz; pdMS_TO_TICKS(<10)=0 = no spacing
            }
            if (_pair == PAIR_LISTEN &&
                espnow_crypto_derive_lmk(_ownPriv, req.pub, req.pub, _ownPub, _candLmk) == 0) {
                memcpy(_candMac, req.src_mac, 6);
                esp_now_peer_info_t pi = {};
                memcpy(pi.peer_addr, _candMac, 6);
                pi.ifidx = WIFI_IF_STA; pi.channel = 0; pi.encrypt = true;
                memcpy(pi.lmk, _candLmk, 16);
                esp_err_t pe = esp_now_add_peer(&pi);
                if (pe == ESP_ERR_ESPNOW_EXIST) pe = esp_now_mod_peer(&pi);
                if (pe == ESP_OK) {
                    _pair = PAIR_CONFIRM;
                    LOG_INFO("ESP-NOW pairing: candidate %02X:%02X:%02X:%02X:%02X:%02X, awaiting confirm",
                             _candMac[0],_candMac[1],_candMac[2],_candMac[3],_candMac[4],_candMac[5]);
                } else {
                    LOG_ERROR("ESP-NOW pairing: peer install failed (0x%x)", pe);
                }
            }
        }
    }

    // Process a captured confirming PROBE: persist, echo STATE a few times so the
    // sweeping dial catches one, then restart into begin().
    if (s_havePairProbe && _pair == PAIR_CONFIRM) {
        s_havePairProbe = false;
        if (!espnow_bond_save(_candMac, _candLmk)) {
            // Unbonded after reboot while the dial saved its half — report
            // failure instead of a false "paired" (red LED, no restart).
            _pairResult = "error";
            cancelPairing();
            return;
        }
        _pairResult = "paired";
        LOG_INFO("ESP-NOW pairing confirmed; saving bond, restart in 5s (green LED)");
        struct espnow_state_pkt p; buildState(&p);
        for (int i = 0; i < 8; i++) {
            esp_now_send(_candMac, (const uint8_t *)&p, sizeof(p));
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        _restartAtMs = uptime_ms() + 5000;  // let main.cpp show SLED_PAIR_OK for 5s
    }
}

void EspnowLink::loop() {
    pairLoop();
    if (!_bonded) return;

    // Surface a version-skewed peer instead of silently tolerating it. The gate
    // still accepts the peer (>= MIN_COMPAT, or PROBE/PAIR bypass), so this is a
    // heads-up that the Dial firmware and unit firmware are on different proto
    // versions and one side should be updated. Throttled to once a minute.
    if (s_peerVer != 0 && s_peerVer != ESPNOW_PROTO_VERSION) {
        uint32_t nowV = uptime_ms();
        if (_lastVerWarnMs == 0 || nowV - _lastVerWarnMs >= 60000) {
            LOG_WARN("ESP-NOW peer on proto v%u, unit on v%u (compat floor v%u) — update the %s",
                     s_peerVer, ESPNOW_PROTO_VERSION, ESPNOW_PROTO_MIN_COMPAT,
                     s_peerVer < ESPNOW_PROTO_VERSION ? "Dial" : "unit");
            _lastVerWarnMs = nowV;
        }
    }

    // Apply any decoded command on the main task (inherits anti-flicker logic).
    struct espnow_cmd_pkt c;
    bool haveCmd;
    portENTER_CRITICAL(&s_cmdMux);
    haveCmd = s_haveCmd;
    if (haveCmd) { c = s_cmd; s_haveCmd = false; }
    portEXIT_CRITICAL(&s_cmdMux);
    if (haveCmd) {
        if (c.mask & ESPNOW_CM_POWER)    _ctrl->setPower(c.power != 0);
        if (c.mask & ESPNOW_CM_MODE)     _ctrl->setMode(c.mode);
        if (c.mask & ESPNOW_CM_FAN)      _ctrl->setFanSpeed(c.fan);
        if (c.mask & ESPNOW_CM_TEMP)     _ctrl->setTargetTemp(espnow_dc_to_c(c.set_dc));
        if (c.mask & ESPNOW_CM_VANE)     _ctrl->setVane(c.vane);
        if (c.mask & ESPNOW_CM_WIDEVANE) _ctrl->setWideVane(c.wide_vane);
        if (c.mask & ESPNOW_CM_UNITS) {
            settings.get().useFahrenheit = (c.use_f != 0);
            settings.save();
            LOG_INFO("Dial set display units: %s", c.use_f ? "F" : "C");
        }
        buildState(&_lastState);
        esp_now_send(_peer, (const uint8_t*)&_lastState, sizeof(_lastState));
        _lastStateTxMs = uptime_ms();
    }

    // Link OTA: hand the bonded link our home Wi-Fi credentials so it can pull
    // its firmware manifest. Unicast to the encrypted peer (LMK on the air).
    if (s_haveWifiReq) {
        s_haveWifiReq = false;
        struct espnow_wifi_resp_pkt r; memset(&r, 0, sizeof(r));
        r.type = ESPNOW_PKT_WIFI_RESP; r.version = ESPNOW_PROTO_VERSION;
        if (WifiManager::loadCredentials((char*)r.ssid, sizeof(r.ssid),
                                         (char*)r.psk, sizeof(r.psk)))
            r.ok = 1;
        LOG_INFO("Link requested Wi-Fi credentials (%s)", r.ok ? "sent" : "none stored");
        esp_now_send(_peer, (const uint8_t*)&r, sizeof(r));
        memset(&r, 0, sizeof(r));   // don't leave the PSK sitting on the stack
    }

    // Dial-driven provisioning: serve a network scan. scanNetworks() blocks a
    // few seconds on the main task — acceptable: MAC-layer probe ACKs (the
    // dial's liveness signal) are hardware-level, STATE heartbeat is 10 s, and
    // the web UI's /wifi-scan already runs the same blocking scan. Results are
    // cached 20 s so a dial page-loss re-request re-sends instantly.
    if (s_haveScanReq) {
        s_haveScanReq = false;
        uint32_t nowScan = uptime_ms();
        if (_scanCount < 0 || nowScan - _scanAtMs > 20000) {
            _scanCount = WifiManager::scanNetworks(_scanCache, WIFI_SCAN_CACHE);
            _scanAtMs = uptime_ms();
            LOG_INFO("Dial requested Wi-Fi scan: %d networks", _scanCount);
        }
        int n = _scanCount < 0 ? 0 : _scanCount;
        int pages = (n + ESPNOW_WIFI_SCAN_MAX_ITEMS - 1) / ESPNOW_WIFI_SCAN_MAX_ITEMS;
        if (pages == 0) pages = 1;   // one empty page resolves the dial's spinner
        for (int pg = 0; pg < pages; pg++) {
            struct espnow_wifi_scan_resp_pkt r; memset(&r, 0, sizeof(r));
            r.type = ESPNOW_PKT_WIFI_SCAN_RESP; r.version = ESPNOW_PROTO_VERSION;
            r.page = (uint8_t)pg; r.n_pages = (uint8_t)pages;
            int base = pg * ESPNOW_WIFI_SCAN_MAX_ITEMS;
            int cnt = n - base;
            if (cnt > ESPNOW_WIFI_SCAN_MAX_ITEMS) cnt = ESPNOW_WIFI_SCAN_MAX_ITEMS;
            if (cnt < 0) cnt = 0;
            r.n_items = (uint8_t)cnt;
            for (int i = 0; i < cnt; i++) {
                strncpy((char*)r.items[i].ssid, _scanCache[base + i].ssid,
                        sizeof(r.items[i].ssid) - 1);
                r.items[i].rssi   = _scanCache[base + i].rssi;
                r.items[i].secure = _scanCache[base + i].secure ? 1 : 0;
            }
            esp_now_send(_peer, (const uint8_t*)&r,
                         ESPNOW_WIFI_SCAN_RESP_HDR_LEN +
                         cnt * (int)sizeof(struct espnow_wifi_scan_item));
            vTaskDelay(pdMS_TO_TICKS(10));   // spacing: don't overflow the TX queue
        }
    }

    // Dial-driven provisioning: apply pushed credentials. Mirrors
    // applyWifiCredentials() (web_server.cpp) minus the JSON layer.
    {
        bool haveSet = false;
        struct espnow_wifi_set_pkt w;
        portENTER_CRITICAL(&s_cmdMux);
        if (s_haveWifiSet) {
            memcpy(&w, (const void*)&s_wifiSet, sizeof(w));
            memset((void*)&s_wifiSet, 0, sizeof(s_wifiSet));   // no PSK in the static
            s_haveWifiSet = false; haveSet = true;
        }
        portEXIT_CRITICAL(&s_cmdMux);
        if (haveSet) {
            w.ssid[sizeof(w.ssid) - 1] = 0;
            w.psk[sizeof(w.psk) - 1] = 0;
            LOG_INFO("Dial provisioned Wi-Fi (SSID: %s)", (const char*)w.ssid);
            settings.get().wifiChangePending = true;   // shorter recovery window
            settings.save();
            WifiManager::clearDisconnectReason();
            WifiManager::connect((const char*)w.ssid, (const char*)w.psk);
            memset(&w, 0, sizeof(w));
            _wifiJoinStartMs   = uptime_ms();
            _wifiStatusUntilMs = _wifiJoinStartMs + 60000;   // 1 Hz status stream window
            _lastWifiStatusTxMs = 0;
            _wifiJoinResult = 0;
        }
    }

    // Status stream: 1 Hz for 60 s after a WIFI_SET. The join usually moves
    // the radio to the home AP's channel; the dial re-sweeps and catches the
    // stream on the new channel. Failures latch so a late-arriving dial still
    // hears the verdict.
    if (_wifiStatusUntilMs != 0) {
        uint32_t nowSt = uptime_ms();
        if ((int32_t)(nowSt - _wifiStatusUntilMs) >= 0) {
            _wifiStatusUntilMs = 0;
        } else if (nowSt - _lastWifiStatusTxMs >= 1000) {
            uint8_t st;
            if (WifiManager::isConnected()) {
                st = _wifiJoinResult = ESPNOW_WIFI_ST_CONNECTED;
            } else if (_wifiJoinResult >= ESPNOW_WIFI_ST_AUTH_FAIL) {
                st = _wifiJoinResult;                        // latched failure
            } else {
                uint8_t reason = WifiManager::lastDisconnectReason();
                if (reason == WIFI_REASON_NO_AP_FOUND)
                    st = _wifiJoinResult = ESPNOW_WIFI_ST_AP_NOT_FOUND;
                else if (reason == WIFI_REASON_AUTH_EXPIRE ||
                         reason == WIFI_REASON_AUTH_FAIL ||
                         reason == WIFI_REASON_MIC_FAILURE ||
                         reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                         reason == WIFI_REASON_HANDSHAKE_TIMEOUT)
                    st = _wifiJoinResult = ESPNOW_WIFI_ST_AUTH_FAIL;
                else if (nowSt - _wifiJoinStartMs > 30000)
                    st = _wifiJoinResult = ESPNOW_WIFI_ST_TIMEOUT;
                else
                    st = ESPNOW_WIFI_ST_CONNECTING;
            }
            struct espnow_wifi_status_pkt sp; memset(&sp, 0, sizeof(sp));
            sp.type = ESPNOW_PKT_WIFI_STATUS; sp.version = ESPNOW_PROTO_VERSION;
            sp.status = st;
            sp.rssi = WifiManager::getRSSI();
            WifiManager::getIP((char*)sp.ip, sizeof(sp.ip));
            esp_now_send(_peer, (const uint8_t*)&sp, sizeof(sp));
            _lastWifiStatusTxMs = nowSt;
        }
    }

    uint32_t now = uptime_ms();
    bool live = isPeerLive();
    if (!live) { _peerWasLive = false; return; }

    // Build current slim STATE; send if (a) the Dial just appeared (initial sync),
    // (b) a field changed and we're past the min-interval, or (c) the heartbeat is due.
    struct espnow_state_pkt p; buildState(&p);
    bool firstSinceLive = !_peerWasLive;
    _peerWasLive = true;
    bool changed = memcmp(&p, &_lastState, sizeof(p)) != 0;
    if (firstSinceLive ||
        (changed && now - _lastStateTxMs >= STATE_MIN_INTERVAL) ||
        (now - _lastStateTxMs >= STATE_HEARTBEAT_MS)) {
        esp_now_send(_peer, (const uint8_t*)&p, sizeof(p));
        _lastState = p; _lastStateTxMs = now;
    }

    // INFO is pull-only: emit while the Dial reports a cold screen, rate-limited.
    if (s_wantInfo && now - _lastInfoTxMs >= INFO_MIN_INTERVAL) {
        struct espnow_info_pkt info; buildInfo(&info);
        esp_now_send(_peer, (const uint8_t*)&info, sizeof(info));
        _lastInfoTxMs = now;
    }

    // DIAG rides the same pull gate as INFO (dial is on an info screen).
    if (s_wantInfo && now - _lastDiagTxMs >= INFO_MIN_INTERVAL) {
        struct espnow_diag_pkt d; buildDiag(&d);
        esp_now_send(_peer, (const uint8_t*)&d, sizeof(d));
        _lastDiagTxMs = now;
    }

    // IDENT is pull-only (v11): the dial asks while it lacks this unit's name
    // (zone picker). Same cadence gate as INFO.
    if (s_wantIdent && now - _lastIdentTxMs >= INFO_MIN_INTERVAL) {
        struct espnow_ident_pkt id; memset(&id, 0, sizeof(id));
        id.type = ESPNOW_PKT_IDENT; id.version = ESPNOW_PROTO_VERSION;
        strncpy((char *)id.name, settings.get().deviceName, sizeof(id.name) - 1);
        esp_now_send(_peer, (const uint8_t*)&id, sizeof(id));
        _lastIdentTxMs = now;
    }
}

static int cmd_mac(int, char **) {
    uint8_t m[6];
    esp_wifi_get_mac(WIFI_IF_STA, m);
    printf("ESPNOW-MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
           m[0],m[1],m[2],m[3],m[4],m[5]);
    return 0;
}

static int cmd_pair(int argc, char **argv) {
    if (argc != 3) { printf("usage: espnow-pair <AA:BB:CC:DD:EE:FF> <32-hex-lmk>\n"); return 1; }
    uint8_t mac[6], lmk[16];
    if (!espnow_parse_mac(argv[1], mac))   { printf("ERR bad mac\n");  return 1; }
    if (!espnow_parse_hex16(argv[2], lmk)) { printf("ERR bad lmk\n");  return 1; }
    if (!espnow_bond_save(mac, lmk)) { printf("ERR bond save failed\n"); return 1; }
    printf("ESPNOW-PAIR OK; restarting\n");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

static int cmd_pmk(int argc, char **argv) {
    if (argc != 2) { printf("usage: espnow-pmk <32-hex-pmk>\n"); return 1; }
    uint8_t pmk[16];
    if (!espnow_parse_hex16(argv[1], pmk)) { printf("ERR bad pmk\n"); return 1; }
    if (!espnow_pmk_save(pmk)) { printf("ERR pmk save failed\n"); return 1; }
    printf("ESPNOW-PMK OK (takes effect on next restart)\n");
    return 0;
}

static int cmd_selftest(int, char **) {
    uint8_t da[32], dA[32], ub[32], uB[32];
    int kr1 = espnow_crypto_keypair(da, dA);
    int kr2 = (kr1 == 0) ? espnow_crypto_keypair(ub, uB) : 0;
    if (kr1 != 0 || kr2 != 0) {
        int e = kr1 ? kr1 : kr2;
        printf("ESPNOW-SELFTEST FAIL keygen rc=%d (-0x%04X)\n", e, (unsigned)(-e));
        return 1;
    }
    uint8_t lk_d[16], lk_u[16];
    int dr1 = espnow_crypto_derive_lmk(da, uB, dA, uB, lk_d);   /* dial role */
    int dr2 = espnow_crypto_derive_lmk(ub, dA, dA, uB, lk_u);   /* unit role */
    if (dr1 != 0 || dr2 != 0) {
        int e = dr1 ? dr1 : dr2;
        printf("ESPNOW-SELFTEST FAIL derive rc=%d (-0x%04X)\n", e, (unsigned)(-e));
        return 1;
    }
    bool kdf = memcmp(lk_d, lk_u, 16) == 0;

    const uint8_t pmk[16] = {'S','E','L','F','T','E','S','T','p','m','k','0','0','0','0','0'};
    uint8_t msg[40]; for (int i = 0; i < 40; i++) msg[i] = (uint8_t)i;
    uint8_t t1[16], t2[16];
    espnow_crypto_auth_tag(pmk, 16, msg, sizeof(msg), t1);
    espnow_crypto_auth_tag(pmk, 16, msg, sizeof(msg), t2);
    bool tag = espnow_crypto_tag_ok(t1, t2);
    msg[0] ^= 1;
    espnow_crypto_auth_tag(pmk, 16, msg, sizeof(msg), t2);
    bool diff = !espnow_crypto_tag_ok(t1, t2);

    printf("ESPNOW-SELFTEST kdf=%d tag=%d diff=%d %s\n",
           kdf, tag, diff, (kdf && tag && diff) ? "PASS" : "FAIL");
    return (kdf && tag && diff) ? 0 : 1;
}

static bool s_console_started = false;

bool espnow_console_started(void) { return s_console_started; }

void espnow_register_console(void) {
    if (s_console_started) return;   // idempotent; single console owner
    esp_console_repl_t *repl = nullptr;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "serin>";
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t dc = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&dc, &rc, &repl) != ESP_OK) return;
#else
    esp_console_dev_uart_config_t dc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (esp_console_new_repl_uart(&dc, &rc, &repl) != ESP_OK) return;
#endif
    esp_console_cmd_t mac_cmd = {};
    mac_cmd.command = "espnow-mac";
    mac_cmd.help    = "Print STA MAC for ESP-NOW pairing";
    mac_cmd.func    = &cmd_mac;
    esp_console_cmd_register(&mac_cmd);

    esp_console_cmd_t pair_cmd = {};
    pair_cmd.command = "espnow-pair";
    pair_cmd.help    = "Bond a Dial: espnow-pair <mac> <lmk_hex>";
    pair_cmd.func    = &cmd_pair;
    esp_console_cmd_register(&pair_cmd);

    esp_console_cmd_t pmk_cmd = {};
    pmk_cmd.command = "espnow-pmk";
    pmk_cmd.help    = "Provision the ESP-NOW PMK: espnow-pmk <32-hex-pmk>";
    pmk_cmd.func    = &cmd_pmk;
    esp_console_cmd_register(&pmk_cmd);

    esp_console_cmd_t selftest_cmd = {};
    selftest_cmd.command = "espnow-selftest";
    selftest_cmd.help    = "Self-test pairing crypto (ECDH agreement + auth tag)";
    selftest_cmd.func    = &cmd_selftest;
    esp_console_cmd_register(&selftest_cmd);
    esp_console_start_repl(repl);
    s_console_started = true;
}

#else  // ESPNOW_REMOTE_ENABLE == 0
EspnowLink espnowLink;
void EspnowLink::begin(CN105Controller *) {}
void EspnowLink::loop() {}
bool EspnowLink::isPeerLive() const { return false; }
void EspnowLink::getPeerMac(uint8_t out[6]) const { for (int i=0;i<6;i++) out[i]=0; }
void EspnowLink::startPairing() {}
void EspnowLink::cancelPairing() {}
bool EspnowLink::pairingActive() const { return false; }
int  EspnowLink::pairingSecondsLeft() const { return 0; }
const char *EspnowLink::pairResult() const { return "idle"; }
void EspnowLink::onPairRecv(const uint8_t *, const uint8_t *, int) {}
void espnow_register_console(void) {}
bool espnow_console_started(void) { return false; }
#endif
