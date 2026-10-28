# ESP32 ↔ MCU protocol notes

Findings from the stock ESP32 application (`PK_T4 V1.431-80-g5532d38`, SHA256 `6e3263521cfad147b645c69245add407dba73df82b6575a7ab27aa13ea05c0b1`), cross-checked with earlynerd's public bus captures and with captures on a PURA MAX first generation (T4-MAIN-V2.3), 26 October 2026. Addresses refer to that image only. See also [motion.md](motion.md) for the drum commands.

Each row is labeled **confirmed** (code + hardware), **static** (code only) or **inferred**.

## Frame

```
5A A5 | len | type | seq | FF | node|flags | data… | CRC16
```

`len` is the total length including the CRC. CRC16 CCITT (init `FFFF`, poly `1021`), stored little endian. The node is the low nibble. Bit 7 marks a read request (ESP → MCU) or a response, and bit 5 marks an event (state change).

## Types handled by the stock receiver (`FUN_4012e278`, table `0x3f41d810`)

| Type | Stock handling | Seen from the MCU |
|---|---|---|
| 0 | heartbeat: stores the data byte, no reply | yes |
| 1 | node 0: status bits; node 1: sensor words | yes |
| 2 | motor configuration echo (10 bytes) | yes |
| 3 | motor status (14 bytes) | yes |
| 4 | motor parameter block readback (11 bytes) | yes, at init |
| 5 | 18-byte motor block, cached only | never |
| 6 | ignored | never |
| 7 | weight samples, into a FIFO | yes |
| 8 | 8-byte block, cached only | never |
| 9 | MCU version (`COMM_CMD_VER`) | yes |
| 0xA | log `reset cmd!` only | never |
| 0xB, > 0xC | log `UART recv cmd is over range!` | never |
| 0xC | log `config datas cmd!` only | never |

**Static + confirmed**: every type the MCU actually sends (0, 1, 2, 3, 4, 7, 9) is now understood. No stock code path was found that consumes the type 4, 5 or 8 blocks.

## Status bits `0x1.0`

Two identical 16-bit words. The stock code spreads the 9 low bits of the first word into a byte cache (`FUN_401274e8`, `loop`-unrolled), read by sensor id through `FUN_40127944` (table `0x3f41abc4`). Bits are active low.

| Bit | Stock sensor | This component | Status |
|---|---|---|---|
| 0 | id 3 | `unk0` | — |
| 1 | id 4 | `approached` | confirmed |
| 2 | `top_hall` (id 6) | `cover_present` | confirmed |
| 3 | `stdby_hall` (id 7) | `drum_level` | confirmed |
| 4 | `dump_hall` (id 8) | `drum_up` | confirmed |
| 5 | `smooth_hall` (id 9) | `drum_down` | confirmed |
| 6 | `open_hall` (id 10) | `tray_open` | confirmed |
| 7 | `close_hall` (id 11) | `tray_closed` | confirmed |
| 8 | **`box_hall`** (id 5) | `bin_present` | static |

`box_hall` (waste bin) is only read by the stock firmware for device types 12/13 (serial prefixes `T3`/`T4`). On the tested unit it never changes, bin in or out. Removing or inserting the bin only produces a ~0.5 s pulse on `tray_closed` (`70` → `F0` → `70`).

## Sensor words `0x1.1`

Seven little-endian 16-bit words (`FUN_4012759c`):

| Word | Stock use | Notes |
|---|---|---|
| w0 | id 0xE raw, id 0x11 converted, logged as `DC:%d` | probably supply voltage; ~2410 raw on the tested unit |
| w1 | id 0xF | unknown |
| w2 | id 0x10, factory test `PT moto cur` (limit 21) | motor current |
| w3 | id 0xC `prox_L` | ignored by this component |
| w4 | — | unknown |
| w5 | id 0xD `prox_R` | this component's `Distance` |
| w6 | — | unknown |

## Init sequence (`FUN_40127d68`, log `mcu init`)

| Step | Data | This component |
|---|---|---|
| type 2, nodes 0/1 | motor config | `buf_20/21` |
| type 0xC, node 0 / node 1 | `{10,200,15,10,10,10,5,0}` / `200`×4 | `buf_C0/C1` |
| **type 0xC, node 2** | **scram config** `{0x01FC, 0x001F, mask, 1}`, log `set scram cfg` | `buf_C2`, mask `0x704C` |
| type 4, nodes 0/1 | `{0,0,0, 1500/1300, 200, 30, 20}` | `buf_40/41` |
| type 0xD, nodes 0/1 | `{2, 0xFF, 10, 30, 6}` | `buf_D0/D1` |
| type 0, node 0 | `{00}` (handshake) | sent |
| type 1, node 0 | `{10,10,10}` (status subscription) | `buf_1` |
| type 7, node 0 | `{5,10,10}` (weight config) | commented out |

The **scram mask** is `0xF04C` for device types 12/13 and `0x704C` otherwise (bit 15 extra). The factory motor test only enables bits 2, 3, 13 and 14. Bits appear to follow the sensor ids (4 `approached`, 6 cover, 12/13 `prox_L`/`prox_R`). The sender applies `& 0xFFCF`, so **bits 4 and 5 never reach the MCU**; the stock motor task sets bit 4 only as an ESP-side flag (stop classified as `SCRAM - PROX`). The mapping from bits to `scram` codes in motor statuses is unknown, so do not change the mask.

## Motor status (`0x03`)

14 data bytes: `result, scram, act, speed, pos, step(u16), cur(u16), cur_max(u16), runtime(u16), ctrl_ID`. `result` 2 = running, 3 = stopped. This component parses it and publishes position, stop code and current per motor (see [motion.md](motion.md#motor-status-feedback-in-home-assistant)).

`ctrl_ID` echoes the last byte of the motor command. As stock (`FUN_4011d284`, `FUN_4011d42c`), the component numbers each command per motor from 1 to 255 and ignores a status carrying another command's ID, so a late status of a previous move is not taken for the current one. A status with ID 0 is still accepted. Confirmed on hardware (28 October 2026): the MCU echoes the ID in every status of that move (flap open with ID 1, close with ID 2).

**Weight cadence (confirmed):** the MCU delivers ~11.35 samples/s either way. Without the type 7 config it sends 10 samples per packet at ~1.13 packets/s; with `{5,10,10}` it sends 5 per packet at ~2.27 packets/s. The config lives in MCU RAM: an ESP OTA does not reset it, a power cycle does.

## Heartbeat and link supervision

- Before the handshake, the MCU sends its version (type 9) followed by a heartbeat `00` about once a second. After the ESP sends type 0 `{00}`, the stock capture shows a heartbeat `01`, then no more spontaneous version/heartbeat (**inferred**: `00` = waiting for the ESP, `01` = acknowledged). With this component the MCU keeps announcing `00` every second; the reason is not established.
- The stock firmware does not read the heartbeat value in any business path. Every handled frame (types 0–5, 7–9) updates a last-reception time (`FUN_40127118`, `0x3ffcc3c0`).
- **Error `mcu`** (index 1 of the 19-entry error table at `0x4010caac`: `DC, mcu, scale, falldown, moto_M, moto_D, hallT, hallB, hallH, hallD, hallS, hallO, hallC, PROX, rtc, atmz, full, scaleD, OLED`) is raised when nothing was received for **more than 15 s** (`0x4012635c`).
- At task start, if the last MCU reboot is older than **7 days** (604800 s), the stock firmware checks the link with type 9 reads, then **reboots the MCU with type 0xA** and runs `mcu init` again. The reboot frame is a write on node 0 with no data (`FUN_401273a8` → `FUN_4012e084(10, 0, 0, 0, 0, …)`): `5A A5 09 0A <seq> FF 00 <crc>`.

What this component does:

| Stock | This component |
|---|---|
| error `mcu` after 15 s without any frame | `MCU Link Lost` (problem) turns on after 15 s without any valid frame, of any type, and off at the next one; an error is logged. Diagnostic only, nothing is stopped |
| weekly MCU reboot (type 0xA), then `mcu init` | checked every hour: once the last reboot (`MCU Last Reboot`, stored across ESP reboots, UTC from Home Assistant time) is 7 days old, the MCU is rebooted with the same frame. The MCU then announces its version again and the usual init sequence runs. The reboot waits until the litter box is idle (no cleaning, maintenance, manual move, leveling, open flap, pet in front or inside) and the link is up. The status bits are ignored for 2 s after the reboot: on the first test (28 October 2026, 13:07) the MCU re-initialized within 0.2 s, but its first status reported `Drum Down` while the drum was level, which started a leveling (flap opened and closed, drum still). The 7 days count from the first run after flashing; a clock going back restarts the count instead of rebooting. `Reboot MCU` does it at once, with the same idle checks |

## MCU version (type 9)

Data `00 0B 74`: hw 11, sw 116. Already parsed by this component.
