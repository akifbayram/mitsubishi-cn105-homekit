#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Persistent ESP-NOW bond: the single peer MAC + per-pair LMK in NVS "espnow". */
bool espnow_bond_load(uint8_t mac[6], uint8_t lmk[16]);  /* true if valid bond present */
void espnow_bond_save(const uint8_t mac[6], const uint8_t lmk[16]);
void espnow_bond_clear(void);

#ifdef __cplusplus
}
#endif
