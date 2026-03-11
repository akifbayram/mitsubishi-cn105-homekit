#pragma once

#include "cn105_protocol.h"
#include <cstring>

// ════════════════════════════════════════════════════════════════════════════
// CN105 Protocol enum <-> string conversions
// Single source of truth — used by protocol logging and web JSON output
// ════════════════════════════════════════════════════════════════════════════

// ── Mode ────────────────────────────────────────────────────────────────────

inline const char* modeToLogStr(uint8_t mode) {
    switch (mode) {
        case CN105_MODE_HEAT: return "HEAT";
        case CN105_MODE_DRY:  return "DRY";
        case CN105_MODE_COOL: return "COOL";
        case CN105_MODE_FAN:  return "FAN";
        case CN105_MODE_AUTO: return "AUTO";
        default:              return "UNKNOWN";
    }
}

inline const char* modeToWebStr(uint8_t mode) {
    switch (mode) {
        case CN105_MODE_HEAT: return "heat";
        case CN105_MODE_DRY:  return "dry";
        case CN105_MODE_COOL: return "cool";
        case CN105_MODE_FAN:  return "fan";
        case CN105_MODE_AUTO: return "auto";
        default:              return "unknown";
    }
}

inline uint8_t strToMode(const char *s) {
    if (strcmp(s, "heat") == 0) return CN105_MODE_HEAT;
    if (strcmp(s, "dry")  == 0) return CN105_MODE_DRY;
    if (strcmp(s, "cool") == 0) return CN105_MODE_COOL;
    if (strcmp(s, "fan")  == 0) return CN105_MODE_FAN;
    if (strcmp(s, "auto") == 0) return CN105_MODE_AUTO;
    return CN105_MODE_AUTO;
}

// ── Fan Speed ───────────────────────────────────────────────────────────────

inline const char* fanToLogStr(uint8_t fan) {
    switch (fan) {
        case CN105_FAN_AUTO:  return "AUTO";
        case CN105_FAN_QUIET: return "QUIET";
        case CN105_FAN_1:     return "1";
        case CN105_FAN_2:     return "2";
        case CN105_FAN_3:     return "3";
        case CN105_FAN_4:     return "4";
        default:              return "UNKNOWN";
    }
}

inline const char* fanToWebStr(uint8_t fan) {
    switch (fan) {
        case CN105_FAN_AUTO:  return "auto";
        case CN105_FAN_QUIET: return "quiet";
        case CN105_FAN_1:     return "1";
        case CN105_FAN_2:     return "2";
        case CN105_FAN_3:     return "3";
        case CN105_FAN_4:     return "4";
        default:              return "unknown";
    }
}

inline uint8_t strToFan(const char *s) {
    if (strcmp(s, "auto")  == 0) return CN105_FAN_AUTO;
    if (strcmp(s, "quiet") == 0) return CN105_FAN_QUIET;
    if (strcmp(s, "1")     == 0) return CN105_FAN_1;
    if (strcmp(s, "2")     == 0) return CN105_FAN_2;
    if (strcmp(s, "3")     == 0) return CN105_FAN_3;
    if (strcmp(s, "4")     == 0) return CN105_FAN_4;
    return CN105_FAN_AUTO;
}

// ── Vane (Vertical) ─────────────────────────────────────────────────────────

inline const char* vaneToLogStr(uint8_t vane) {
    switch (vane) {
        case CN105_VANE_AUTO:  return "AUTO";
        case CN105_VANE_1:     return "1";
        case CN105_VANE_2:     return "2";
        case CN105_VANE_3:     return "3";
        case CN105_VANE_4:     return "4";
        case CN105_VANE_5:     return "5";
        case CN105_VANE_SWING: return "SWING";
        default:               return "UNKNOWN";
    }
}

inline const char* vaneToWebStr(uint8_t vane) {
    switch (vane) {
        case CN105_VANE_AUTO:  return "auto";
        case CN105_VANE_1:     return "1";
        case CN105_VANE_2:     return "2";
        case CN105_VANE_3:     return "3";
        case CN105_VANE_4:     return "4";
        case CN105_VANE_5:     return "5";
        case CN105_VANE_SWING: return "swing";
        default:               return "unknown";
    }
}

inline uint8_t strToVane(const char *s) {
    if (strcmp(s, "auto")  == 0) return CN105_VANE_AUTO;
    if (strcmp(s, "1")     == 0) return CN105_VANE_1;
    if (strcmp(s, "2")     == 0) return CN105_VANE_2;
    if (strcmp(s, "3")     == 0) return CN105_VANE_3;
    if (strcmp(s, "4")     == 0) return CN105_VANE_4;
    if (strcmp(s, "5")     == 0) return CN105_VANE_5;
    if (strcmp(s, "swing") == 0) return CN105_VANE_SWING;
    return CN105_VANE_AUTO;
}

// ── Wide Vane (Horizontal) ──────────────────────────────────────────────────

inline const char* wideVaneToLogStr(uint8_t wv) {
    switch (wv) {
        case CN105_WVANE_LEFT_LEFT:   return "<<";
        case CN105_WVANE_LEFT:        return "<";
        case CN105_WVANE_CENTER:      return "|";
        case CN105_WVANE_RIGHT:       return ">";
        case CN105_WVANE_RIGHT_RIGHT: return ">>";
        case CN105_WVANE_SPLIT:       return "<>";
        case CN105_WVANE_SWING:       return "SWING";
        default:                      return "?";
    }
}

inline const char* wideVaneToWebStr(uint8_t wv) {
    switch (wv) {
        case CN105_WVANE_LEFT_LEFT:   return "ll";
        case CN105_WVANE_LEFT:        return "l";
        case CN105_WVANE_CENTER:      return "c";
        case CN105_WVANE_RIGHT:       return "r";
        case CN105_WVANE_RIGHT_RIGHT: return "rr";
        case CN105_WVANE_SPLIT:       return "split";
        case CN105_WVANE_SWING:       return "swing";
        default:                      return "unknown";
    }
}

inline uint8_t strToWideVane(const char *s) {
    if (strcmp(s, "ll")    == 0) return CN105_WVANE_LEFT_LEFT;
    if (strcmp(s, "l")     == 0) return CN105_WVANE_LEFT;
    if (strcmp(s, "c")     == 0) return CN105_WVANE_CENTER;
    if (strcmp(s, "r")     == 0) return CN105_WVANE_RIGHT;
    if (strcmp(s, "rr")    == 0) return CN105_WVANE_RIGHT_RIGHT;
    if (strcmp(s, "split") == 0) return CN105_WVANE_SPLIT;
    if (strcmp(s, "swing") == 0) return CN105_WVANE_SWING;
    return CN105_WVANE_CENTER;
}

// ── Sub Mode (from 0x09 data[3]) ────────────────────────────────────────────

inline const char* subModeToLogStr(uint8_t sm) {
    switch (sm) {
        case 0x00: return "NORMAL";
        case 0x02: return "DEFROST";
        case 0x04: return "PREHEAT";
        case 0x08: return "STANDBY";
        default:   return "?";
    }
}

inline const char* subModeToWebStr(uint8_t sm) {
    switch (sm) {
        case 0x00: return "normal";
        case 0x02: return "defrost";
        case 0x04: return "preheat";
        case 0x08: return "standby";
        default:   return "unknown";
    }
}

// ── Stage (from 0x09 data[4]) ───────────────────────────────────────────────

inline const char* stageToLogStr(uint8_t st) {
    switch (st) {
        case 0x00: return "IDLE";
        case 0x01: return "LOW";
        case 0x02: return "GENTLE";
        case 0x03: return "MEDIUM";
        case 0x04: return "MODERATE";
        case 0x05: return "HIGH";
        case 0x06: return "DIFFUSE";
        default:   return "?";
    }
}

inline const char* stageToWebStr(uint8_t st) {
    switch (st) {
        case 0x00: return "idle";
        case 0x01: return "low";
        case 0x02: return "gentle";
        case 0x03: return "medium";
        case 0x04: return "moderate";
        case 0x05: return "high";
        case 0x06: return "diffuse";
        default:   return "unknown";
    }
}

// ── Auto Sub Mode (from 0x09 data[5]) ───────────────────────────────────────

inline const char* autoSubModeToLogStr(uint8_t asm_) {
    switch (asm_) {
        case 0x00: return "OFF";
        case 0x01: return "COOL";
        case 0x02: return "HEAT";
        case 0x03: return "LEADER";
        default:   return "?";
    }
}

inline const char* autoSubModeToWebStr(uint8_t asm_) {
    switch (asm_) {
        case 0x00: return "off";
        case 0x01: return "cool";
        case 0x02: return "heat";
        case 0x03: return "leader";
        default:   return "unknown";
    }
}
