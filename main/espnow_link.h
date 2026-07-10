#pragma once
#include <cstdint>
#include <cstddef>

// Serin Link protocol v2 (see main/sl2_proto.h + the libserinlink core in
// main/sl2_link.c). This header keeps the v1 EspnowLink surface so main.cpp,
// wifi_recovery.cpp and web_ws.cpp compile unchanged; the implementation is a
// thin adapter wiring the sl2 core to CN105Controller / WifiManager / HomeKit.

#ifndef ESPNOW_REMOTE_ENABLE
#define ESPNOW_REMOTE_ENABLE 1
#endif

class CN105Controller;

// Terminal pairing-window outcome, classified once in the adapter where the
// core's result string originates. NONE covers the non-terminal states and the
// deliberately-unindicated results (idle/listening/confirming/cancelled).
enum EspnowPairOutcome : uint8_t {
    ESPNOW_PAIR_NONE = 0,
    ESPNOW_PAIR_OK,        // "paired"
    ESPNOW_PAIR_FAIL,      // "timeout" / "full" / "pin-mismatch"
};

class EspnowLink {
public:
    void begin(CN105Controller *ctrl);   // call AFTER WiFi started + esp_now usable
    void loop();                         // call from main loop (~every 10ms ok)
    bool isBonded() const;               // >=1 dial in the bond table
    bool isPeerLive() const;             // any bonded dial probed recently
    void getPeerMac(uint8_t out[6]) const;   // first bonded dial (00.. if none)
    void startPairing();                 // 60 s signed-TOFU pairing window
    void cancelPairing();
    bool pairingActive() const;
    int  pairingSecondsLeft() const;
    const char *pairResult() const;      // idle/listening/confirming/paired/
                                         // timeout/full/pin-mismatch/cancelled
    EspnowPairOutcome pairOutcome() const;   // pairResult() classified for LED/UI
};

extern EspnowLink espnowLink;
void espnow_register_console(void);
// True once the ESP-NOW serial REPL has taken ownership of the USB-Serial-JTAG
// console. Used to keep Improv Serial and this REPL mutually exclusive (a single
// console owner) — see enableFallbackAP().
bool espnow_console_started(void);
// The full "forget remote" flow shared by the web and button paths: clear the
// bonds, show the SLED_UNPAIR blink, esp_restart(). Callable from any task —
// it lands in the cross-task mailbox and EspnowLink::loop() (main task, which
// owns both the sl2 core and the LED strip) runs the terminal sequence.
void espnow_forget_and_restart(void);
