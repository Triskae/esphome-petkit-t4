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
| `smooth` | 3 | confirmed | stops at position 5 with `scram=0` once the spurious resume command was removed (20:38:35); before, the ignored up command turned it into `scram=1` |

Status positions reported by `0x03`: 1 level, 3 dump, 5 smooth (even values are in between), 7 tray open, 9 tray closed.

## Leveling: before / after

| | Before | After |
|---|---|---|
| Command | type 3, `pos` 0, overtime 1000 (down) or 80 (up from the bottom) | type 3, `pos: level`, overtime 800 |
| Who stops the drum | ESPHome, when `drum_level` is seen in `0x1.0` | the MCU, at the destination |
| Observed stop | overshoot by 1–1.5 s, or stop between positions on overtime, then no further trigger: stuck | MCU stops ~25 ms after reaching position 1 |
| Already level | down then back up | the MCU does not move; leveling is finished and the tray is closed |
| During cleaning | — | unchanged (see below) |
| From the dump side | straight down to level | via smooth, as stock: see [Leveling via smooth](#leveling-via-smooth) |

### Leveling via smooth

Positions and actions are different things: **level**, **dump** and **smooth** are the drum's three hall positions (1, 3, 5); **leveling** is the action of bringing the drum back to level. Smooth is on the other side of level from dump: going past level to smooth lets the litter fall back and spreads it.

Stock leveling (`FUN_4011ceb8`, also run when maintenance is left with OK, process 3) goes to smooth (type 3, pos 3, down), pauses 0.5 s, comes back up to level, pauses 0.5 s, then makes a small final move (step 30). When the drum is on the smooth side it goes up to level directly.

| Start | Before | Now |
|---|---|---|
| Dump side (after maintenance, `Level`, boot, an interrupted move) | `pos: level`, down | `pos: smooth`, down past level; at smooth, 1.5 s pause then `pos: level`, up (the same `Drum Down` handler as the cleaning) |
| Smooth side | `pos: level`, up | unchanged |
| Already level | no move | unchanged (stock would also swing to smooth and back) |

Passing level on the way to smooth is not the end of the leveling: a `smoothing` flag makes the `Drum Level` release skip closing the flap and `auto_clean` that one time; it is also cleared at smooth and by `emergency_stop`.

Not ported: the 0.5 s pauses (the shared handler pauses 1.5 s, as for the cleaning) and the final 30-step move.

Validated on hardware (28 October 2026, 23:58–23:59, `Maintenance` off from the maintenance position): flap open (7, code 0), drum down with `pos: smooth`, level passed at 23:59:02.9 with no command sent, stop at smooth (5, code 0) after 17.2 s, 2 s later `pos: level` up, stop at level (1, code 0) after 5.7 s, flap closed (9, code 0).

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
| 20:29–20:31 | cleaning with a hand in front of the entrance during the way up | MCU stops the drum by itself on proximity; resume after 5 s; cleaning finished |
| 20:37–20:38 | cleaning after the resume fix | no command when passing level; dump, smooth and level all reached with `scram=0` |

Still to validate: pressing Level when already level, and several days of automatic cleanings.

## Stopping the drum when a pet is detected

The scram mask bits appear to follow the stock sensor ids: bit 4 = `approached`, bit 6 = cover (`top_hall`), bits 12/13 = `prox_L`/`prox_R`. **The stock sender (`FUN_40127c90`) masks the value with `0xFFCF`, so bits 4 and 5 never reach the MCU**: bit 4 is an ESP-side flag only, used to classify a stop as `SCRAM - PROX`. On the MCU side, proximity protection comes from bits 12/13, which this component already sends permanently (`0x704C`).

| | Stock firmware | This component |
|---|---|---|
| MCU-side proximity scram | bits 12/13, enabled at each step start and toggled during some phases | bits 12/13 always enabled (static `0x704C`) |
| ESP-side stop | after the MCU stops, the cause is classified: 6 = `SCRAM - PROX` (internal bit 4 + proximity state), 15 = pet present (weight), 7 = other, 4 = direct resume | `emergency_stop` sends a stop on `Approach` and on `Pet Inside` (weight) |
| Stop cause visible | yes (logged, classified) | yes since motor statuses are read: `Drum Stop Code` / `Tray Stop Code` in Home Assistant |
| Resume | step suspended, then continued (`moto back work suspend/continue`) | `auto_clean` once `Approach` has been off for 5 s |

Static analysis: `FUN_4011e008` (`0x4011e270`, `0x4011e70d`, `0x4011e9f8`–`0x4011ec6c`), `FUN_40127c90`. **Observed on hardware (26 October 2026, hand in front of the entrance during a cleaning, pet kept away): the MCU already stops the drum by itself.** Its stopped status (`result=3`) arrives in the same millisecond as the `Approach` bit, ~140 ms before the stop sent by ESPHome (20:29:26.220 vs 20:29:26.359, and again at 20:29:34). The stop code is `1`, the same as for an end stop or a stop command, so `Drum Stop Code` alone cannot tell a proximity stop apart. The cleaning resumed once `Approach` had been off for 5 s and finished normally (dump `scram=0`, smooth, level `scram=0`). After such a stop the MCU reports position `0` (unknown) until the next hall sensor.

### Fixed: resume command while passing level

During the cleaning descent the drum passes the level sensor. Its release used to run `auto_clean`, which "resumed" the cleaning and sent a new `pos: dump` up command while the drum was still moving down. In both observed runs (19:39:20 and 20:30:50 on 26 October 2026) the MCU ignored it and kept going to smooth. The `Drum Level` release now runs `auto_clean` only when no cleaning is in progress; resuming an interrupted cleaning still happens on `Approach` / `Pet Inside` release. Verified at 20:38:27: no command when passing level, and the smooth stop now ends with `scram=0`.

### Fixed: pending clean dropped when a pet is in front

After a visit, `clean_if_needed` waits `Clean After` and then turns `Clean` on. `Clean` refuses to start while `Approach` is on, and nothing retried it: on 27 October 2026 at 04:42:19 a 77 g deposit was left because the cat was in front of the entrance (04:42:05–04:42:23) exactly when the one-minute delay ended. It was only cleaned after the next visit.

| | Before | Now |
|---|---|---|
| `Approach` released while a clean is pending | `auto_clean` (nothing, the pending clean already used `cleans_left`) | the delay restarts: the clean starts a full `Clean After` after the pet was last seen |
| Pet in front or inside when the delay ends | clean refused or skipped, never retried | waits until `Approach` and `Pet Inside` are off, then runs the usual checks |

`Clean Pending` stays on while waiting. The stock firmware never drops the request either (it suspends it, or starts and lets the step protection pause the drum), but it does not restart the countdown when a pet only passes in front; this variant is deliberately more conservative.

## Maintenance: before / after

Stock recipe 9 (`FUN_4011d998`, branch `0x4011dfaf`): make sure the waste flap is closed (`FUN_4011ccd8`), align the drum if it isn't at a known position (`FUN_4011cc10`), then a **relative** drum move (type 1) up with dump as limit, step 1000, speed 100, overtime 800, followed by a 900 s wait step.

| | Before | After |
|---|---|---|
| Waste flap | **opened** first | **closed** first if it isn't (as stock); maintenance is cancelled if it doesn't close within 10 s |
| Drum move | type 3, no destination, up, timeout 120 | type 1 (`mode: sense`), up, `duration: 1000`, `pos: dump`, timeout 800 |
| Alignment | — | not needed: maintenance still only starts from level |
| Leveling while in maintenance | could open the flap and move the drum (`level_if_needed` had no maintenance check) | `level_if_needed` does nothing while maintenance is on |
| Exit | stop, update litter mass, level | stop, update litter mass, level [via smooth](#leveling-via-smooth), as stock's exit (process 3) |

With the flap closed, maintenance never drops anything into the waste bin, so it stays allowed when `Tray Full` is on.

Not ported: the 900 s wait step (its release conditions are unknown, so maintenance still lasts until it is turned off) and the stock exit path.

## Who stops the motors

Every ordinary move now carries a destination, so the MCU stops it itself; ESPHome no longer sends a stop when it sees a hall sensor.

| Move | Destination | Stop sent by ESPHome before | Now |
|---|---|---|---|
| Cleaning, leveling | `dump` / `smooth` / `level` | on `Drum Up` press, `Drum Down` press and `Drum Level` release (after the MCU's own stop) | none |
| Manual `Drum Up` / `Drum Down` switches | none, timeout 1000 | on the next hall sensor | `pos: dump` / `pos: smooth`, timeout 800; the switch is turned off (state only) when the position is reached |
| Waste flap open / close | none | on every `Tray` change | `pos: open` (4) / `pos: closed` (5), stock builder codes (`FUN_4011cc9c`) |

Stops are still sent where they are needed: `Approach` / `Pet Inside` (`emergency_stop`), cover opened, bin removed, `Block Movement`, OTA, and turning off `Clean`, `Maintenance` or a manual switch.

The flap destinations come from the stock builder and production test, and are confirmed on hardware: the MCU stops at position 7 / 9 with stop code 0 (see below).

### Fixed: cleaning held at dump

At dump the drum pauses 3 s, then `level_if_needed` sends it down. If a pet came during that pause, `Approach` was still on when the pause ended (5 s `delayed_off`), so nothing moved; when it cleared, `auto_clean` resumed with `Clean`, which refuses to start while `Drum Up` is on, and its refusal turned the cleaning off. The drum stayed at dump until the next event that runs `level_if_needed` (next approach, visit, cover). A resume refused only because the drum is at dump now carries on down (`level_if_needed`); a new cleaning still can't start from dump. Found by reading the code, not observed.

## Fixed: Block Movement

The guard in `level_if_needed` and `clean_if_needed` was a list of conditions, which ESPHome ANDs: moves were only blocked during an OTA **and** with `Block Movement` on. The guard is now "OTA in progress **or** `Block Movement`" (substitution `movement_blocked`; only a running update counts: an aborted or failed one, which doesn't reboot, used to keep every move blocked until a reboot, and now runs the pending leveling), checked by those scripts and by every command that starts a move: `Clean`, `Maintenance`, `Tray`, `Drum Up`, `Drum Down`. The conditions read the states directly; the `Movement Blocked` diagnostic sensor only shows it in Home Assistant (reading the sensor itself made the leveling requested when `Block Movement` is turned off see a stale "blocked" and do nothing).

### Hardware validation (27 October 2026, 23:28–23:36)

| Test | Result |
|---|---|
| `Tray` on | open with `pos: open`: the MCU stops at position 7, stop code 0; `level_if_needed` then closes it with `pos: closed`: position 9, stop code 0 |
| `Maintenance` on (flap already closed) | type 1, pos 2, up, speed 100, step 1000, overtime 800: the drum turns up for 10.0 s (step unit ≈ 10 ms) and stops at position 2 (between level and dump), stop code 0 |
| `Maintenance` off | flap opens (7, code 0), drum back to level in 8.1 s (1, code 0), flap closes (9, code 0) |
| `Block Movement` on, then `Level` and `Tray` | no motor command sent |
| `Block Movement` off | the pending leveling runs (flap open/close, drum already level) |

## Abnormal stops

Normal moves end with stop code 0 (destination reached) or 1 (stopped). The other codes are handled as the stock firmware does (`FUN_4011d300`, `FUN_4011cbc4`, `FUN_4011e008`; see the research report `RETOURS_MOTEURS_02_03.md`). Normal motion is unchanged.

| Code | Meaning | Reaction |
|---|---|---|
| 4 | drum stall (seen once: high current at start) | pause 14 s, back off 400 steps (~4 s) the way it came, then resume (`auto_clean`: the cleaning resumes, or the drum levels); after 3 back-offs: `Motor Error`, cleaning stopped, one leveling attempt. The counter is reset when the drum reaches level |
| 17, 18 | timeout (18 seen: overtime reached) | `Motor Error`, cleaning stopped, no further move |
| 5 / flap close failure | flap stall | first failure: the flap is reopened and closed again (stock retries the flap); second failure: full bin (`Tray Full`) |

`Motor Error` blocks cleanings, automatic or manual, until the `Clear Motor Error` button is pressed (leveling stays allowed). `Motor Error Reason`, `Drum Stop Reason` and `Tray Stop Reason` show the codes as text.

The MCU repeats the stopped status of a command, sometimes with a new position (a flap released after a failed close moves on to 9, then 10). Only the first stopped status of each command is reported as its stop, so each reaction runs once; `Tray Close Failed` is published before the stop code, so it already shows the first failure.

Hardware validation (28 October 2026, object placed in the bin so the flap can't close):

| Time | Event |
|---|---|
| 12:35:40 | `Tray` on: flap opened (tag 1, pos 7), then closed by `level_if_needed` (tag 2) |
| 12:35:48 | close stopped at pos 8, code 18 (8 s overtime) → flap reopened (tag 3) and closed again (tag 4) |
| 12:35:57 | second failure, code 18 → `Tray Close Failed` and `Tray Full` on |
| 12:36:57 | object removed: the flap closes (pos 9); `Tray Full` stays on until `Bin Emptied` |

That run showed the repeated statuses: the later status of tag 4 at pos 10 was taken as another failed close and the flap was reopened and closed once more; fixed by reporting the stop once per command.

Not ported: the stock pauses before resuming after a pet (6 s / 14 s; ESPHome resumes 5 s after `Approach` clears), and resuming the interrupted step where it stopped.

## Motor status feedback in Home Assistant

Motor statuses (`0x03`, see [mcu-protocol.md](mcu-protocol.md)) are parsed and logged at DEBUG (`Motor <n>: result=… scram=… pos=… cur=…`). Optional sensors of the `pkt4_mcu` sensor platform:

| Sensor (YAML key) | Default entity | Value |
|---|---|---|
| `drum_position` / `tray_position` | `Drum Position`, `Tray Position` | position marker: 1 level, 3 dump, 5 smooth, 7 tray open, 9 tray closed; even values are in between |
| `drum_stop_code` / `tray_stop_code` | `Drum Stop Code`, `Tray Stop Code` | `scram` of the last stopped status: 0 = destination reached, 1 = stop command / end stop; other values are MCU-side protections (4 and 5 observed, meanings partly unknown) |
| `drum_current` / `tray_current` | `Drum Current` (disabled by default) | raw current, unit unknown (~400–550 while the drum turns) |

Stopped statuses with zero runtime (acknowledgements of a stop sent while already idle) are ignored, so they do not overwrite the last real stop; a command's stop code is published once, from its first stopped status. Statuses carrying another command's `ctrl_ID` are ignored too (see [mcu-protocol.md](mcu-protocol.md)); the moves themselves don't depend on statuses, only what is published and the failed flap close detection do.
