/*
 * espnow_link.cpp — Serin Link v2 adapter.
 *
 * The protocol/state machine lives in the vendored libserinlink core
 * (sl2_link.c, platform-free). This file provides its three contracts:
 *   port   — esp_now_* transport, uptime clock, NVS "serinlink" kv
 *   crypto — libsodium (Ed25519 identity/signatures, X25519, HKDF-SHA256)
 *   hvac   — CN105Controller bridge (semantic v2 model <-> CN105 bytes)
 * plus the v1-shaped EspnowLink facade main.cpp/web_ws/wifi_recovery use.
 *
 * Threading: the ESP-NOW recv callback (Wi-Fi task) only pushes raw frames
 * into an SPSC ring; loop() (main task) drains it into the core.
 */
#include "espnow_link.h"

#if ESPNOW_REMOTE_ENABLE

#include <cstring>
#include <cstdio>
#include <cmath>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_console.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sodium.h>

#include "sl2_link.h"
#include "sl2_rxq.h"
#include "esp_utils.h"
#include "logging.h"
#include "cn105_protocol.h"
#include "cn105_strings.h"
#include "wifi_manager.h"
#include "wifi_recovery.h"
#include "homekit_setup.h"
#include "settings.h"
#include "status_led.h"
#include "ble_config.h"
#ifdef BLE_ENABLE
#include "ble_sensor.h"
#endif

static const char *TAG = "espnow_link";

EspnowLink espnowLink;

/* One pairing-window definition; the console strings paste the same number. */
#define PAIR_WINDOW_S      60
#define SL2_STR2(x)        #x
#define SL2_STR(x)         SL2_STR2(x)
#define PAIR_WINDOW_S_STR  SL2_STR(PAIR_WINDOW_S)

static sl2_link_t       s_link;
static sl2_rxq_t        s_rxq;
static sl2_port_t       s_port;
static sl2_crypto_t     s_crypto;
static sl2_hvac_iface_t s_hvac;
static CN105Controller *s_ctrl = nullptr;
static bool             s_started = false;
// caps-change watch: dial re-pulls CAPS when the built CAPS content changes.
// Gated on the settings generation counter so the rebuild only runs after an
// actual save(), not every loop tick.
static uint32_t         s_capsGen = 0;

/* Cross-task mailbox. The sl2 core is single-context by contract (sl2_link.h):
 * only loop() — on the main task — may mutate s_link. The web (httpd) and
 * console (REPL) tasks request mutations through these flags instead of
 * calling into the core; loop() drains them next tick (<= one loop period,
 * imperceptible next to the pairing window). */
static volatile bool    s_reqPairStart     = false;
static volatile bool    s_reqPairCancel    = false;
static volatile bool    s_reqForgetAll     = false;
static volatile bool    s_reqForgetRestart = false;   // forget + LED + esp_restart()

/* Status snapshot, refreshed by loop() on the main task. The web (httpd) and
 * console tasks read this adapter-owned copy instead of calling into the core,
 * so the single-context contract holds for reads as well as writes (no
 * dependence on core internals being tear-safe across a re-vendor). */
struct LinkStatus {
    bool    bonded  = false;
    bool    live    = false;
    bool    pairing = false;
    int     secsLeft = 0;
    uint8_t mac0[6] = {};
    /* Interned literal from the core (sl2_link.c only ever assigns string
     * constants) — a single aligned pointer store is tear-free for the
     * httpd/console reader tasks, unlike a byte-wise buffer copy. */
    const char *result = "idle";
    EspnowPairOutcome outcome = ESPNOW_PAIR_NONE;
};
static LinkStatus s_stat;

/* ── port: transport / clock / kv ─────────────────────────────────────── */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    sl2_rxq_push(&s_rxq, info->src_addr, info->des_addr, data, len);
}

static bool p_send(void *, const uint8_t mac[6], const void *buf, size_t len) {
    return esp_now_send(mac, (const uint8_t *)buf, len) == ESP_OK;
}

static bool p_peer_add(void *, const uint8_t mac[6], const uint8_t lmk[16], bool encrypt) {
    esp_now_peer_info_t pi = {};
    memcpy(pi.peer_addr, mac, 6);
    pi.ifidx = WIFI_IF_STA;
    pi.channel = 0;                       // follow the STA channel
    pi.encrypt = encrypt;
    if (encrypt && lmk) memcpy(pi.lmk, lmk, 16);
    esp_err_t err = esp_now_add_peer(&pi);
    if (err == ESP_ERR_ESPNOW_EXIST) err = esp_now_mod_peer(&pi);
    if (err != ESP_OK) LOG_ERROR("sl2 peer_add rc=0x%x", (unsigned)err);
    return err == ESP_OK;
}

static void p_peer_del(void *, const uint8_t mac[6]) { esp_now_del_peer(mac); }

static bool p_own_mac(void *, uint8_t out[6]) {
    return esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK;
}

static uint8_t current_channel(void) {
    uint8_t ch = 0;
    wifi_second_chan_t sc;
    return esp_wifi_get_channel(&ch, &sc) == ESP_OK ? ch : 0;
}

static uint8_t p_channel(void *) { return current_channel(); }

static uint32_t p_now_ms(void *) { return uptime_ms(); }

static const char *SL2_NVS_NS = "serinlink";

static bool p_kv_get(void *, const char *key, void *buf, size_t *len) {
    nvs_handle_t h;
    if (nvs_open(SL2_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t err = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return err == ESP_OK;
}

static bool p_kv_set(void *, const char *key, const void *buf, size_t len) {
    nvs_handle_t h;
    if (nvs_open(SL2_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, key, buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void p_log(void *, int level, const char *msg) {
    switch (level) {
        case 0:  LOG_ERROR("%s", msg); break;
        case 1:  LOG_WARN("%s", msg);  break;
        case 2:  LOG_INFO("%s", msg);  break;
        default: LOG_DEBUG("%s", msg); break;
    }
}

/* ── crypto: libsodium ────────────────────────────────────────────────── */

static int c_rand(void *, uint8_t *buf, size_t len) {
    randombytes_buf(buf, len);
    return 0;
}
static int c_xkp(void *, uint8_t priv[32], uint8_t pub[32]) {
    randombytes_buf(priv, 32);
    return crypto_scalarmult_curve25519_base(pub, priv);
}
static int c_xsh(void *, const uint8_t priv[32], const uint8_t peer[32], uint8_t out[32]) {
    return crypto_scalarmult_curve25519(out, priv, peer);
}
static int c_ekp(void *, uint8_t priv[64], uint8_t pub[32]) {
    return crypto_sign_ed25519_keypair(pub, priv);
}
static int c_sign(void *, const uint8_t priv[64], const uint8_t *msg, size_t msg_len,
                  uint8_t sig[64]) {
    return crypto_sign_ed25519_detached(sig, nullptr, msg, msg_len, priv);
}
static int c_verify(void *, const uint8_t pub[32], const uint8_t *msg, size_t msg_len,
                    const uint8_t sig[64]) {
    return crypto_sign_ed25519_verify_detached(sig, msg, msg_len, pub);
}

/* ── hvac: CN105 <-> semantic v2 mapping ──────────────────────────────── */

static uint8_t mode_to_sl2(bool power, uint8_t cn) {
    if (!power) return SL2_MODE_OFF;
    switch (cn) {
        case CN105_MODE_HEAT: return SL2_MODE_HEAT;
        case CN105_MODE_DRY:  return SL2_MODE_DRY;
        case CN105_MODE_COOL: return SL2_MODE_COOL;
        case CN105_MODE_FAN:  return SL2_MODE_FAN_ONLY;
        case CN105_MODE_AUTO: return SL2_MODE_AUTO;
        default:              return SL2_MODE_AUTO;
    }
}

static uint8_t mode_from_sl2(uint8_t m) {
    switch (m) {
        case SL2_MODE_HEAT:     return CN105_MODE_HEAT;
        case SL2_MODE_DRY:      return CN105_MODE_DRY;
        case SL2_MODE_COOL:     return CN105_MODE_COOL;
        case SL2_MODE_FAN_ONLY: return CN105_MODE_FAN;
        default:                return CN105_MODE_AUTO;
    }
}

/* fan detents: step i of 5 <-> canonical percent round(i*100/5) */
static uint8_t fan_to_pct(uint8_t cn) {
    switch (cn) {
        case CN105_FAN_QUIET: return 20;
        case CN105_FAN_1:     return 40;
        case CN105_FAN_2:     return 60;
        case CN105_FAN_3:     return 80;
        case CN105_FAN_4:     return 100;
        default:              return SL2_FAN_AUTO;
    }
}

static uint8_t fan_from_pct(uint8_t pct) {
    if (pct == SL2_FAN_AUTO) return CN105_FAN_AUTO;
    int step = ((int)pct * 5 + 50) / 100;
    if (step < 1) step = 1;
    if (step > 5) step = 5;
    static const uint8_t tab[5] = { CN105_FAN_QUIET, CN105_FAN_1, CN105_FAN_2,
                                    CN105_FAN_3, CN105_FAN_4 };
    return tab[step - 1];
}

static uint8_t vanev_to_sl2(uint8_t cn) {
    if (cn == CN105_VANE_SWING) return SL2_VANE_SWING;
    if (cn >= CN105_VANE_1 && cn <= CN105_VANE_5) return cn;   // 1..5 match
    return SL2_VANE_AUTO;
}
static uint8_t vanev_from_sl2(uint8_t v) {
    if (v == SL2_VANE_SWING) return CN105_VANE_SWING;
    if (v >= 1 && v <= 5) return v;
    return CN105_VANE_AUTO;
}
/* horizontal axis: positions 1..5 match CN105, SPLIT (0x08) is advertised as
 * one extra sl2 position, swing = 0x0C. CN105 wide vane has no AUTO —
 * incoming 0 falls back to CENTER. */
static constexpr uint8_t SL2_WVANE_POS_SPLIT = 6;   // also the n_pos in CAPS
static uint8_t vaneh_to_sl2(uint8_t cn) {
    if (cn == CN105_WVANE_SWING) return SL2_VANE_SWING;
    if (cn == CN105_WVANE_SPLIT) return SL2_WVANE_POS_SPLIT;
    if (cn >= CN105_WVANE_LEFT_LEFT && cn <= CN105_WVANE_RIGHT_RIGHT) return cn;
    return CN105_WVANE_CENTER;   // center default (positions 1..5 map 1:1)
}
static uint8_t vaneh_from_sl2(uint8_t v) {
    if (v == SL2_VANE_SWING) return CN105_WVANE_SWING;
    if (v == SL2_WVANE_POS_SPLIT) return CN105_WVANE_SPLIT;
    if (v >= 1 && v <= 5) return v;
    return CN105_WVANE_CENTER;
}

static uint8_t action_of(const CN105State &st) {
    switch (st.subMode) {           // vendor sub-states outrank the operating flag
        case CN105_SUB_DEFROST: return SL2_ACT_DEFROST;
        case CN105_SUB_PREHEAT: return SL2_ACT_PREHEAT;
        case CN105_SUB_STANDBY: return SL2_ACT_STANDBY;
        default: break;
    }
    if (!st.power || !st.operating) return SL2_ACT_IDLE;
    switch (st.mode) {
        case CN105_MODE_HEAT: return SL2_ACT_HEATING;
        case CN105_MODE_COOL: return SL2_ACT_COOLING;
        case CN105_MODE_DRY:  return SL2_ACT_DRYING;
        case CN105_MODE_FAN:  return SL2_ACT_FAN;
        case CN105_MODE_AUTO:
            if (st.autoSubMode == CN105_AUTOSUB_COOL) return SL2_ACT_COOLING;
            if (st.autoSubMode == CN105_AUTOSUB_HEAT) return SL2_ACT_HEATING;
            return SL2_ACT_IDLE;
        default: return SL2_ACT_IDLE;
    }
}

static bool h_get_state(void *, sl2_hvac_state_t *out) {
    const CN105State st = s_ctrl->getEffectiveState();
    memset(out, 0, sizeof *out);
    out->hvac_link        = s_ctrl->isConnected();
    out->wifi             = WifiManager::isConnected();
    out->wifi_provisioned = WifiManager::hasCredentials();
    out->setup_ap         = wifiRecovery.isAPActive();
    out->use_f            = settings.get().useFahrenheit;
    out->mode   = mode_to_sl2(st.power, st.mode);
    out->action = action_of(st);
    out->fan    = fan_to_pct(st.fanSpeed);
    uint8_t vc = settings.get().vaneConfig;
    out->vane_v = (vc >= 1) ? vanev_to_sl2(st.vane) : 0;
    out->vane_h = (vc >= 2) ? vaneh_to_sl2(st.wideVane) : 0;
    out->preset = SL2_PRESET_NONE;
    out->fault  = (st.errorCode == 0x80) ? 0 : st.errorCode;
    out->room_dc = sl2_c_to_dc(st.roomTemp);
    out->set_dc  = sl2_c_to_dc(st.targetTemp);
    out->set_low_dc  = SL2_DC_NA;
    out->set_high_dc = SL2_DC_NA;
    out->room_hum_pct = SL2_HUM_NA;
    out->hum_set_pct  = SL2_HUM_NA;
#ifdef BLE_ENABLE
    /* Remote-sensor low-battery latch: ON at <=10%, clear only at >=15% so a
     * cell hovering at the threshold doesn't flap the home-face chip. */
    static bool s_battLatch = false;
    if (BleSensor::isEnabled() && BleSensor::isActive()) {
        int8_t b = BleSensor::battery();
        if (b >= 0) {
            if (b <= 10)      s_battLatch = true;
            else if (b >= 15) s_battLatch = false;
        }
        /* Forward remote-sensor humidity (%RH) to the dial. NAN = sensor has no
         * humidity channel, so leave room_hum_pct at SL2_HUM_NA. */
        float rh = BleSensor::humidity();
        if (!std::isnan(rh)) {
            if (rh < 0.0f)        rh = 0.0f;
            else if (rh > 100.0f) rh = 100.0f;
            out->room_hum_pct = (uint8_t)lroundf(rh);
        }
    } else {
        s_battLatch = false;
    }
    out->sensor_batt_low = s_battLatch;
#endif
    return true;
}

static bool h_apply(void *, uint16_t mask, const struct sl2_cmd_pkt *cmd) {
    if (mask & SL2_CM_MODE) {
        if (cmd->mode == SL2_MODE_OFF) {
            s_ctrl->setPower(false);
        } else {
            uint8_t cnMode = mode_from_sl2(cmd->mode);
            if (!mode_mask_allows(settings.get().modeMask, cnMode)) {
                // Dial with a stale CAPS cache (re-pull is in flight).
                LOG_WARN("Link CMD mode %u rejected — disabled by capability mask", cmd->mode);
            } else {
                s_ctrl->setPower(true);
                s_ctrl->setMode(cnMode);
            }
        }
    }
    if (mask & SL2_CM_TEMP)  s_ctrl->setTargetTemp(sl2_dc_to_c(cmd->set_dc));
    if (mask & SL2_CM_FAN)   s_ctrl->setFanSpeed(fan_from_pct(cmd->fan));
    if (mask & SL2_CM_VANEV) s_ctrl->setVane(vanev_from_sl2(cmd->vane_v));
    if (mask & SL2_CM_VANEH) s_ctrl->setWideVane(vaneh_from_sl2(cmd->vane_h));
    /* setX() only stages fields into the cross-task mailbox — nothing
     * reaches the heat pump until the batch is committed (threading
     * contract in cn105_protocol.h). Safe no-op when the mode above was
     * rejected and nothing got staged. */
    if (mask & (SL2_CM_MODE | SL2_CM_TEMP | SL2_CM_FAN | SL2_CM_VANEV | SL2_CM_VANEH))
        s_ctrl->sendPendingChanges();
    if (mask & SL2_CM_UNITS) {
        settings.get().useFahrenheit = (cmd->use_f != 0);
        settings.save();
        LOG_INFO("Dial set display units: %s", cmd->use_f ? "F" : "C");
    }
    /* TEMP_BAND / PRESET / HUM: not declared in this controller's CAPS. */
    return true;
}

static bool h_get_caps(void *, struct sl2_caps_pkt *out) {
    out->caps_flags = 0;
    uint8_t mm = settings.get().modeMask;
    uint16_t modes = (uint16_t)(1u << SL2_MODE_OFF);   // Off is always available
    // Compose the existing CN105->cap-bit and CN105->sl2 tables so this list
    // is the only place that enumerates the real modes.
    static constexpr uint8_t kCn105Modes[] = {
        CN105_MODE_HEAT, CN105_MODE_COOL, CN105_MODE_DRY, CN105_MODE_FAN, CN105_MODE_AUTO,
    };
    for (uint8_t m : kCn105Modes)
        if (mm & modeToCapBit(m)) modes |= (uint16_t)(1u << mode_to_sl2(true, m));
    out->modes = modes;
    out->presets = 0;
    out->fan_steps = 5;
    out->fan_flags = SL2_FAN_HAS_AUTO;
    uint8_t vc = settings.get().vaneConfig;
    out->vane_v = (vc >= 1) ? SL2_VANECAP(5, true, true) : 0;
    out->vane_h = (vc >= 2) ? SL2_VANECAP(SL2_WVANE_POS_SPLIT, false, true) : 0;
    out->set_min_dc = sl2_c_to_dc(CN105_TEMP_MIN);
    out->set_max_dc = sl2_c_to_dc(CN105_TEMP_MAX);
    out->set_step_dc = 5;                   // 0.5 °C, the CN105 enhanced-mode step
    out->ftab_id = 1;                       // Mitsubishi 61-88F table
    out->band_min_gap_dc = 0;
    out->hum_step_pct = 0;
    uint16_t feat = SL2_FEAT_WIFI_INFO | SL2_FEAT_HOMEKIT | SL2_FEAT_OUTSIDE_T |
                    SL2_FEAT_COMPRESSOR | SL2_FEAT_FW_INFO | SL2_FEAT_RUNTIME |
                    SL2_FEAT_LINK_OTA_CREDS | SL2_FEAT_WIFI_SETUP;
#ifdef BLE_ENABLE
    if (BleSensor::isBleEnabled() && BleSensor::isEnabled()) feat |= SL2_FEAT_SENSOR_BATT;
#endif
    out->features = feat;
    snprintf(out->name, sizeof out->name, "%s", settings.get().deviceName);
    return true;
}

/* NUL-joined string pair for variable TLVs; returns bytes or 0 if too big. */
static uint8_t tlv_strings(uint8_t *dst, size_t cap, const char *a, const char *b) {
    size_t la = strlen(a) + 1, lb = strlen(b) + 1;
    if (la + lb > cap || la + lb > 250) return 0;
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    return (uint8_t)(la + lb);
}

static size_t h_fill_info(void *, uint8_t *buf, size_t cap) {
    size_t off = 0;
    const CN105State st = s_ctrl->getEffectiveState();
    uint8_t tmp[128];

    {   /* WIFI_INFO: i8 rssi, u8 channel, ssid\0 ip\0 */
        char ssid[33] = "", ip[16] = "";
        WifiManager::getSSID(ssid, sizeof ssid);
        WifiManager::getIP(ip, sizeof ip);
        tmp[0] = (uint8_t)WifiManager::getRSSI();
        tmp[1] = current_channel();
        uint8_t n = tlv_strings(tmp + 2, sizeof tmp - 2, ssid, ip);
        if (n) sl2_tlv_put(buf, cap, &off, SL2_TLV_WIFI_INFO, tmp, (uint8_t)(2 + n));
    }
    {   /* HOMEKIT: u8 paired, code\0 payload\0 */
        const char *code = homekit_get_setup_code();
        const char *payload = homekit_get_setup_payload();
        tmp[0] = (uint8_t)homekit_get_controller_count();
        uint8_t n = tlv_strings(tmp + 1, sizeof tmp - 1,
                                code ? code : "", payload ? payload : "");
        if (n) sl2_tlv_put(buf, cap, &off, SL2_TLV_HOMEKIT, tmp, (uint8_t)(1 + n));
    }
    if (st.outsideTempValid) {
        int16_t dc = sl2_c_to_dc(st.outsideTemp);
        sl2_tlv_put(buf, cap, &off, SL2_TLV_OUTSIDE_T, &dc, 2);
    }
    {   /* COMPRESSOR: hz, stage, vendor sub_mode, vendor auto_sub */
        uint8_t c[4] = { st.compressorHz, st.stage, st.subMode, st.autoSubMode };
        sl2_tlv_put(buf, cap, &off, SL2_TLV_COMPRESSOR, c, 4);
    }
#ifdef BLE_ENABLE
    if (BleSensor::isEnabled() && BleSensor::isActive()) {
        int8_t b = BleSensor::battery();
        if (b >= 0) {
            uint8_t pct = (uint8_t)b;
            sl2_tlv_put(buf, cap, &off, SL2_TLV_SENSOR_BATT, &pct, 1);
        }
    }
#endif
    {   /* FW_INFO: version\0 build_date\0 */
        const esp_app_desc_t *app = esp_app_get_description();
        uint8_t n = tlv_strings(tmp, sizeof tmp, app->version, app->date);
        if (n) sl2_tlv_put(buf, cap, &off, SL2_TLV_FW_INFO, tmp, n);
    }
    if (st.runtimeValid) {
        uint32_t h = (uint32_t)st.runtimeHours;
        uint8_t b[4];
        memcpy(b, &h, 4);
        sl2_tlv_put(buf, cap, &off, SL2_TLV_RUNTIME, b, 4);
    }
    {   /* SYS: u32 uptime_s, u8 reset_reason */
        uint32_t up = uptime_ms() / 1000u;
        memcpy(tmp, &up, 4);
        tmp[4] = (uint8_t)esp_reset_reason();
        sl2_tlv_put(buf, cap, &off, SL2_TLV_SYS, tmp, 5);
    }
    return off;
}

static bool h_wifi_creds(void *, char ssid[33], char psk[65]) {
    return WifiManager::loadCredentials(ssid, 33, psk, 65);
}

/* Dial pressed "Change network": raise the recovery hotspot now. Runs on the
 * main task (espnowLink.loop() and wifiRecovery.loop() share it). */
static bool h_wifi_setup(void *) {
    LOG_INFO("Serin Link: dial requested the setup hotspot");
    wifiRecovery.beginChangeWindow();
    return true;
}

/* ── caps fingerprint ─────────────────────────────────────────────────── */

/* Bonded dials cache CAPS by caps_seq, which the core persists — but the
 * CONTENT can change under a stable seq (settings edits, a firmware update
 * declaring new features). Fingerprint the built packet instead of
 * enumerating its inputs here, so every future caps-affecting field is
 * covered automatically (same scheme as the core repo's ESPHome adapter). */
static const char *SL2_KV_CAPS_FP = "sl2_cfp";   /* adapter-owned, u32 */

static uint32_t caps_fingerprint(void) {
    struct sl2_caps_pkt cp;
    memset(&cp, 0, sizeof cp);
    h_get_caps(nullptr, &cp);
    return fnv1a32(&cp, sizeof cp);
}

/* Main task only (mutates s_link). NVS is read once (first call) and mirrored
 * in RAM after that, so the common "settings saved, CAPS unchanged" case is a
 * pure integer compare. */
static void caps_announce_if_changed(void) {
    static uint32_t s_lastFp  = 0;
    static bool     s_fpValid = false;
    if (!s_fpValid) {
        size_t len = sizeof s_lastFp;
        s_fpValid = p_kv_get(nullptr, SL2_KV_CAPS_FP, &s_lastFp, &len) &&
                    len == sizeof s_lastFp;
    }
    uint32_t fp = caps_fingerprint();
    if (s_fpValid && fp == s_lastFp) return;
    LOG_INFO("CAPS content changed (fp %08lx -> %08lx) — announcing",
             (unsigned long)(s_fpValid ? s_lastFp : 0), (unsigned long)fp);
    sl2_link_caps_changed(&s_link);
    p_kv_set(nullptr, SL2_KV_CAPS_FP, &fp, sizeof fp);
    s_lastFp = fp;
    s_fpValid = true;
}

/* ── EspnowLink facade ────────────────────────────────────────────────── */

static EspnowPairOutcome outcome_of(const char *r) {
    if (strcmp(r, "paired") == 0) return ESPNOW_PAIR_OK;
    if (strcmp(r, "timeout") == 0 || strcmp(r, "full") == 0 ||
        strcmp(r, "pin-mismatch") == 0) return ESPNOW_PAIR_FAIL;
    return ESPNOW_PAIR_NONE;   /* idle/listening/confirming/cancelled */
}

/* Main task only. */
static void snapshot_status(void) {
    s_stat.bonded   = sl2_link_dial_count(&s_link) > 0;
    s_stat.live     = sl2_link_any_live(&s_link);
    s_stat.pairing  = sl2_link_pairing(&s_link);
    s_stat.secsLeft = sl2_link_pair_seconds_left(&s_link);
    if (!sl2_link_dial_mac(&s_link, 0, s_stat.mac0)) memset(s_stat.mac0, 0, 6);
    const char *r = sl2_link_pair_result(&s_link);
    if (r != s_stat.result) {              /* interned — pointer compare works */
        s_stat.outcome = outcome_of(r);
        s_stat.result  = r;
    }
}

void EspnowLink::begin(CN105Controller *ctrl) {
    s_ctrl = ctrl;
    if (sodium_init() < 0) { LOG_ERROR("sodium_init failed"); return; }
    sl2_rxq_init(&s_rxq);

    if (esp_now_init() != ESP_OK) { LOG_ERROR("esp_now_init failed"); return; }
    if (esp_now_set_pmk((const uint8_t *)SL2_ESPNOW_PMK) != ESP_OK ||
        esp_now_register_recv_cb(on_recv) != ESP_OK) {
        LOG_ERROR("esp_now pmk/recv_cb setup failed");
        esp_now_deinit();
        return;
    }

    s_port = sl2_port_t{};
    s_port.send = p_send;         s_port.peer_add = p_peer_add;
    s_port.peer_del = p_peer_del; s_port.own_mac = p_own_mac;
    s_port.get_channel = p_channel;
    s_port.now_ms = p_now_ms;     s_port.kv_get = p_kv_get;
    s_port.kv_set = p_kv_set;     s_port.log = p_log;

    s_crypto = sl2_crypto_t{};
    s_crypto.rand_bytes = c_rand;
    s_crypto.x25519_keypair = c_xkp;   s_crypto.x25519_shared = c_xsh;
    s_crypto.ed25519_keypair = c_ekp;
    s_crypto.ed25519_sign = c_sign;    s_crypto.ed25519_verify = c_verify;

    s_hvac = sl2_hvac_iface_t{};
    s_hvac.get_state = h_get_state;
    s_hvac.apply = h_apply;
    s_hvac.get_caps = h_get_caps;
    s_hvac.fill_info_tlvs = h_fill_info;
    s_hvac.wifi_creds = h_wifi_creds;
    s_hvac.wifi_setup = h_wifi_setup;

    sl2_link_init(&s_link, &s_port, &s_crypto, &s_hvac);
    if (!sl2_link_start(&s_link)) { LOG_ERROR("sl2_link_start failed"); return; }
    s_started = true;

    s_capsGen = settings.generation();
    caps_announce_if_changed();
    snapshot_status();

    int n = sl2_link_dial_count(&s_link);
    if (n > 0) {
        uint8_t m[6];
        sl2_link_dial_mac(&s_link, 0, m);
        LOG_INFO("Serin Link v2: %d dial(s) bonded, first %02X:%02X:%02X:%02X:%02X:%02X",
                 n, m[0], m[1], m[2], m[3], m[4], m[5]);
    } else {
        LOG_INFO("Serin Link v2: no dial bonded, link idle");
    }
}

void EspnowLink::loop() {
    if (!s_started) return;

    /* Drain the cross-task mailbox first: the sl2 core is single-context by
     * contract (sl2_link.h), so mutations requested by the web (httpd) and
     * console (REPL) tasks execute here, in the link's owning task. If both
     * a start and a cancel land within one tick, start runs first and the
     * cancel wins — the same net result as the user's last click. */
    if (s_reqForgetAll)  { s_reqForgetAll = false;  sl2_link_forget_all(&s_link); }
    if (s_reqPairStart)  { s_reqPairStart = false;  sl2_link_pair_start(&s_link, PAIR_WINDOW_S * 1000); }
    if (s_reqPairCancel) { s_reqPairCancel = false; sl2_link_pair_cancel(&s_link); }
    if (s_reqForgetRestart) {
        s_reqForgetRestart = false;
        sl2_link_forget_all(&s_link);
#if PIN_LED_DATA >= 0
        /* Main task owns the strip: animate the blink inline. The blocking
         * hold also gives an httpd requester's reply time to flush. */
        statusLED.holdBlocking(SLED_UNPAIR, 2000);
#else
        vTaskDelay(pdMS_TO_TICKS(500));
#endif
        esp_restart();
    }

    /* rx drain stays per-tick so the SPSC ring can't back up; inbound CMDs
     * and pairing packets are fully handled inside sl2_link_on_recv(). */
    sl2_rxq_frame_t f;
    while (sl2_rxq_pop(&s_rxq, &f))
        sl2_link_on_recv(&s_link, f.src, f.dst, f.data, f.len);

    /* Periodic core work at ~20 Hz, not every ~10 ms tick: sl2_link_loop()
     * rebuilds the full STATE (hvac read + memcmp) each call while a dial is
     * live, to feed a TX path floored at SL2_STATE_MIN_INTERVAL_MS (250 ms) —
     * running it at the tick rate is ~10x wasted builds. Pair timeouts and
     * the status snapshot tolerate 50 ms granularity. */
    static uint32_t s_lastCore = 0;
    uint32_t now = uptime_ms();
    if (now - s_lastCore < 50) return;
    s_lastCore = now;

    sl2_link_loop(&s_link);

    /* Settings edits (web UI) that alter CAPS content bump caps_seq so dials
     * re-pull. The rebuild only runs after a settings save, not every tick. */
    if (s_capsGen != settings.generation()) {
        s_capsGen = settings.generation();
        caps_announce_if_changed();
    }

    snapshot_status();
}

bool EspnowLink::isBonded() const { return s_stat.bonded; }
bool EspnowLink::isPeerLive() const { return s_stat.live; }
void EspnowLink::getPeerMac(uint8_t out[6]) const { memcpy(out, s_stat.mac0, 6); }
void EspnowLink::startPairing() { if (s_started) s_reqPairStart = true; }
void EspnowLink::cancelPairing() { if (s_started) s_reqPairCancel = true; }
bool EspnowLink::pairingActive() const { return s_stat.pairing; }
int  EspnowLink::pairingSecondsLeft() const { return s_stat.secsLeft; }
const char *EspnowLink::pairResult() const { return s_stat.result; }
EspnowPairOutcome EspnowLink::pairOutcome() const { return s_stat.outcome; }

void espnow_forget_and_restart(void) {
    if (!s_started) esp_restart();   /* link never came up — nothing to forget */
    s_reqForgetRestart = true;       /* loop() runs forget + LED + esp_restart */
}

/* ── console ──────────────────────────────────────────────────────────── */

static int cmd_mac(int, char **) {
    uint8_t m[6];
    esp_wifi_get_mac(WIFI_IF_STA, m);
    printf("ESPNOW-MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
           m[0], m[1], m[2], m[3], m[4], m[5]);
    return 0;
}

static int cmd_pair(int argc, char **) {
    if (argc != 1) {
        printf("usage: espnow-pair            (opens a " PAIR_WINDOW_S_STR " s pairing window)\n");
        return 1;
    }
    espnowLink.startPairing();
    printf("ESPNOW-PAIR window open (" PAIR_WINDOW_S_STR " s); check: espnow-status\n");
    return 0;
}

static int cmd_forget(int, char **) {
    if (s_started) s_reqForgetAll = true;   /* executed by loop() on the main task */
    printf("ESPNOW-FORGET OK (all dials)\n");
    return 0;
}

static int cmd_status(int, char **) {
    printf("ESPNOW-STATUS bonded=%d live=%d pairing=%d left=%ds result=%s\n",
           (int)espnowLink.isBonded(), (int)espnowLink.isPeerLive(),
           (int)espnowLink.pairingActive(), espnowLink.pairingSecondsLeft(),
           espnowLink.pairResult());
    return 0;
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
    static const esp_console_cmd_t cmds[] = {
        { .command = "espnow-mac",    .help = "Print STA MAC",              .func = &cmd_mac },
        { .command = "espnow-pair",   .help = "Open a " PAIR_WINDOW_S_STR " s dial-pairing window",
          .func = &cmd_pair },
        { .command = "espnow-forget", .help = "Forget all bonded dials",    .func = &cmd_forget },
        { .command = "espnow-status", .help = "Show Serin Link status",     .func = &cmd_status },
    };
    for (const auto &c : cmds) esp_console_cmd_register(&c);
    esp_console_start_repl(repl);
    s_console_started = true;
}

#else  // ESPNOW_REMOTE_ENABLE == 0
#include <esp_system.h>
EspnowLink espnowLink;
void EspnowLink::begin(CN105Controller *) {}
void EspnowLink::loop() {}
bool EspnowLink::isBonded() const { return false; }
bool EspnowLink::isPeerLive() const { return false; }
void EspnowLink::getPeerMac(uint8_t out[6]) const { for (int i = 0; i < 6; i++) out[i] = 0; }
void EspnowLink::startPairing() {}
void EspnowLink::cancelPairing() {}
bool EspnowLink::pairingActive() const { return false; }
int  EspnowLink::pairingSecondsLeft() const { return 0; }
const char *EspnowLink::pairResult() const { return "idle"; }
EspnowPairOutcome EspnowLink::pairOutcome() const { return ESPNOW_PAIR_NONE; }
void espnow_register_console(void) {}
bool espnow_console_started(void) { return false; }
void espnow_forget_and_restart(void) { esp_restart(); }
#endif
