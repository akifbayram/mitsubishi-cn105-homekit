#include "improv_serial.h"
#include "wifi_manager.h"
#include "wifi_recovery.h"
#include "branding.h"
#include "board_profile.h"
#include "logging.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#include "sdkconfig.h"
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
#include <driver/usb_serial_jtag_vfs.h>
#elif defined(CONFIG_ESP_CONSOLE_UART)
#include <driver/uart_vfs.h>
#endif

static const char *TAG = "improv";

// Improv Serial is a binary, newline-framed protocol. The console's default
// line-ending translation (RX CR->LF, TX LF->CRLF) mangles frame bytes equal to
// 0x0D/0x0A, and the browser SDK re-syncs its byte parser on '\n'. While Improv
// owns the console we switch to raw LF passthrough; on stop we restore the
// console defaults so the ESP-NOW REPL's Enter handling keeps working.
static void improv_set_console_raw(bool raw) {
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    usb_serial_jtag_vfs_set_rx_line_endings(raw ? ESP_LINE_ENDINGS_LF : ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(raw ? ESP_LINE_ENDINGS_LF : ESP_LINE_ENDINGS_CRLF);
#elif defined(CONFIG_ESP_CONSOLE_UART)
    uart_vfs_dev_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          raw ? ESP_LINE_ENDINGS_LF : ESP_LINE_ENDINGS_CR);
    uart_vfs_dev_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                          raw ? ESP_LINE_ENDINGS_LF : ESP_LINE_ENDINGS_CRLF);
#endif
}

// ── Protocol constants ────────────────────────────────────────────────────────
static constexpr uint8_t PKT_CURRENT_STATE = 0x01;
static constexpr uint8_t PKT_ERROR_STATE   = 0x02;
static constexpr uint8_t PKT_RPC           = 0x03;
static constexpr uint8_t PKT_RPC_RESULT    = 0x04;

static constexpr uint8_t CMD_WIFI_SETTINGS         = 0x01;
static constexpr uint8_t CMD_GET_CURRENT_STATE      = 0x02;
static constexpr uint8_t CMD_GET_DEVICE_INFO        = 0x03;
static constexpr uint8_t CMD_SCAN_WIFI              = 0x04;

static constexpr uint8_t STATE_AUTHORIZED   = 0x02;
static constexpr uint8_t STATE_PROVISIONING = 0x03;
static constexpr uint8_t STATE_PROVISIONED  = 0x04;

static constexpr uint8_t ERR_INVALID_RPC       = 0x01;
static constexpr uint8_t ERR_UNKNOWN_RPC       = 0x02;
static constexpr uint8_t ERR_UNABLE_TO_CONNECT = 0x03;

// ── Module state ──────────────────────────────────────────────────────────────
static TaskHandle_t s_task_handle = nullptr;
static char         s_device_name[32] = {};
static uint8_t      s_cur_state = STATE_AUTHORIZED;

// ── Frame reader parse state ──────────────────────────────────────────────────
enum ParseState : uint8_t {
    SYNC_I, SYNC_M, SYNC_P, SYNC_R, SYNC_O, SYNC_V,
    WAIT_VER, WAIT_TYPE, WAIT_LEN, WAIT_DATA, WAIT_CSUM
};

static ParseState s_parse      = SYNC_I;
static uint8_t    s_frame_type = 0;
static uint8_t    s_frame_len  = 0;
static uint8_t    s_frame_data[255];
static uint8_t    s_frame_idx  = 0;
static uint8_t    s_csum_acc   = 0;

// ── Frame encoder ─────────────────────────────────────────────────────────────
static void write_frame(uint8_t type, const uint8_t* data, uint8_t len) {
    uint8_t checksum = 0x01 + type + len;
    // Leading '\n': guarantees the IMPROV header starts a fresh line so the
    // browser SDK — which re-syncs its byte parser on '\n' and discards non-IMPROV
    // runs up to the next newline — can lock onto the frame even while other tasks
    // are logging to the same console mid-stream.
    fputc('\n', stdout);
    fwrite("IMPROV", 1, 6, stdout);
    fputc(0x01, stdout);
    fputc(type, stdout);
    fputc(len,  stdout);
    for (uint8_t i = 0; i < len; i++) {
        fputc(data[i], stdout);
        checksum += data[i];
    }
    fputc(checksum & 0xFF, stdout);
    // Trailing '\n' terminates the frame (Improv Serial transport convention).
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_current_state(uint8_t state) {
    write_frame(PKT_CURRENT_STATE, &state, 1);
}

static void send_error(uint8_t error) {
    write_frame(PKT_ERROR_STATE, &error, 1);
}

// ── Command handlers ──────────────────────────────────────────────────────────
static void handle_get_device_info() {
    const char* strings[4] = { BRAND_NAME, FW_VERSION, BOARD_NAME, s_device_name };
    uint8_t buf[255];
    uint8_t pos = 1;
    buf[0] = CMD_GET_DEVICE_INFO;
    for (int i = 0; i < 4; i++) {
        uint8_t l = (uint8_t)strlen(strings[i]);
        if (pos + 1 + l > sizeof(buf)) break;
        buf[pos++] = l;
        memcpy(buf + pos, strings[i], l);
        pos += l;
    }
    write_frame(PKT_RPC_RESULT, buf, pos);
}

static void handle_scan_wifi() {
    WifiManager::ScannedNetwork nets[20];
    int count = WifiManager::scanNetworks(nets, 20);

    for (int i = 0; i < count; i++) {
        uint8_t buf[80];
        uint8_t pos = 1;
        buf[0] = CMD_SCAN_WIFI;

        uint8_t sl = (uint8_t)strlen(nets[i].ssid);
        char rssi_str[8];
        snprintf(rssi_str, sizeof(rssi_str), "%d", (int)nets[i].rssi);
        uint8_t rl = (uint8_t)strlen(rssi_str);
        const char* auth = nets[i].secure ? "YES" : "NO";
        uint8_t al = nets[i].secure ? 3 : 2;

        if (pos + 3 + sl + rl + al > sizeof(buf) - 1) continue;

        buf[pos++] = sl; memcpy(buf + pos, nets[i].ssid, sl); pos += sl;
        buf[pos++] = rl; memcpy(buf + pos, rssi_str, rl);     pos += rl;
        buf[pos++] = al; memcpy(buf + pos, auth, al);          pos += al;

        write_frame(PKT_RPC_RESULT, buf, pos);
    }

    // Zero-string terminator signals end-of-list
    uint8_t term = CMD_SCAN_WIFI;
    write_frame(PKT_RPC_RESULT, &term, 1);
}

static void handle_wifi_settings(const uint8_t* data, uint8_t len) {
    if (len < 2) { send_error(ERR_INVALID_RPC); return; }

    uint8_t ssid_len = data[0];
    if (ssid_len > 32 || 1 + ssid_len >= len) { send_error(ERR_INVALID_RPC); return; }

    char ssid[33] = {};
    char pass[65] = {};
    memcpy(ssid, data + 1, ssid_len);

    uint8_t pass_start = 1 + ssid_len;
    uint8_t pass_len   = data[pass_start];
    uint8_t pass_copy  = pass_len > 64 ? 64 : pass_len;
    if (pass_start + 1 + pass_copy <= len)
        memcpy(pass, data + pass_start + 1, pass_copy);

    LOG_INFO("[Improv] WiFi settings: SSID='%s'", ssid);
    WifiManager::connect(ssid, pass);

    s_cur_state = STATE_PROVISIONING;
    send_current_state(STATE_PROVISIONING);

    for (int i = 0; i < 150; i++) {         // 15 s @ 100 ms ticks
        vTaskDelay(pdMS_TO_TICKS(100));
        if (WifiManager::isConnected()) break;
    }

    if (WifiManager::isConnected()) {
        WifiManager::saveCredentials(ssid, pass);
        wifiRecovery.setChangePending(true);
        s_cur_state = STATE_PROVISIONED;
        send_current_state(STATE_PROVISIONED);

        char ip[16] = {};
        WifiManager::getIP(ip, sizeof(ip));
        char url[36] = {};
        snprintf(url, sizeof(url), "http://%s/", ip);
        uint8_t url_len = (uint8_t)strlen(url);

        uint8_t resp[40];
        resp[0] = CMD_WIFI_SETTINGS;
        resp[1] = url_len;
        memcpy(resp + 2, url, url_len);
        write_frame(PKT_RPC_RESULT, resp, 2 + url_len);

        LOG_INFO("[Improv] Provisioned, IP: %s", ip);
    } else {
        send_error(ERR_UNABLE_TO_CONNECT);
        s_cur_state = STATE_AUTHORIZED;
        send_current_state(STATE_AUTHORIZED);
        LOG_WARN("[Improv] Connection failed");
    }
}

// ── Frame reader ──────────────────────────────────────────────────────────────
static void dispatch_frame() {
    if (s_frame_type != PKT_RPC || s_frame_len < 1) return;
    switch (s_frame_data[0]) {
        case CMD_GET_CURRENT_STATE: send_current_state(s_cur_state);                               break;
        case CMD_GET_DEVICE_INFO:   handle_get_device_info();                                       break;
        case CMD_WIFI_SETTINGS:     handle_wifi_settings(s_frame_data + 1, s_frame_len - 1);       break;
        case CMD_SCAN_WIFI:         handle_scan_wifi();                                             break;
        default:                    send_error(ERR_UNKNOWN_RPC);                                    break;
    }
}

static void process_byte(uint8_t b) {
    switch (s_parse) {
        case SYNC_I:    s_parse = (b == 'I') ? SYNC_M    : SYNC_I; return;
        case SYNC_M:    s_parse = (b == 'M') ? SYNC_P    : SYNC_I; return;
        case SYNC_P:    s_parse = (b == 'P') ? SYNC_R    : SYNC_I; return;
        case SYNC_R:    s_parse = (b == 'R') ? SYNC_O    : SYNC_I; return;
        case SYNC_O:    s_parse = (b == 'O') ? SYNC_V    : SYNC_I; return;
        case SYNC_V:    s_parse = (b == 'V') ? WAIT_VER  : SYNC_I; return;
        case WAIT_VER:
            s_csum_acc = b; s_parse = WAIT_TYPE; return;
        case WAIT_TYPE:
            s_frame_type = b; s_csum_acc += b; s_parse = WAIT_LEN; return;
        case WAIT_LEN:
            s_frame_len = b; s_frame_idx = 0; s_csum_acc += b;
            s_parse = (b == 0) ? WAIT_CSUM : WAIT_DATA; return;
        case WAIT_DATA:
            if (s_frame_idx < sizeof(s_frame_data))
                s_frame_data[s_frame_idx] = b;
            s_frame_idx++;
            s_csum_acc += b;
            if (s_frame_idx >= s_frame_len) s_parse = WAIT_CSUM;
            return;
        case WAIT_CSUM:
            s_parse = SYNC_I;
            if ((s_csum_acc & 0xFF) != b) {
                LOG_WARN("[Improv] checksum mismatch, discarding frame");
                return;
            }
            dispatch_frame();
            return;
    }
}

// ── FreeRTOS task ─────────────────────────────────────────────────────────────
static void improv_task(void* /*arg*/) {
    fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

    s_cur_state = STATE_AUTHORIZED;
    s_parse = SYNC_I;
    send_current_state(STATE_AUTHORIZED);
    LOG_INFO("[Improv] Started, state=AUTHORIZED");

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, 0) > 0) break;

        int c = fgetc(stdin);
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        process_byte((uint8_t)c);
    }

    LOG_INFO("[Improv] Stopped");
    s_task_handle = nullptr;
    vTaskDelete(nullptr);
}

// ── Public API ────────────────────────────────────────────────────────────────
void improv_serial_start(const char* deviceName) {
    if (s_task_handle) return;
    strncpy(s_device_name, deviceName, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';
    improv_set_console_raw(true);   // binary-clean console for the framed protocol
    xTaskCreate(improv_task, "improv", 4096, nullptr, tskIDLE_PRIORITY + 1, &s_task_handle);
    LOG_INFO("[Improv] Task created");
}

void improv_serial_stop() {
    if (!s_task_handle) return;
    xTaskNotifyGive(s_task_handle);
    // 100ms grace: WifiRecovery has a 6s AP linger before calling stop(),
    // so the task has finished sending its last response well before this fires.
    vTaskDelay(pdMS_TO_TICKS(100));
    // s_task_handle is cleared by the task itself before vTaskDelete
    improv_set_console_raw(false);  // restore CR/CRLF for the REPL / serial monitor
}
