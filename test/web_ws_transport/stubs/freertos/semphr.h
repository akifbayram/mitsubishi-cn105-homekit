#pragma once
#include "FreeRTOS.h"
#include <condition_variable>
#include <mutex>
struct TestSemaphore {
    std::mutex mutex;
    std::condition_variable cv;
    bool available;
    explicit TestSemaphore(bool initial): available(initial) {}
};
using SemaphoreHandle_t = TestSemaphore *;
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateBinary();
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t wait);
void xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
