/* Host tests for the room-source reconcile and catalog in main/espnow_link.cpp.
 * run.sh lifts the real functions out of the firmware source (extract.py) and
 * builds them against the fakes below, so what runs here is the shipped logic.
 *
 * The contract, from the controller room-source design: going offline never
 * changes the selected source — its health goes stale instead. v0.2.6 broke
 * that for Average: "fewer than two members available" was repaired by
 * rewriting the selection to Single + Internal, and availability is a runtime
 * fact that is false for every member on the first loop pass after boot and
 * for a user who taps Average before ticking any sensor. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "settings.h"
#include "sl2_link.h"

#define REQUIRE(test) do { if (!(test)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #test); std::abort(); } } while (0)

// ── Fakes for everything the lifted functions reach ─────────────────────────
static const char *TAG = "test";

SettingsStore settings;
static int saves = 0;
void SettingsStore::save() { saves++; }

static bool avgSelectable = false;
namespace RoomAvg { bool averageSelectable() { return avgSelectable; } }

static bool bleConfigured[ROOM_MAX_BLE_SENSORS];
namespace BleSensor {
bool isConfigured(int i) { return i >= 0 && i < ROOM_MAX_BLE_SENSORS && bleConfigured[i]; }
}

// The real one parses the address; any stable per-slot id serves the catalog.
uint64_t room_sensor_id_from_addr(const char *addr) {
    const uint8_t mac[6] = {0, 0, 0, 0, 0, (uint8_t)addr[0]};
    return sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_SENSOR, mac);
}

static sl2_link_t s_link;
static int        dialCount = 0;
static uint8_t    dialMacs[SL2_MAX_DIALS][6];
extern "C" int sl2_link_dial_count(const sl2_link_t *) { return dialCount; }
extern "C" bool sl2_link_dial_mac(const sl2_link_t *, int idx, uint8_t out[6]) {
    if (idx < 0 || idx >= dialCount) return false;
    memcpy(out, dialMacs[idx], 6);
    return true;
}

#include "room_source.inc"

// ── Helpers ─────────────────────────────────────────────────────────────────
static const uint8_t kDial[6] = {0x10, 0x51, 0xDB, 0x8E, 0xEA, 0xB0};
static const int     kCap     = 16;

static void reset() {
    settings.get() = DeviceSettings{};
    saves = 0;
    avgSelectable = false;
    memset(bleConfigured, 0, sizeof bleConfigured);
    dialCount = 0;
}

// The reconcile rate-limits itself to once a second off the clock it is
// handed, so every call here lands a full second after the last.
static void reconcile() {
    static uint32_t now = 0;
    now += 1000;
    room_source_reconcile_catalog(now);
}

static const sl2_room_source_entry *catalogEntry(const sl2_room_source_entry *a, int n,
                                                 uint64_t id) {
    for (int i = 0; i < n; i++)
        if (a[i].id == id) return &a[i];
    return nullptr;
}

// ── Reconcile ───────────────────────────────────────────────────────────────

// Boot: Average is stored with two BLE members, but BleSensor::begin() has not
// run yet and no dial has reported, so nothing counts as available.
static void test_average_survives_boot_before_any_member_is_available() {
    reset();
    auto &st = settings.get();
    st.roomMode     = 1;
    st.roomSingle   = ROOM_MEMBER_BLE0;
    st.roomMembers  = (1u << ROOM_MEMBER_BLE0) | (1u << (ROOM_MEMBER_BLE0 + 1));
    st.roomSourceId = SL2_ROOM_SOURCE_AVERAGE_ID;

    reconcile();
    reconcile();

    REQUIRE(st.roomMode == 1);
    REQUIRE(st.roomSingle == ROOM_MEMBER_BLE0);
    REQUIRE(saves == 0);
}

// Web UI: the Average button sends roomMode alone, and members can only be
// ticked from inside Average — so the user arrives with none ticked.
static void test_average_survives_being_entered_with_no_members_ticked() {
    reset();
    auto &st = settings.get();
    st.roomMode     = 1;
    st.roomSingle   = ROOM_MEMBER_BLE0;
    st.roomMembers  = 1u << ROOM_MEMBER_INTERNAL;
    st.roomSourceId = SL2_ROOM_SOURCE_AVERAGE_ID;
    bleConfigured[0] = bleConfigured[1] = true;

    reconcile();

    REQUIRE(st.roomMode == 1);
    REQUIRE(st.roomSingle == ROOM_MEMBER_BLE0);
    REQUIRE(saves == 0);
}

// The Link repair is the half of the reconcile that stays: a single-mode Link
// pick with no pin is pinned to the only bonded dial…
static void test_unpinned_link_pick_is_pinned_to_the_only_dial() {
    reset();
    auto &st = settings.get();
    st.roomSingle   = ROOM_MEMBER_LINK;
    st.roomSourceId = SL2_ROOM_SOURCE_INTERNAL_ID;
    dialCount = 1;
    memcpy(dialMacs[0], kDial, 6);

    reconcile();

    REQUIRE(st.roomSingle == ROOM_MEMBER_LINK);
    REQUIRE(st.roomSourceId == sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, kDial));
    REQUIRE(saves == 1);
}

// …and handed back to Internal when no dial is left to pin.
static void test_link_pick_with_no_dial_reverts_to_internal() {
    reset();
    auto &st = settings.get();
    st.roomSingle   = ROOM_MEMBER_LINK;
    st.roomSourceId = sl2_room_source_mac_id(SL2_ROOM_SOURCE_NS_LINK, kDial);

    reconcile();

    REQUIRE(st.roomMode == 0);
    REQUIRE(st.roomSingle == ROOM_MEMBER_INTERNAL);
    REQUIRE(saves == 1);
}

// ── Catalog ─────────────────────────────────────────────────────────────────

// A dial names the selection by looking its id up in the catalog, so the
// selected source has to be listed even when it could not be picked afresh.
static void test_selected_average_stays_listed_when_not_selectable() {
    reset();
    settings.get().roomMode = 1;

    sl2_room_source_entry a[kCap]{};
    int n = room_catalog_build(a, kCap);

    const sl2_room_source_entry *avg = catalogEntry(a, n, SL2_ROOM_SOURCE_AVERAGE_ID);
    REQUIRE(avg != nullptr);
    REQUIRE(!(avg->flags & SL2_ROOM_SOURCE_F_SELECTABLE));
}

static void test_selectable_average_is_offered() {
    reset();
    avgSelectable = true;

    sl2_room_source_entry a[kCap]{};
    int n = room_catalog_build(a, kCap);

    const sl2_room_source_entry *avg = catalogEntry(a, n, SL2_ROOM_SOURCE_AVERAGE_ID);
    REQUIRE(avg != nullptr);
    REQUIRE(avg->flags & SL2_ROOM_SOURCE_F_SELECTABLE);
}

static void test_average_is_not_listed_when_neither_selected_nor_selectable() {
    reset();

    sl2_room_source_entry a[kCap]{};
    int n = room_catalog_build(a, kCap);

    REQUIRE(catalogEntry(a, n, SL2_ROOM_SOURCE_AVERAGE_ID) == nullptr);
    REQUIRE(catalogEntry(a, n, SL2_ROOM_SOURCE_INTERNAL_ID) != nullptr);
}

int main() {
    test_average_survives_boot_before_any_member_is_available();
    test_average_survives_being_entered_with_no_members_ticked();
    test_unpinned_link_pick_is_pinned_to_the_only_dial();
    test_link_pick_with_no_dial_reverts_to_internal();
    test_selected_average_stays_listed_when_not_selectable();
    test_selectable_average_is_offered();
    test_average_is_not_listed_when_neither_selected_nor_selectable();
    std::printf("room_source_reconcile: all tests passed\n");
    return 0;
}
