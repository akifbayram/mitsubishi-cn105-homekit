#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void *;
int xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *arg, int, TaskHandle_t *out);
void vTaskDelete(TaskHandle_t);
