#pragma once
// Button-triggered BLE sensor pairing: a proximity-gated window that adds a
// sensor with no phone involved. Owns the window timer and the commit path;
// the accept rule itself is the pure PairPolicy in ble_pair_policy.h.
//
// Flow: triple-click -> 45 s window (cyan slow pulse) -> the user carries the
// sensor to the unit -> strongest-by-margin wins -> it is stored and becomes
// the heat pump's single-mode room source.

#include "ble_config.h"

namespace BlePair {
#ifdef BLE_ENABLE
    void begin();
    void loop();                 // main task, 1 Hz from BLE keepalive block
    void onTripleClick();        // main task, from the button dispatch
    bool isListening();          // drives SledInputs::blePairListening

    // NimBLE host task — called for every decodable advertisement while the
    // window is open.
    void observeAdvert(const char* mac, int rssi, bool tempDecoded,
                       const char* advName, const char* type);
#else
    inline void begin() {}
    inline void loop() {}
    inline void onTripleClick() {}
    inline bool isListening() { return false; }
    inline void observeAdvert(const char*, int, bool, const char*, const char*) {}
#endif
}
