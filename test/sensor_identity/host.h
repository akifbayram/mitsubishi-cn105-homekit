#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "sl2_proto.h"
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(mux) ((void)(mux))
#define taskEXIT_CRITICAL(mux) ((void)(mux))
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)
#define CASE(name) if (!strcmp(argv[1], name))
static uint64_t now_ms = 1000;
static int64_t esp_timer_get_time() { return (int64_t)now_ms * 1000; }
#include "clock.inc"
constexpr int ROOM_MAX_BLE_SENSORS = 4, ROOM_MEMBER_LINK = 1, ROOM_MEMBER_BLE0 = 2, ROOM_MEMBER_INTERNAL = 0;
