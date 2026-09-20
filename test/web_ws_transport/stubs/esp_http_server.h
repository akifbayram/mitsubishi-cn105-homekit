#pragma once
#include <cstddef>
#include <cstdint>
#define CONFIG_HTTPD_QUEUE_WORK_BLOCKING 0
using esp_err_t = int;
using httpd_handle_t = void *;
constexpr int ESP_OK=0, ESP_FAIL=-1, HTTPD_WS_CLIENT_WEBSOCKET=2, HTTPD_WS_TYPE_TEXT=1;
struct httpd_ws_frame_t { int type; uint8_t *payload; size_t len; };
int httpd_ws_get_fd_info(httpd_handle_t, int);
int httpd_sess_update_lru_counter(httpd_handle_t, int);
int httpd_ws_send_frame_async(httpd_handle_t, int, httpd_ws_frame_t *);
int httpd_queue_work(httpd_handle_t, void (*work)(void *), void *arg);
