#include "espnow_bond.h"
#include <nvs_flash.h>
#include "logging.h"

static const char *TAG = "espnow_bond";
static const char *NS  = "espnow";

bool espnow_bond_load(uint8_t mac[6], uint8_t lmk[16]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t ml = 6, kl = 16;
    bool ok = nvs_get_blob(h, "peerMac", mac, &ml) == ESP_OK && ml == 6 &&
              nvs_get_blob(h, "lmk", lmk, &kl) == ESP_OK && kl == 16;
    nvs_close(h);
    return ok;
}

bool espnow_bond_save(const uint8_t mac[6], const uint8_t lmk[16]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { LOG_ERROR("bond nvs_open failed"); return false; }
    esp_err_t err = nvs_set_blob(h, "peerMac", mac, 6);
    if (err == ESP_OK) err = nvs_set_blob(h, "lmk", lmk, 16);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        /* A silent failure here means the unit reboots unbonded while the dial
         * saved its half — surface it so pairing reports failure, not OK. */
        LOG_ERROR("bond nvs write failed: %s", esp_err_to_name(err));
        return false;
    }
    LOG_INFO("ESP-NOW bond saved (%02X:%02X:%02X:%02X:%02X:%02X)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool espnow_pmk_load(uint8_t pmk[16]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t pl = 16;
    bool ok = nvs_get_blob(h, "pmk", pmk, &pl) == ESP_OK && pl == 16;
    nvs_close(h);
    return ok;
}

bool espnow_pmk_save(const uint8_t pmk[16]) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) { LOG_ERROR("pmk nvs_open failed"); return false; }
    esp_err_t set_err = nvs_set_blob(h, "pmk", pmk, 16);
    esp_err_t commit_err = (set_err == ESP_OK) ? nvs_commit(h) : set_err;
    if (commit_err != ESP_OK) {
        LOG_ERROR("pmk nvs write failed");
        nvs_close(h);
        return false;
    }
    nvs_close(h);
    LOG_INFO("ESP-NOW pmk saved");
    return true;
}

void espnow_bond_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "peerMac");   /* ESP_ERR_NVS_NOT_FOUND if absent — ignore */
    nvs_erase_key(h, "lmk");
    nvs_commit(h); nvs_close(h);
}
