#pragma once

// MQTT compile gate. Unlike BLE (tied to BT hardware), MQTT works on every
// board, so it defaults ON for all boards. Flash-tight custom builds can
// compile it out with -DMQTT_DISABLE=1.
#if !defined(MQTT_DISABLE)
#define MQTT_ENABLE
#endif
