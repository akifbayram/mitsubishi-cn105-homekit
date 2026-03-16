# CN105 Protocol

The CN105 connector uses a serial protocol at 2400 baud with 8E1 (even parity). For detailed protocol documentation, see the [muart-group wiki](https://muart-group.github.io/).

## Packet Format

| Byte | Field |
|------|-------|
| 0 | `0xFC` sync byte |
| 1 | Packet type |
| 2–3 | `0x01 0x30` header |
| 4 | Data length |
| 5+ | Data payload |
| Last | Checksum: `(0xFC - sum_of_all_bytes) & 0xFF` |

## Polling Cycle

5 phases, default 2s interval:

| Phase | Type | Data |
|-------|------|------|
| 0x02 | Settings | Power, mode, target temp, fan, vane, wide vane |
| 0x03 | Room temp | Room temperature, outside temperature, runtime hours |
| 0x04 | Error | Error code (0x80 = normal) |
| 0x06 | Status | Operating state, compressor frequency |
| 0x09 | Standby | Sub mode, stage, auto sub mode |

## Remote Temperature (0x07)

A SET command (type `0x41`) that overrides the heat pump's internal thermistor with an external temperature reading. Sending `0x00` disables the override and reverts to the internal sensor.

| Byte | Field |
|------|-------|
| 5 | `0x07` (remote temp command) |
| 6 | `0x01` enable, `0x00` disable |
| 7 | Legacy encoding: `3 + ((temp - 10) * 2)` |
| 8 | Enhanced encoding: `temp * 2 + 128` (`0x80` when disabled) |

Both encodings are sent in the same packet — the heat pump uses whichever it supports. Temperature is rounded to the nearest 0.5°C.
