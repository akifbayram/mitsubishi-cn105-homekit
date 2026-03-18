#pragma once

#include <stdint.h>

/// Start a captive-portal DNS server that answers all A-record queries
/// with the given IP address.  Runs in its own FreeRTOS task.
void dns_captive_start(uint32_t redirect_ip);

/// Stop the captive-portal DNS task and close the socket.
void dns_captive_stop();
