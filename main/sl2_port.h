/*
 * sl2_port.h — platform port for the Serin Link core.
 *
 * The core assumes only: a datagram transport with ≤250 B MTU and 6-byte peer
 * addresses (ESP-NOW today), a millisecond clock, and a small key/value store.
 * Reference port: esp_now_* + esp_timer + nvs_* (see esphome/ and the
 * mitsubishi-cn105-homekit adapter).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SL2_MTU 250
#define SL2_BCAST_MAC ((const uint8_t *)"\xff\xff\xff\xff\xff\xff")

typedef struct sl2_port {
    void *ctx;
    /* transport */
    bool (*send)(void *ctx, const uint8_t mac[6], const void *buf, size_t len);
    bool (*peer_add)(void *ctx, const uint8_t mac[6],
                     const uint8_t lmk[16], bool encrypt);   /* lmk NULL if !encrypt */
    void (*peer_del)(void *ctx, const uint8_t mac[6]);
    bool (*own_mac)(void *ctx, uint8_t out[6]);
    /* current radio channel (1..13), 0 if unknown. Optional: NULL ok.
     * Advertised in PAIR_RESP so the dial retunes off a bled broadcast. */
    uint8_t (*get_channel)(void *ctx);
    /* time */
    uint32_t (*now_ms)(void *ctx);           /* monotonic; wraps ok (u32 math) */
    /* storage: false from kv_get = absent; *len in = cap, out = actual */
    bool (*kv_get)(void *ctx, const char *key, void *buf, size_t *len);
    bool (*kv_set)(void *ctx, const char *key, const void *buf, size_t len);
    /* optional: NULL ok. level: 0 err, 1 warn, 2 info, 3 debug */
    void (*log)(void *ctx, int level, const char *msg);
} sl2_port_t;

#ifdef __cplusplus
}
#endif
