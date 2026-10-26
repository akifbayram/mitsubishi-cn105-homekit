// Host test for BLE scan duty and the Add Sensor discovery session.
// Production functions come from main/ble_sensor.cpp via test_ble_discovery.py.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define CASE(name) if (!strcmp(argv[1], name))

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)

static uint32_t now_ms = 1000;
static uint32_t uptime_ms() { return now_ms; }

constexpr int ROOM_MAX_BLE_SENSORS = 4;
constexpr int ROOM_MEMBER_INTERNAL = 0, ROOM_MEMBER_LINK = 1, ROOM_MEMBER_BLE0 = 2;

// ── Settings ────────────────────────────────────────────────────────────────
struct BleSensorCfg { char addr[18] = {}; char name[24] = {}; };
struct DeviceSettings {
    int roomMode = 0, roomSingle = ROOM_MEMBER_INTERNAL;
    uint16_t roomStaleTimeoutS = 600;
    bool bleEnabled = true;
    BleSensorCfg bleSensors[ROOM_MAX_BLE_SENSORS];
};
struct Settings { DeviceSettings s; DeviceSettings& get() { return s; } void save() {} } settings;
static float room_apply_offset(const DeviceSettings&, int, float t) { return t; }

// ── Collaborators of BleSensor::loop ────────────────────────────────────────
struct CN105Controller {
    bool isConnected() { return false; }
    void sendRemoteTemperature(float) {}
    void clearRemoteTemperature() {}
    static float quantizeRemoteTemp(float t) { return t; }
};
namespace LinkSensor { bool isActive() { return false; } }
namespace RoomAvg { bool isFeeding() { return false; } }

// ── NimBLE ──────────────────────────────────────────────────────────────────
constexpr int BLE_OWN_ADDR_PUBLIC = 0, BLE_HS_FOREVER = -1;
struct ble_gap_event;
struct ble_gap_disc_params { uint8_t passive, filter_duplicates; uint16_t itvl, window; };
static ble_gap_disc_params last_params;
static int disc_starts = 0;
static int ble_gap_disc(int, int, const ble_gap_disc_params* p, int (*)(ble_gap_event*, void*), void*) {
    last_params = *p; ++disc_starts; return 0;
}
static int ble_gap_disc_cancel() { return 0; }
static int gap_event_cb(ble_gap_event*, void*) { return 0; }

#include "ble_header.inc"
#include "ble_state.inc"

static bool initNimble() { s_nimbleInitialized.store(true); return true; }
static void deinitNimble() { s_nimbleInitialized.store(false); }

namespace BleSensor {
int feedSlot();
void loop(CN105Controller&);
void setBleEnabled(bool);
void startDiscovery();
void stopDiscovery();
bool isDiscovering();
uint32_t discoveryElapsedMs();
bool pollDiscoveryComplete();
}

#include "ble_functions.inc"

static void configureSensor(int idx, const char* mac, bool fresh) {
    strcpy(settings.get().bleSensors[idx].addr, mac);
    SlotState& sl = s_slots[idx];
    sl.valid = true;
    strcpy(sl.mac, mac);
    sl.temp = fresh ? 21.0f : NAN;
    sl.lastUpdate = fresh ? now_ms : 0;
}

int main(int, char** argv) {
    CN105Controller cn105;
    s_bleEnabled.store(true);
    s_nimbleInitialized.store(true);

    // Sensors broadcast every ~10 s to several minutes. A 30 % listening
    // window lost two thirds of their readings on the bench, so the scan must
    // stay at the full window once every configured sensor is reporting.
    CASE("reporting_sensors_keep_full_listening_window") {
        configureSensor(0, "aa:bb:cc:dd:ee:ff", true);
        startScan();
        const int starts = disc_starts;
        BleSensor::loop(cn105);
        CHECK(last_params.window == 144 && last_params.itvl == 160);
        CHECK(disc_starts == starts);   // no stop/restart churn either
    }

    // H5074-family sensors took 16-24 s between readings at the median, so a
    // 10 s session caught them about one time in four.
    CASE("discovery_keeps_running_after_ten_seconds") {
        BleSensor::startDiscovery();
        now_ms += 60000;
        CHECK(!BleSensor::pollDiscoveryComplete());
        CHECK(BleSensor::isDiscovering());
    }

    CASE("discovery_ends_at_ten_minute_cap") {
        BleSensor::startDiscovery();
        now_ms += 10 * 60 * 1000 - 1;
        CHECK(!BleSensor::pollDiscoveryComplete());
        now_ms += 1;
        CHECK(BleSensor::pollDiscoveryComplete());
        CHECK(!BleSensor::isDiscovering());
        CHECK(!BleSensor::pollDiscoveryComplete());   // completion is reported once
    }

    // Cancel and pick both end the session on the next main-loop poll, which
    // reports completion once so every open browser resets its button.
    CASE("stop_request_ends_discovery_once") {
        BleSensor::startDiscovery();
        now_ms += 5000;
        BleSensor::stopDiscovery();
        CHECK(BleSensor::isDiscovering());   // the httpd task only queues it
        CHECK(BleSensor::pollDiscoveryComplete());
        CHECK(!BleSensor::isDiscovering());
        CHECK(!BleSensor::pollDiscoveryComplete());
    }

    // A stop that arrives with no session open (a second browser, a late
    // click) must not end the next session the moment it starts.
    CASE("stale_stop_does_not_end_next_discovery") {
        BleSensor::stopDiscovery();
        CHECK(!BleSensor::pollDiscoveryComplete());
        BleSensor::startDiscovery();
        CHECK(!BleSensor::pollDiscoveryComplete());
        CHECK(BleSensor::isDiscovering());
    }

    CASE("stopping_discovery_releases_radio_without_sensors") {
        BleSensor::startDiscovery();
        BleSensor::loop(cn105);              // drains the deferred scan start
        CHECK(s_scanning.load());
        BleSensor::stopDiscovery();
        CHECK(BleSensor::pollDiscoveryComplete());
        CHECK(!s_scanning.load());
    }

    CASE("stopping_discovery_keeps_tracking_configured_sensors") {
        configureSensor(0, "aa:bb:cc:dd:ee:ff", true);
        BleSensor::startDiscovery();
        BleSensor::loop(cn105);
        BleSensor::stopDiscovery();
        CHECK(BleSensor::pollDiscoveryComplete());
        CHECK(s_scanning.load());
    }

    // The web UI shows how long the scan has been looking, from the state push.
    CASE("elapsed_time_follows_the_session") {
        CHECK(BleSensor::discoveryElapsedMs() == 0);
        BleSensor::startDiscovery();
        now_ms += 42000;
        CHECK(BleSensor::discoveryElapsedMs() == 42000);
        BleSensor::stopDiscovery();
        BleSensor::pollDiscoveryComplete();
        CHECK(BleSensor::discoveryElapsedMs() == 0);
    }

    CASE("disabling_ble_ends_discovery") {
        BleSensor::startDiscovery();
        now_ms += 2000;
        BleSensor::setBleEnabled(false);
        CHECK(BleSensor::pollDiscoveryComplete());
        CHECK(!BleSensor::isDiscovering());
    }
    return 0;
}
