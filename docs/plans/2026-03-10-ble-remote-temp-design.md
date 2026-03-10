# BLE Remote Temperature Sensor Support

## Purpose

Override the heat pump's internal temperature sensor with a BLE wireless sensor placed in a more representative location. The heat pump's built-in sensor is often mounted near the ceiling and reads high, causing poor temperature control. Feeding it a remote temperature via the CN105 protocol's `0x07` command lets the unit regulate based on actual room conditions.

## CN105 Remote Temperature Protocol (0x07)

SET packet (type `0x41`) with command byte `0x07`:

```
Bytes 0-4:  Header [0xFC, 0x41, 0x01, 0x30, 0x10]
Byte 5:     0x07 (remote temperature command)
Byte 6:     0x01 = providing remote temp, 0x00 = revert to internal sensor
Byte 7:     Legacy encoding: round(temp * 2) - 16
Byte 8:     Enhanced encoding: round(temp * 2) + 128
Bytes 9-20: 0x00 (padding)
Byte 21:    Checksum: (0xFC - sum_of_bytes_0_to_20) & 0xFF
```

Both legacy and enhanced encodings are sent simultaneously for compatibility. The heat pump reverts to its internal sensor after ~10 minutes without receiving a remote temperature update — periodic re-sends (keepalive) are required.

## Architecture

### Components

#### BLESensor (`src/ble_sensor.cpp` + `include/ble_sensor.h`)

- NimBLE-Arduino passive/active BLE scanner
- Scan cycle: 5s scan every 10s (configurable)
- Advertisement callback filters by configured MAC address
- Auto-detects advertisement format, delegates to decoder
- Exposes: `temperature`, `humidity`, `battery`, `lastUpdate`, `rssi`
- Stale timeout: 90s with no update → marks sensor invalid

#### BLE Decoders (`include/ble_decoders.h`)

Supported formats:

| Format | Models/Sensors | Encoding |
|--------|---------------|----------|
| Govee 3-byte | H5072, H5075, H5101, H5102, H5174, H5177 | Big-endian combined: `(b0<<16\|b1<<8\|b2) / 10000` for temp |
| Govee 2-byte | H5074, H5100, H5104, H5105, H5179 | Little-endian int16: `raw / 100.0` for temp |
| BTHome v2 | Any BTHome device | Service data UUID `0xFCD2`, TLV objects |
| 0x181A Service Data | pvvx Xiaomi LYWSD03MMC, generic | Service data UUID `0x181A`, int16 temp at bytes 6-7 |

Each decoder returns `{valid, temperature, humidity, battery}` or failure.

**Govee 3-byte decoding:**
```cpp
int32_t raw = (data[0] << 16) | (data[1] << 8) | data[2];
bool negative = raw & 0x800000;
if (negative) raw ^= 0x800000;
float temp = (raw / 1000) / 10.0f;
float hum = (raw % 1000) / 10.0f;
if (negative) temp = -temp;
```

**Govee 2-byte decoding:**
```cpp
int16_t temp_raw = data[0] | (data[1] << 8);  // little-endian signed
uint16_t hum_raw = data[2] | (data[3] << 8);
float temp = temp_raw / 100.0f;
float hum = hum_raw / 100.0f;
```

**Govee identification:** Manufacturer data company ID `0xEC88` (on wire: `88 EC`).

#### CN105 Integration (`cn105_protocol.cpp`)

- New `sendRemoteTemperature(float temp)` method — builds and sends the `0x07` packet
- Keepalive: called every 20s while BLE sensor is active and valid
- `sendRemoteTemperature(0)` or negative value → sends byte 6 = 0x00 to revert to internal sensor
- Integrates with existing poll cycle deferral (wait for `_cycleRunning == false`)

#### Settings (`settings.h` / `settings.cpp`)

New NVS fields:
- `bleEnabled` (bool, default false)
- `bleSensorAddr` (6-byte MAC address, stored as string "AA:BB:CC:DD:EE:FF")
- `bleSensorName` (string, for display in web UI)

#### Web UI (`web/index.html`)

Settings panel — "Remote Temperature Sensor" section:
- "Scan for Sensors" button → triggers BLE scan, returns discovered sensors as list
- Each result shows: device name, MAC address, RSSI signal strength, detected format
- Select a sensor → saves MAC + name to settings, enables BLE remote temp
- "Disable" button → sends revert command, clears settings
- Status display: sensor name, last temperature, humidity, battery %, signal, last update time

#### WebSocket Commands

- `bleScan` → start discovery scan, returns results as JSON array
- `bleSelect` → `{"mac": "AA:BB:CC:DD:EE:FF", "name": "Govee H5075"}` — pair sensor
- `bleDisable` → revert to internal sensor and clear config

#### HTTP API Fallback (`web_server.cpp`)

- WebSocket `set` command with `remoteTemp` field (float, 0 to disable)
- Allows external systems (Home Assistant, scripts) to feed temperature without BLE
- When API-provided, BLE scanning is paused; API value uses same 20s keepalive

### Data Flow

```
BLE Sensor → advertisement → ESP32-C6 NimBLE scan → decoder → BLESensor.temperature
    ↓
main loop: every 20s if BLE valid → cn105.sendRemoteTemperature(bleSensor.temperature)
    ↓
Heat pump uses remote temp for control, reports it back in 0x03 response
    ↓
HomeKit / Web UI show corrected temperature
```

### Memory Budget

| Component | Flash | RAM |
|-----------|-------|-----|
| NimBLE-Arduino | ~60-80KB | ~30KB |
| Decoders | ~3KB | negligible |
| BLESensor class | ~5KB | ~1KB |
| **Total additional** | **~70-90KB** | **~31KB** |

Current partition: 1,984KB app — comfortable headroom.

### Failure Handling

- **Sensor offline** (no advertisement for 90s): revert to internal sensor, log warning, show status in web UI
- **Scan finds nothing**: "No sensors found" in web UI
- **WiFi+BLE coexistence**: ESP32-C6 supports concurrent WiFi + BLE. Monitor free heap; if critically low, reduce scan frequency
- **Sensor format unknown**: show device in scan results as "Unknown format" — user can still select, but decoder will skip until format support is added

### Dependencies

- [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — lightweight BLE stack for ESP32, ESP32-C6 compatible
- No other new dependencies
