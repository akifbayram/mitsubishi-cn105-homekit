# CN105 Protocol

The CN105 connector uses a serial protocol at 2400 baud with 8E1 (even parity). Some indoor units (many SEZ-KD ducted units, MSZ-GC) answer only at 9600; the rate is selectable in the web UI under **Device** → **Advanced** and stored in NVS as `cn105Baud`. For detailed protocol documentation, see the [muart-group wiki](https://muart-group.github.io/).

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

The configured interval is the idle delay between cycles. A settings SET
starts a confirmation cycle after a 750 ms quiet period, even when the idle
interval is 15 or 30 seconds. Unconfirmed settings are checked at a short
cadence until the existing 10-second confirmation/refusal window expires.
Continuous writes yield to a due poll so they cannot starve status updates.

Each request allows 2.5 seconds for its response. A missing reply ends that
cycle and starts a 750 ms quiet period before another transaction; it does
not hold queued commands for the full idle interval. A reply of the expected
type that arrives inside the quiet period resumes the cycle from that phase;
a later reply does not resume it. A cycle older than one deadline yields
to a queued command at the next phase boundary, so a slow unit cannot hold a
SET for all five phases. Every well-formed reply updates state, but only the
response type currently requested advances the cycle. Three consecutive
error-query timeouts disable the optional `0x04` phase; a later `0x04` answer
re-enables it.

## Remote Temperature (0x07)

A SET command (type `0x41`) that overrides the heat pump's internal thermistor with an external temperature reading. Sending `0x00` disables the override and reverts to the internal sensor.

| Byte | Field |
|------|-------|
| 5 | `0x07` (remote temp command) |
| 6 | `0x01` enable, `0x00` disable |
| 7 | Legacy encoding: `3 + ((temp - 10) * 2)` |
| 8 | Enhanced encoding: `temp * 2 + 128` (`0x80` when disabled) |

Both encodings are sent in the same packet — the heat pump uses whichever it supports. Temperature is rounded to the nearest 0.5°C.

The unit keeps the last remote temperature it was fed when the controller restarts. A device that has ever fed one (NVS `remoteFed`, latched on the first send; on upgrade derived from a stored remote room source) sends one disable packet on the first connection after boot, when the first poll cycle ends, unless a room source has already written the register since boot. A source that starts feeding later simply overrides it.

A device that has never fed the pump leaves the register alone. On a unit with a Mitsubishi wall controller (MHK, PAR) that register carries the controller's own room sensor, and a disable packet would hand the unit back to its internal thermistor.

## Serin Link Bonds

The unit side of the Serin Link pairing lives in `main/sl2_link.c` (vendored from the Serin Link core). Bonds and the unit's identity are stored in NVS namespace `serinlink`:

| Key | Contents |
|-----|----------|
| `sl2_id` | The unit's Ed25519 identity: private key (64 B) then public key (32 B) |
| `sl2_dials` | The bond table: up to 4 dials, each with MAC, link key, and the dial's pinned identity |
| `sl2_cseq` | CAPS sequence number |

Pairing is trust-on-first-use in both directions. The unit pins each dial's identity to its MAC, and the dial pins the unit's identity.

- **Adding a dial.** The Link card offers **Pair another Link** while fewer than 4 dials are paired. With all 4 slots taken it hides the button, and the serial console command `espnow-pair` still opens a window.
- **Re-pairing a bonded dial.** A pairing window opens even when all 4 slots are taken. A dial that is already bonded, presenting its pinned identity, re-pairs into its own slot with a fresh link key. A new dial is refused, and the window stays open for a bonded dial. If a new dial was refused and no bonded dial re-paired, the window closes with the result `full`. With no pairing attempt at all, it closes with `timeout`.
- **Forgetting one dial.** The Link card lists every bonded dial once two or more are paired, each with its own **Forget**. That sends the WebSocket command `{"cmd":"forgetDial","mac":"AA:BB:CC:DD:EE:FF"}`, which removes that bond, its radio peer, and its room reading without a restart. **Forget All** (`forgetRemote`) clears every bond and restarts.
- **Factory reset keeps the identity.** A factory reset erases every NVS namespace, including all bonds, then writes `sl2_id` back. Dials have pinned that identity, so a newly generated one would make each of them refuse to pair again until the dial itself was reset. After a factory reset, pair each dial again as if it were new.
