/* Serin Link — INFO TLV builder helpers (controller side).
 *
 * Typed wrappers over sl2_tlv_put for every core TLV a controller emits
 * (wire spec §9), plus the Mitsubishi value tables the COMPRESSOR TLV is
 * documented to carry. Multi-byte fields are written little-endian
 * explicitly, so the encoding is host-order-independent (unlike the packed
 * structs elsewhere, these helpers also run in host tests).
 *
 * All put_* helpers share sl2_tlv_put's contract: append whole-TLV or write
 * nothing — a truncated TLV would desync the receiver's parse of everything
 * after it.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sl2_proto.h"
#include "sl2_port.h"   /* SL2_MTU: natural sizing bound for INFO buffers */

#ifdef __cplusplus
extern "C" {
#endif

/* ENERGY (0x09) n/a sentinels: either half may be absent independently. */
#define SL2_INFO_W_NA  0xFFFFu
#define SL2_INFO_WH_NA 0xFFFFFFFFu

static inline void sl2_info__u16le(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)(v >> 8);
}

static inline void sl2_info__u32le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* NUL-joined string pair "a\0b\0". Returns total length, or 0 if the pair
 * doesn't fit dst (caller then skips the whole TLV). */
static inline uint8_t sl2_info__strpair(uint8_t *dst, size_t cap,
                                        const char *a, const char *b) {
    size_t la = strlen(a) + 1, lb = strlen(b) + 1;
    if (la + lb > cap) return 0;
    memcpy(dst, a, la);
    memcpy(dst + la, b, lb);
    return (uint8_t)(la + lb);
}

/* 0x01 WIFI_INFO: i8 rssi; u8 channel; str ssid; str ip */
static inline bool sl2_info_put_wifi(uint8_t *buf, size_t cap, size_t *off,
                                     int8_t rssi, uint8_t channel,
                                     const char *ssid, const char *ip) {
    uint8_t v[2 + 33 + 46];                  /* ssid<=32+NUL, ip fits IPv6 */
    v[0] = (uint8_t)rssi;
    v[1] = channel;
    uint8_t n = sl2_info__strpair(v + 2, sizeof v - 2, ssid, ip);
    if (n == 0) return false;
    return sl2_tlv_put(buf, cap, off, SL2_TLV_WIFI_INFO, v, (uint8_t)(2 + n));
}

/* 0x03 OUTSIDE_T: i16 outside_dc */
static inline bool sl2_info_put_outside_t(uint8_t *buf, size_t cap, size_t *off,
                                          int16_t outside_dc) {
    uint8_t v[2];
    sl2_info__u16le(v, (uint16_t)outside_dc);
    return sl2_tlv_put(buf, cap, off, SL2_TLV_OUTSIDE_T, v, 2);
}

/* 0x04 COMPRESSOR: u8 hz; u8 stage; u8 vendor_sub; u8 vendor_auto_sub */
static inline bool sl2_info_put_compressor(uint8_t *buf, size_t cap, size_t *off,
                                           uint8_t hz, uint8_t stage,
                                           uint8_t sub_mode, uint8_t auto_sub) {
    uint8_t v[4] = { hz, stage, sub_mode, auto_sub };
    return sl2_tlv_put(buf, cap, off, SL2_TLV_COMPRESSOR, v, 4);
}

/* 0x05 SENSOR_BATT: u8 pct */
static inline bool sl2_info_put_batt(uint8_t *buf, size_t cap, size_t *off,
                                     uint8_t pct) {
    return sl2_tlv_put(buf, cap, off, SL2_TLV_SENSOR_BATT, &pct, 1);
}

/* 0x06 FW_INFO: str version; str build_date */
static inline bool sl2_info_put_fw(uint8_t *buf, size_t cap, size_t *off,
                                   const char *version, const char *build_date) {
    uint8_t v[96];
    uint8_t n = sl2_info__strpair(v, sizeof v, version, build_date);
    if (n == 0) return false;
    return sl2_tlv_put(buf, cap, off, SL2_TLV_FW_INFO, v, n);
}

/* 0x07 RUNTIME: u32 hours */
static inline bool sl2_info_put_runtime(uint8_t *buf, size_t cap, size_t *off,
                                        uint32_t hours) {
    uint8_t v[4];
    sl2_info__u32le(v, hours);
    return sl2_tlv_put(buf, cap, off, SL2_TLV_RUNTIME, v, 4);
}

/* 0x08 SYS: u32 uptime_s; u8 reset_reason */
static inline bool sl2_info_put_sys(uint8_t *buf, size_t cap, size_t *off,
                                    uint32_t uptime_s, uint8_t reset_reason) {
    uint8_t v[5];
    sl2_info__u32le(v, uptime_s);
    v[4] = reset_reason;
    return sl2_tlv_put(buf, cap, off, SL2_TLV_SYS, v, 5);
}

/* 0x09 ENERGY: u16 input_w (SL2_INFO_W_NA); u32 wh_total (SL2_INFO_WH_NA) */
static inline bool sl2_info_put_energy(uint8_t *buf, size_t cap, size_t *off,
                                       uint16_t input_w, uint32_t wh_total) {
    uint8_t v[6];
    sl2_info__u16le(v, input_w);
    sl2_info__u32le(v + 2, wh_total);
    return sl2_tlv_put(buf, cap, off, SL2_TLV_ENERGY, v, 6);
}

/* ── COMPRESSOR value tables (Mitsubishi; wire spec §9) ──────────────────
 * Adapters whose HVAC layer reports these states as strings (e.g. ESPHome
 * cn105 text sensors) map them back to the wire codes here. ASCII
 * case-insensitive; unknown/NULL/empty map to 0 — the idle/normal floor,
 * which is also what a pump that reports nothing sends. */

static inline bool sl2_info__ieq(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - ('a' - 'A'));
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == *b;
}

static inline uint8_t sl2_info__code(const char *const *tab, uint8_t n,
                                     const char *s) {
    if (s != NULL)
        for (uint8_t i = 0; i < n; i++)
            if (sl2_info__ieq(tab[i], s)) return i;
    return 0;
}

/* stage: actual indoor-unit activity, byte 1 of the COMPRESSOR TLV */
static inline uint8_t sl2_info_stage_code(const char *s) {
    static const char *const tab[] = { "IDLE",     "LOW",  "GENTLE", "MEDIUM",
                                       "MODERATE", "HIGH", "DIFFUSE" };
    return sl2_info__code(tab, 7, s);
}

/* vendor_sub: NORMAL/DEFROST/PREHEAT/STANDBY, byte 2 */
static inline uint8_t sl2_info_sub_mode_code(const char *s) {
    static const char *const tab[] = { "NORMAL", "DEFROST", "PREHEAT",
                                       "STANDBY" };
    return sl2_info__code(tab, 4, s);
}

/* vendor_auto_sub: AUTO_OFF/AUTO_COOL/AUTO_HEAT/AUTO_LEADER, byte 3 */
static inline uint8_t sl2_info_auto_sub_code(const char *s) {
    static const char *const tab[] = { "AUTO_OFF", "AUTO_COOL", "AUTO_HEAT",
                                       "AUTO_LEADER" };
    return sl2_info__code(tab, 4, s);
}

#ifdef __cplusplus
}
#endif
