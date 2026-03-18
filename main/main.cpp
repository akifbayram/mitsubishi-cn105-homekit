#include <stdio.h>
#include "logging.h"

static const char *TAG = "main";

extern "C" void app_main(void) {
    logging_init();
    LOG_INFO("Mitsubishi CN105 HomeKit Controller starting...");
}
