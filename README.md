# Petkit T4 ESPHome Firmware

**Custom.** Fully replaces the stock firmware of the PetKit PURA MAX (T4) while introducing some extra neat features, as well as native Home Assistant integration.

It's able to work completely offline & without HA, although it's recommended to calibrate the load cell weight sensor before use (see [Weight sensor calibration](#weight-sensor-calibration)).


## Features

Behaves like the stock firmware:
- Cleaning after every visit of at least 15 s, the stock recipe: dump, pause, smooth, pause, level ([details](#automatic-cleaning))
- Leveling and maintenance moves following the stock recipes, every drum and flap stop done by the MCU ([details](#motion))
- Full waste bin detected from a failed flap close ([details](#full-waste-bin-and-motor-errors))
- Abnormal motor stops handled as stock: drum stall back-off and retries, flap close retry ([details](#full-waste-bin-and-motor-errors))

Extra features:
- Native Home Assistant integration: settings, manual motor controls and many internal sensors (motor positions, stop codes, weight, presence)
- Top (menu) button: Maintenance mode (double click) / Reboot (hold)
- Bottom (ok) button: Maintenance done / Level the litter
- Advanced status displaying, QR code with a Wi-Fi AP connection shortcut
- Quieter functioning
- Collected waste amount tracking
- Beep after reaching set amount of litter in maintenance mode
- `Block Movement` switch, and additional safety checks compared to the stock fw (weight, cover, bin, pet presence)
- Optional [Improv-BLE](https://improv-wifi.com) support


## Automatic cleaning

By default (`Clean After Every Visit`), a cleaning is planned after every visit of at least 15 s, whether the pet left waste or not, as the stock firmware does; the waste weight is not looked at. Turning the switch off restores the weight-based decision (`Clean Threshold`).

The cleaning starts `Clean After` minutes after the visit. The delay restarts each time `Approach` clears, and the cleaning waits for the pet to leave if it is still in front or inside ([details](docs/motion.md#fixed-pending-clean-dropped-when-a-pet-is-in-front)). Details and comparison with the stock decision: [docs/cleaning.md](docs/cleaning.md).


## Motion

The drum and the waste flap are driven the way the stock firmware does it: each move carries a **destination** (`pos`) and **the MCU stops the motor itself** when it reaches the matching hall sensor. Previously ESPHome started the drum without a destination and sent a stop when it saw the sensor; the latency let the drum overshoot the narrow level window and stop between positions, which left the litter box stuck with the leveling animation.

| Destination (`pos:` of `pkt4_mcu.motor`) | Code | Used for |
|---|---|---|
| `level` | 1 | leveling, end of cleaning |
| `dump` | 2 | cleaning, way up; manual `Drum Up` |
| `smooth` | 3 | cleaning, way down; leveling from the dump side; manual `Drum Down` |
| `open` / `closed` | 4 / 5 | waste flap |

| Move | Sequence |
|---|---|
| Cleaning | `pos: dump` → 3 s pause → `pos: smooth` → 1.5 s pause → `pos: level` |
| Leveling | `pos: level`; from the dump side it goes past level to smooth first, then back to level |
| Maintenance | flap closed, then a relative move up ([details](docs/motion.md#maintenance-before--after)) |

ESPHome only sends stops for safety (pet, cover, bin, `Block Movement`, OTA) or when a command is turned off ([details](docs/motion.md#who-stops-the-motors)). When a pet is detected in front of the entrance, the MCU's own proximity protection stops the drum by itself; ESPHome also stops it on `Pet Inside` (weight) and resumes once `Approach` has been off for 5 s ([details](docs/motion.md#stopping-the-drum-when-a-pet-is-detected)).

Motor statuses reported by the MCU are decoded and exposed as diagnostic sensors: `Drum Position` / `Tray Position`, `Drum Stop Code` / `Tray Stop Code` with their `… Stop Reason` text, and `Drum Current` (raw, disabled by default); see [docs/motion.md](docs/motion.md#motor-status-feedback-in-home-assistant). The MCU UART is read robustly (2048-byte RX buffer, every buffered frame parsed each loop, resync on the frame magic).


## Full waste bin and motor errors

The stock firmware has no fill sensor: it infers a full bin when the waste flap fails to close. This firmware does the same: the first failed close reopens and closes the flap again, the second one turns `Tray Full` on. `Tray Full` stays on until the `Bin Emptied` button is pressed, and no cleaning, automatic or manual, starts meanwhile ([details](docs/cleaning.md#full-waste-bin)).

Other abnormal motor stops are handled as stock ([details](docs/motion.md#abnormal-stops)):

| Stop | Reaction |
|---|---|
| Drum stall | pause 14 s, back off and retry, up to 3 times; then `Motor Error` and one leveling attempt |
| Drum timeout | `Motor Error`, cleaning stopped |

`Motor Error` blocks cleanings until the `Clear Motor Error` button is pressed; `Motor Error Reason` tells why.


## Weight sensor calibration

1. Place the T4 on a reasonably flat surface, make sure it's standing on all four its feet, as load cells are attached to each of them.
2. Press the `Weight Calibration Zero` configuration button until the `Weight` diagnostic sensor reads positive zero exactly.
3. Put a precisely (to 0.01 kg) known weight on top of the T4.
4. Enter its exact weight in decimal kilograms to `Weight Calibration` configuration number input **and press enter**.
5. Remove the weight and check for exact positive zero again, press the button again to reach it.
6. Place the weight again and check for its weight drift, re-input the value if needed (you might need to add/remove a trailing non-significant zero in order for the value to be actually sent and saved).
7. Rinse, repeat as desired (a perfect 0.01 kg precision is known to be reachable on T4).

The factory calibration of each unit can be extracted from a stock flash dump with [`tools/extract_stock_calibration.py`](tools/extract_stock_calibration.py), or in a browser by dropping the dump on [`tools/stock-calibration.html`](tools/stock-calibration.html); it is not used by the firmware yet ([details](docs/stock-calibration.md)).


## Documentation

- [docs/cleaning.md](docs/cleaning.md): automatic cleaning decision, full waste bin, comparison with the stock firmware.
- [docs/motion.md](docs/motion.md): drum and flap commands, destinations, abnormal stops, fixes and hardware tests.
- [docs/mcu-protocol.md](docs/mcu-protocol.md): frame format, packet types, status bits, sensor words, init sequence, heartbeat and link supervision.
- [docs/stock-calibration.md](docs/stock-calibration.md): factory weight calibration and the tools (Python script, browser page) that extract it from a stock flash dump.


## Roadmap

Cleaning ([docs/cleaning.md](docs/cleaning.md)):
* [x] Auto cleaning start (a.k.a. non-kitten mode)
* [x] Clean after every visit, as stock, instead of on waste weight
* [x] A pending clean waits for the pet to leave instead of being dropped
* [x] Full waste bin detected from a failed flap close, cleanings blocked until `Bin Emptied`
* [ ] Minimum interval between two cleanings
* [ ] Do-not-disturb hours
* [ ] Stock minimum pet weight of 1.5 kg (`Pet Threshold`, 1 kg, is used instead)

Motion ([docs/motion.md](docs/motion.md)):
* [x] Every drum and flap move carries a stock MCU destination and is stopped by the MCU (no more overshoot or stuck leveling screen)
* [x] Cleaning, leveling (via smooth from the dump side) and maintenance follow the stock recipes
* [x] Pet detection: the MCU stops the drum by itself; no spurious resume when passing level
* [x] Abnormal stops handled as stock: drum stall back-off, timeouts raise `Motor Error`, flap close retried once (validated on hardware)
* [x] `Block Movement` blocks every move on its own
* [x] A cleaning interrupted by a pet at dump carries on, and an aborted OTA no longer blocks moves until a reboot
* [ ] Resume an interrupted step where it stopped (stock keeps the step context, `FUN_4011c1ac`); a cleaning interrupted by a pet restarts from the way up to dump
* [ ] Detect a failed flap opening (only a failed close is detected)
* [ ] Leveling: 0.5 s pauses, final 30-step move, and the swing via smooth even when already level
* [ ] Maintenance: the 900 s wait step and the exit with the OK button
* [ ] Recipe variants: flap homing before opening, relative moves and shakes (the selecting settings are not identified)
* [ ] Scram mask switched per step (static `0x704C` for now)
* [ ] Tune the timeouts and speeds

MCU link ([docs/mcu-protocol.md](docs/mcu-protocol.md)):
* [x] All packet types sent by the MCU identified; motor statuses decoded and exposed to HA
* [x] Robust UART reading (no lost frames during motion)
* [x] Report an error when the MCU stays silent for 15 s, as stock does (`MCU Link Lost`)
* [x] Weekly MCU reboot (stock, packet `0xA`), only when idle; `Reboot MCU` button
* [x] Complete the heartbeat handshake as stock: init frames 20 ms apart, one sequence counter per frame type (`MCU Handshake`)
* [ ] Identify stock settings `0x12`, `0x23` and `3`, and the remaining stop codes
* [ ] Parametrized MCU init

Weight ([docs/stock-calibration.md](docs/stock-calibration.md)):
* [x] Weight as additional safety check
* [x] Dumping the stock calibration data from factory binaries (per-chip)
* [x] Browser page that extracts the stock calibration from a dump, no Python needed
* [ ] Use the extracted values for the weight (stock two-range conversion; validate with known masses before replacing the current calibration)
* [ ] Automated calibration
* [ ] Residual weight sits around -15 to -22 g after cleanings
* [ ] Weight reference rebasing like stock, with the deposit measured when the pet leaves. Low priority: cleanings no longer depend on the weight, only `Waste Collected` would benefit (a read-only simulation was tried and removed, commit `86c2ef6`)
* [ ] Detect the waste bin removal (not reported on T4-MAIN-V2.3; `Bin Emptied` button meanwhile)

Pets:
* [ ] Pet weight saving
* [ ] Multiple pets support


## Thanks to:

- @egormanga for the [original firmware](https://github.com/egormanga/esphome-petkit-t4) this fork builds on.
- @earlynerd with his [repo](https://github.com/earlynerd/petkit-pura-max-serial-bus) for the pioneering and _enormous_ help in the reverse engineering.
- @dwyschka for the idea and the upstart (earlynerd/petkit-pura-max-serial-bus#1).
