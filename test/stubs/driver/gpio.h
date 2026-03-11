#pragma once
#include <cstdint>

typedef int gpio_num_t;

#define GPIO_PULLUP_ONLY 0x04

inline int gpio_set_pull_mode(gpio_num_t, int) { return 0; }
