# Drum motion: MCU-side destinations

This page describes how the drum is driven since the motion rework, why it changed, and what was validated on hardware (PURA MAX first generation, T4-MAIN-V2.3, 26 October 2026).

## Motor command (`0x02`) recap

Data bytes after the node byte: `[type, c1, pos, act, speed, step(u16), overtime(u16), ctrl_ID]`.

| Field | Meaning |
|---|---|
| `type` | 1 = relative move by `step`, 2 = stop, 3 = move until destination / overtime |
| `pos` | **destination at which the MCU stops the motor itself** (stock firmware logs it as `pos` / `Dpos`); 0 = none |
| `act` | direction (drum: 1 = down, 2 = up in this component's naming) |
| `overtime` | safety limit |

Drum destinations (`pkt4_mcu.motor` option `pos:`):

| `pos:` | Code | Status | Evidence |
|---|---|---|---|
| `level` | 1 | confirmed | MCU stops at status position 1 with `result=3, scram=0` (4 runs) |
| `dump` | 2 | confirmed | stop at position 3 with `scram=0`; a command sent while already at dump completes instantly |
| `smooth` | 3 | probable | stops at position 5 without stopping at level on the way; smooth is also an end stop (`scram=1` with or without destination) |

Status positions reported by `0x03`: 1 level, 3 dump, 5 smooth (even values are in between), 7 tray open, 9 tray closed.

## Leveling: before / after

| | Before | After |
|---|---|---|
| Command | type 3, `pos` 0, overtime 1000 (down) or 80 (up from the bottom) | type 3, `pos: level`, overtime 800 |
| Who stops the drum | ESPHome, when `drum_level` is seen in `0x1.0` | the MCU, at the destination |
| Observed stop | overshoot by 1–1.5 s, or stop between positions on overtime, then no further trigger: stuck | MCU stops ~25 ms after reaching position 1 |
| Already level | down then back up | the MCU does not move; leveling is finished and the tray is closed |
| During cleaning | — | unchanged (see below) |

## Cleaning: before / after

| Step | Before | After |
|---|---|---|
| Open tray | type 3, overtime 80 | unchanged |
| Up to dump | no destination, ESPHome stops at `drum_up` | `pos: dump`, MCU stops at dump |
| At dump | reverse immediately | 3 s pause (stock) |
| Down to smooth | no destination, ESPHome stops at `drum_down` | `pos: smooth`, MCU stops at smooth (passes level without stopping) |
| At smooth | return to level immediately | 1.5 s pause (stock) |
| Back to level | no destination, overtime 80 | `pos: level`, overtime 800 |
| Close tray | speed 5, overtime 80 | unchanged |

Not ported from the stock recipe: tray homing before opening, relative moves (type 1) and shakes; the settings selecting those branches are not identified.

## Serial reception

Leveling and cleaning rely on seeing hall edges and motor statuses. The UART RX buffer is 2048 bytes and every buffered frame is parsed on each loop, with byte-wise resync on `5A A5`. `dump_config` reports `RX bytes dropped` (0 on every capture since the fix; before it, up to ~80–90 % of frames were lost during boot and motion).

## Hardware validation (26 October 2026)

| Run | Sequence | Result |
|---|---|---|
| 19:35 | leveling from dump, `pos: level` | MCU stop at level, `scram=0`, no overshoot |
| 19:38–19:39 | cleaning (no destination) + leveling `pos: level` | final stop at level by the MCU |
| 20:02 | cleaning with dump/smooth codes swapped | descent command completed at once at dump: codes corrected |
| 20:06–20:07 | cleaning `pos: dump` → 3 s → `pos: smooth` → 1.5 s → `pos: level` | every stop by the MCU, ~93 s, 0 RX bytes dropped |

Still to validate: pressing Level when already level, and several days of automatic cleanings.

## Stopping the drum when a pet is detected

| | Stock firmware | This component |
|---|---|---|
| Who cuts the motor on proximity | **the MCU** (hardware scram) | **ESPHome**, on the `Approach` bit (`emergency_stop` script); same for `Pet Inside` (weight) |
| Scram mask (`0xC` node 2) | changed per motion step: bits 12, 2, 3 at step start, **bit 4 (approach)** too unless two internal flags are set; bits 12/4 toggled during the step | fixed `0x704C` (bits 2, 3, 6, 12, 13, 14); **bit 4 never set** |
| Stop cause | classified after a scram stop: 6 = `SCRAM - PROX`, 15 = pet present (weight), 7 = other, 4 = direct resume | unknown (motor statuses are not read) |
| Resume | step suspended, then continued (`moto back work suspend/continue`) | `auto_clean` once `Approach` has been off for 5 s |

Static analysis only (`FUN_4011e008`, `0x4011e270`, `0x4011e70d`, `0x4011e9f8`–`0x4011ec6c`). A MCU-side proximity stop would avoid the serial round trip, but enabling bit 4 and reading the resulting statuses still needs to be designed and tested with an object in front of the sensor, never with the pet.
