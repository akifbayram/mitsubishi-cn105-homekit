// Host tests for HomeKit service visibility (hk_service_slots.h): which
// services a boot builds, and the fixed IID slot each optional service owns.
// The SDK model below mirrors esp_hap_acc.c: hap_acc_add_serv() numbers the
// service, then each of its characteristics, from one per-accessory counter.
#include "hk_service_slots.h"
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

// Heat Pump accessory before the optional services: Accessory Information
// (service + Identify, Manufacturer, Model, Name, SerialNumber,
// FirmwareRevision), then the Thermostat (service + 5 mandatory chars,
// StatusFault, the two thresholds in Range only, StatusActive, Name,
// ConfiguredName).
static constexpr int ACC_INFO_IIDS = 7;
static int thermostat_iids(bool single) { return 1 + 5 + 1 + (single ? 0 : 2) + 3; }

struct Layout { int32_t thermostatLast; int32_t first[HK_SLOT_COUNT]; bool built[HK_SLOT_COUNT]; };

// What the SDK assigns with no pinning: every built service takes the next IIDs.
static Layout sequential(bool single, uint8_t builtSvcs, bool vane) {
    Layout l{};
    int32_t next = 1 + ACC_INFO_IIDS;
    next += thermostat_iids(single);
    l.thermostatLast = next - 1;
    for (int s = 0; s < HK_SLOT_COUNT; ++s) {
        l.built[s] = s == HK_SLOT_VANE_PRESET ? vane : (builtSvcs & (1 << s)) != 0;
        l.first[s] = 0;
        if (!l.built[s]) continue;
        l.first[s] = next;
        next += HK_SLOT_SIZE[s];
    }
    return l;
}

// What homekit_services_create_all() pins.
static Layout pinned(bool single, uint8_t builtSvcs, bool vane) {
    Layout l = sequential(single, builtSvcs, vane);
    for (int s = 0; s < HK_SLOT_COUNT; ++s)
        l.first[s] = l.built[s] ? hk_slot_first_iid(l.thermostatLast, HkSlot(s)) : 0;
    return l;
}

int main() {
    // ── Service bits: slot order for the four that can be hidden ────────
    CHECK(HK_SVC_FAN == 1 << HK_SLOT_FAN);
    CHECK(HK_SVC_FAN_AUTO == 1 << HK_SLOT_FAN_AUTO);
    CHECK(HK_SVC_FAN_MODE == 1 << HK_SLOT_FAN_MODE);
    CHECK(HK_SVC_DRY_MODE == 1 << HK_SLOT_DRY_MODE);
    CHECK(HK_SVC_ALL == (HK_SVC_FAN | HK_SVC_FAN_AUTO | HK_SVC_FAN_MODE | HK_SVC_DRY_MODE));

    // ── Built set: the capability mask gates, hiding only removes ───────
    CHECK(hk_services_available(MODE_CAP_ALL) == HK_SVC_ALL);
    CHECK(hk_services_built(0, MODE_CAP_ALL) == HK_SVC_ALL);
    CHECK(hk_services_available(MODE_CAP_ALL & ~MODE_CAP_FAN) == (HK_SVC_ALL & ~HK_SVC_FAN_MODE));
    CHECK(hk_services_available(MODE_CAP_ALL & ~MODE_CAP_DRY) == (HK_SVC_ALL & ~HK_SVC_DRY_MODE));
    for (int hidden = 0; hidden <= 0xFF; ++hidden)
        for (int mask = 0; mask <= MODE_CAP_ALL; ++mask) {
            const uint8_t built = hk_services_built(uint8_t(hidden), uint8_t(mask));
            CHECK((built & hidden) == 0);
            CHECK((built & ~hk_services_available(uint8_t(mask))) == 0);
            CHECK((built | (hidden & hk_services_available(uint8_t(mask)))) == hk_services_available(uint8_t(mask)));
        }

    // ── Today's numbers, from the code ──────────────────────────────────
    const int32_t rangeFirst[HK_SLOT_COUNT]  = {20, 26, 31, 36, 41};
    const int32_t singleFirst[HK_SLOT_COUNT] = {18, 24, 29, 34, 39};
    for (bool single : {false, true}) {
        const Layout full = sequential(single, HK_SVC_ALL, true);
        CHECK(full.thermostatLast == (single ? 17 : 19));
        for (int s = 0; s < HK_SLOT_COUNT; ++s) {
            CHECK(full.first[s] == (single ? singleFirst : rangeFirst)[s]);
            // Full layout: the slots are exactly the SDK's sequential numbering.
            CHECK(hk_slot_first_iid(full.thermostatLast, HkSlot(s)) == full.first[s]);
        }
    }

    // ── Every combination: removed × FAN/DRY capability × Vane × Range/Single
    for (bool single : {false, true})
        for (bool fanCap : {false, true})
            for (bool dryCap : {false, true})
                for (bool vane : {false, true})
                    for (int hidden = 0; hidden <= HK_SVC_ALL; ++hidden) {
                        const uint8_t mask = uint8_t(MODE_CAP_HEAT | MODE_CAP_COOL | MODE_CAP_AUTO |
                                                     (fanCap ? MODE_CAP_FAN : 0) | (dryCap ? MODE_CAP_DRY : 0));
                        const uint8_t built = hk_services_built(uint8_t(hidden), mask);
                        const Layout full = sequential(single, HK_SVC_ALL, true);
                        const Layout pin = pinned(single, built, vane);
                        std::set<int32_t> used;
                        for (int s = 0; s < HK_SLOT_COUNT; ++s) {
                            if (!pin.built[s]) continue;
                            // Each built service keeps the IIDs it has with everything built.
                            CHECK(pin.first[s] == full.first[s]);
                            for (int i = 0; i < HK_SLOT_SIZE[s]; ++i) {
                                const int32_t iid = pin.first[s] + i;
                                CHECK(iid > pin.thermostatLast);
                                CHECK(used.insert(iid).second);   // no two built services share an IID
                            }
                        }
                        // Nothing removed: the pinned layout differs from what
                        // the SDK assigned before this feature only where a
                        // mask-gated switch was missing ahead of a built one.
                        if (hidden == 0) {
                            const Layout before = sequential(single, built, vane);
                            bool same = true;
                            for (int s = 0; s < HK_SLOT_COUNT; ++s) same &= before.first[s] == pin.first[s];
                            const bool moves = (!fanCap && (dryCap || vane)) || (!dryCap && vane);
                            CHECK(same == !moves);
                        }
                    }

    puts("hk_service_slots: all passed");
    return 0;
}
