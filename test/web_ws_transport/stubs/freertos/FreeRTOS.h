#pragma once
#include <cstdint>
constexpr int pdTRUE=1, pdPASS=1, tskIDLE_PRIORITY=0;
constexpr uint32_t portMAX_DELAY=UINT32_MAX;
#define pdMS_TO_TICKS(x) (x)
