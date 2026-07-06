#pragma once
#include "ble_config.h"   // brings nothing harmful; keeps include order consistent
#include <cstdint>
#include "espnow_proto.h"

#ifndef ESPNOW_REMOTE_ENABLE
#define ESPNOW_REMOTE_ENABLE 1
#endif

class CN105Controller;

class EspnowLink {
public:
    void begin(CN105Controller *ctrl);   // call AFTER WiFi started + esp_now usable
    void loop();                         // call from main loop (~every 100ms ok)
    bool isBonded()  const { return _bonded; }
    bool isPeerLive() const;             // heard a PROBE from the Dial recently
    void getPeerMac(uint8_t out[6]) const;
    void startPairing();
    void cancelPairing();
    bool pairingActive() const;
    int  pairingSecondsLeft() const;
    const char *pairResult() const;
    // Called from the static recv callback; not intended as external API.
    void onPairRecv(const uint8_t src[6], const uint8_t *data, int len);
private:
    enum PairState { PAIR_OFF, PAIR_LISTEN, PAIR_CONFIRM };
    void ensureEspnowInit();
    void pairLoop();
    void buildState(struct espnow_state_pkt *p);
    void buildInfo(struct espnow_info_pkt *p);
    void buildDiag(struct espnow_diag_pkt *p);
    CN105Controller *_ctrl = nullptr;
    bool     _bonded = false;
    uint8_t  _peer[6] = {0};
    uint32_t _lastStateTxMs = 0;
    uint32_t _lastInfoTxMs = 0;
    uint32_t _lastDiagTxMs = 0;
    uint32_t _lastVerWarnMs = 0;   // throttle for the proto-version-skew warning
    struct espnow_state_pkt _lastState{};
    bool     _peerWasLive = false;
    PairState _pair = PAIR_OFF;
    bool      _espnowReady = false;
    uint32_t  _pairStartMs = 0;
    uint32_t  _restartAtMs = 0;   // 0 = no pending restart; else uptime_ms() deadline
    uint8_t   _candMac[6] = {0};
    uint8_t   _candLmk[16] = {0};
    uint8_t   _ownPriv[32] = {0};
    uint8_t   _ownPub[32] = {0};
    const char *_pairResult = "idle";
};

extern EspnowLink espnowLink;
void espnow_register_console(void);
// True once the ESP-NOW serial REPL has taken ownership of the USB-Serial-JTAG
// console. Used to keep Improv Serial and this REPL mutually exclusive (a single
// console owner) — see enableFallbackAP().
bool espnow_console_started(void);
