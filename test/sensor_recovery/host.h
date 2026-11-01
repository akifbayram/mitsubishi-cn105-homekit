#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <strings.h>
#include "sl2_proto.h"
#include "sl2_bond.h"
#include "room_feed.h"
#include "link_units.h"
#include "ble_decoders.h"

using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define CASE(name) if (!strcmp(argv[1], name))

static uint64_t now_ms = 1000;
static int64_t esp_timer_get_time() { return (int64_t)now_ms * 1000; }
static void once(std::function<void()>& callback) {
    auto run = callback;
    callback = nullptr;
    if (run) run();
}
constexpr int ROOM_MAX_BLE_SENSORS = 4, ROOM_MEMBER_INTERNAL = 0,
              ROOM_MEMBER_LINK = 1, ROOM_MEMBER_BLE0 = 2,
              ROOM_MEMBER_COUNT = ROOM_MEMBER_BLE0 + ROOM_MAX_BLE_SENSORS;
struct BleSensorCfg { char addr[18] = {}; char name[24] = {}; };
struct DeviceSettings {
    bool bleEnabled = true;
    int roomMode = 0, roomSingle = ROOM_MEMBER_BLE0, roomStaleTimeoutS = 600;
    uint64_t roomSourceId = 0;
    uint8_t roomMembers = 0;
    int roomOffsets[ROOM_MEMBER_COUNT] = {};
    BleSensorCfg bleSensors[ROOM_MAX_BLE_SENSORS];
};
struct Settings { DeviceSettings s; DeviceSettings& get() { return s; } void save() {} } settings;
static bool mac_from_str(const char* text, uint8_t out[6]) {
    unsigned b[6];
    if (strlen(text) != 17 || sscanf(text, "%x:%x:%x:%x:%x:%x", b, b+1, b+2, b+3, b+4, b+5) != 6)
        return false;
    for (int i = 0; i < 6; i++) { if (b[i] > 255) return false; out[i] = b[i]; }
    return true;
}
static int room_offset_dc(const DeviceSettings& s, int member) { return s.roomOffsets[member]; }
static float room_apply_offset(const DeviceSettings& s, int member, float t) {
    return t + s.roomOffsets[member] / 10.0f;
}

// These calls are the production main task's CN105 mailbox boundaries. The
// hooks execute an httpd setter at the same unlocked boundary deterministically.
class CN105Controller {
public:
    bool connected = true;
    float temp = NAN;
    int sends = 0, clears = 0;
    std::function<void()> atConnected, atSend, atClear;
    bool isConnected() { once(atConnected); return connected; }
    void sendRemoteTemperature(float t) { once(atSend); temp = t; sends++; }
    void clearRemoteTemperature() { once(atClear); temp = NAN; clears++; }
    static float quantizeRemoteTemp(float t) { return std::clamp(roundf(t * 2) / 2, 0.5f, 63.5f); }
};

using esp_err_t = int;
constexpr int ESP_OK = 0, BLE_GAP_EVENT_DISC = 1, BLE_GAP_EVENT_DISC_COMPLETE = 2,
              BLE_OWN_ADDR_PUBLIC = 0, BLE_HS_FOREVER = -1;
struct ble_gap_disc_params { int passive, filter_duplicates, itvl, window; };
struct ble_gap_disc_desc {
    struct { uint8_t val[6]; } addr;
    uint8_t* data;
    uint8_t length_data;
    int rssi;
};
struct ble_gap_event { int type; ble_gap_disc_desc disc; };
struct { void (*sync_cb)() = nullptr; void (*reset_cb)(int) = nullptr; } ble_hs_cfg;
static bool host_ready = true, radio_scanning = false;
static int scan_attempts = 0, scan_failures = 0, init_attempts = 0, init_failures = 0;
static int stop_attempts = 0, stop_failures = 0;
static int cancel_attempts = 0, cancel_failures = 0, radio_window = 0;
static int (*radio_callback)(ble_gap_event*, void*) = nullptr;
static int ble_hs_synced() { return host_ready; }
static int ble_gap_disc(int, int, ble_gap_disc_params* params, int (*callback)(ble_gap_event*, void*), void*) {
    scan_attempts++;
    if (!host_ready) return 22;
    if (scan_failures) { scan_failures--; return 22; }
    if (radio_scanning) return 2;
    radio_callback = callback;
    radio_scanning = true;
    radio_window = params->window;
    return 0;
}
static void scan_complete() {
    radio_scanning = false;
    if (radio_callback) { ble_gap_event event = {}; event.type = BLE_GAP_EVENT_DISC_COMPLETE; radio_callback(&event, nullptr); }
}
static int ble_gap_disc_active() { return radio_scanning; }
static int ble_gap_disc_cancel() {
    cancel_attempts++;
    if (cancel_failures) { cancel_failures--; return 22; }
    radio_scanning = false; return 0;
}
static int nimble_port_init() { init_attempts++; if (init_failures) { init_failures--; return 1; } return ESP_OK; }
static void nimble_port_freertos_init(void (*)(void*)) { if (host_ready && ble_hs_cfg.sync_cb) ble_hs_cfg.sync_cb(); }
static void nimble_port_run() {}
static void nimble_port_freertos_deinit() {}
static int nimble_port_stop() { stop_attempts++; if (stop_failures) { stop_failures--; return 1; } return 0; }
static void nimble_port_deinit() { radio_scanning = false; }
static unsigned esp_get_free_heap_size() { return 100000; }
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(unsigned ms) { now_ms += ms; }
