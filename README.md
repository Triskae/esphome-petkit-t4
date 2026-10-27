# Petkit T4 ESPHome Firmware

https://github.com/user-attachments/assets/c50be11d-95dd-4898-9601-6adc21f130d6/#Firmware.Dreams.mp4


**Custom.** Fully replaces the stock firmware while introducing some extra neat features, as well as native Home Assistant integration.

It's able to work completely offline & without HA, although it's recommended to calibrate the load cell weight sensor before use.


## Extra features added

- Top (menu) button: Maintenance mode (double click) / Reboot (hold)
- Bottom (ok) button: Maintenance done / Level the litter
- Advanced status displaying
- Quieter functioning
- Collected waste amount tracking
- Beep after reaching set amount of litter in maintenance mode
- QR code with a Wi-Fi AP connection shortcut
- Optional [Improv-BLE](https://improv-wifi.com) support
- Various internal sensors exposed to HA
- Additional safety checks compared to the stock fw
- Manual motor controls


## Yet to implement

* [x] Auto cleaning start (a.k.a. non-kitten mode)
* [ ] Pet weight saving
* [ ] Multiple pets support
* [x] Extra unknown packets (all packet types sent by the MCU are identified, see [docs/mcu-protocol.md](docs/mcu-protocol.md); motor statuses are decoded and exposed to HA)
* [x] Better & more failsafe motor control (every drum and flap stop is done by the MCU, motor statuses are exposed to HA and a failed flap close is detected, see [Drum motion](#drum-motion); reacting to other failed moves is below)
* [ ] Parametrized MCU init
* [ ] Automated calibration
* [x] Dumping the stock calibration data from factory binaries (per-chip): [`tools/extract_stock_calibration.py`](tools/extract_stock_calibration.py), see [docs/stock-calibration.md](docs/stock-calibration.md)
  * [ ] Use the extracted values for the weight (stock two-range conversion; validate with known masses before replacing the current calibration). Not used yet
* [x] Weight as additional safety check
* [x] Drum and flap moves stopped by the MCU at stock destinations (no more overshoot or stuck leveling screen), see [docs/motion.md](docs/motion.md)
* [x] Robust MCU UART reading (no lost frames during motion)
* [x] Cleaning like stock: dump, pause, smooth, pause, level
* [x] Leveling like stock: via smooth when coming from the dump side
* [x] Maintenance like stock: flap closed, relative move up
* [x] Pet detection: the MCU stops the drum by itself; no spurious resume when passing level; a pending clean waits for the pet to leave instead of being dropped
* [x] Clean after every visit, as stock, instead of on waste weight (see [docs/cleaning.md](docs/cleaning.md))
* [x] Full waste bin detected from a failed flap close, cleanings blocked until `Bin Emptied` (see [docs/cleaning.md](docs/cleaning.md#full-waste-bin))
* [x] `Block Movement` blocks every move on its own
* [ ] Tune the timeouts and speeds
* [ ] Stock cleaning rules not ported yet (see [docs/cleaning.md](docs/cleaning.md)):
  * [ ] Minimum interval between two cleanings
  * [ ] Do-not-disturb hours
  * [ ] Stock minimum pet weight of 1.5 kg (`Pet Threshold`, 1 kg, is used instead)
* [ ] Stock motion (see [docs/motion.md](docs/motion.md)):
  * [x] Moves sent the stock way: every drum and flap move carries a stock MCU destination (level / dump / smooth, open / closed) and the MCU stops it itself; cleaning, leveling and maintenance follow the stock recipes
  * [ ] React to a move that ends without reaching its destination (stop on timeout or stall): retry with the remaining budget, then report an error, as stock does
  * [ ] Resume an interrupted step where it stopped (stock keeps the step context, `FUN_4011c1ac`); a cleaning interrupted by a pet restarts from the way up to dump
  * [ ] Detect a failed flap opening (only a failed close is detected)
  * [ ] Leveling: 0.5 s pauses, final 30-step move, and the swing via smooth even when already level
  * [ ] Maintenance: the 900 s wait step and the exit with the OK button
  * [ ] Recipe variants: flap homing before opening, relative moves and shakes (the selecting settings are not identified)
  * [ ] Scram mask switched per step (static `0x704C` for now)
* [ ] MCU link supervision (see [docs/mcu-protocol.md](docs/mcu-protocol.md)):
  * [ ] Complete the heartbeat handshake (the MCU keeps announcing `00`)
  * [ ] Report an error when the MCU stays silent for 15 s, as stock does
  * [ ] Weekly MCU reboot (stock, packet `0xA`)
  * [ ] Identify stock settings `0x12`, `0x23` and `3`, and the remaining stop codes (`4`; `5` = stall and `18` = close timeout are known)
* [ ] Weight:
  * [ ] Weight reference rebasing like stock (now only simulated by the shadow sensors), with the deposit measured when the pet leaves, before the reference absorbs it (see [docs/weight-reference-shadow.md](docs/weight-reference-shadow.md))
  * [ ] Residual weight sits around -15 to -22 g after cleanings
  * [ ] Detect the waste bin removal (not reported on T4-MAIN-V2.3; `Bin Emptied` button meanwhile)

## Drum motion

The drum is driven the way the stock firmware does it: each move carries a **destination** (`pos`) and **the MCU stops the drum itself** when it reaches the matching hall sensor. Previously ESPHome started the drum without a destination and sent a stop when it saw the sensor; the latency let the drum overshoot the narrow level window and stop between positions, which left the litter box stuck with the leveling animation.

| Destination (`pos:` of `pkt4_mcu.motor`) | Code | Used for |
|---|---|---|
| `level` | 1 | leveling, end of cleaning |
| `dump` | 2 | cleaning, way up |
| `smooth` | 3 | cleaning, way down |

| | Leveling | Cleaning |
|---|---|---|
| Before | drum moved without destination, stopped by ESPHome on the `Drum Level` edge (overshoot) | up and down without destination, stopped by ESPHome on the `Drum Up` / `Drum Down` edges, no pause |
| Now | `pos: level`, stopped by the MCU; from the dump side it goes past level to smooth first, then back to level, as stock; if already level, leveling just finishes | `pos: dump` → 3 s pause → `pos: smooth` → 1.5 s pause → `pos: level`, every stop by the MCU |

Every ordinary move (cleaning, leveling, manual `Drum Up` / `Drum Down`, waste flap) carries a destination, so ESPHome no longer stops the motors on hall sensors; it only sends stops for safety (pet, cover, bin, `Block Movement`, OTA) or when a command is turned off ([details](docs/motion.md#who-stops-the-motors)). Maintenance follows the stock recipe: flap closed, then a relative move up ([details](docs/motion.md#maintenance-before--after)). `Block Movement` now blocks every move on its own ([details](docs/motion.md#fixed-block-movement)).

The MCU UART is also read more robustly (2048-byte RX buffer, every buffered frame parsed each loop, resync on the frame magic); `RX bytes dropped` is shown in the component config dump. Details and hardware validation: [docs/motion.md](docs/motion.md).

Motor statuses reported by the MCU are decoded and exposed as diagnostic sensors: `Drum Position` / `Tray Position` (position marker), `Drum Stop Code` / `Tray Stop Code` (why the motor stopped: destination, stop command, MCU-side protection) and `Drum Current` (raw, disabled by default). See [docs/motion.md](docs/motion.md#motor-status-feedback-in-home-assistant).

When a pet is detected in front of the entrance, the MCU's own proximity protection (scram mask bits 12/13) stops the drum by itself (verified on hardware, ~140 ms before ESPHome's own stop on `Approach`); ESPHome also stops it on `Pet Inside` (weight) and resumes once `Approach` has been off for 5 s. Passing the level sensor during a cleaning no longer triggers a spurious "resume" ([details](docs/motion.md#fixed-resume-command-while-passing-level)). A pending automatic clean is no longer dropped when a pet is in front as its delay ends: the delay restarts each time `Approach` clears, and the clean waits for the pet to leave ([details](docs/motion.md#fixed-pending-clean-dropped-when-a-pet-is-in-front)). Comparison with the stock behavior: [docs/motion.md](docs/motion.md#stopping-the-drum-when-a-pet-is-detected).


## Automatic cleaning

By default (`Clean After Every Visit`), a cleaning is planned after every visit of at least 15 s, whether the pet left waste or not, as the stock firmware does; the waste weight is not looked at. Turning the switch off restores the weight-based decision (`Clean Threshold`). Details: [docs/cleaning.md](docs/cleaning.md).

A full waste bin is detected like the stock firmware does, when the waste flap fails to close (`Tray Close Failed`); `Tray Full` then stays on until the `Bin Emptied` button is pressed, and no cleaning, automatic or manual, starts meanwhile ([details](docs/cleaning.md#full-waste-bin)).


## Documentation

- [docs/cleaning.md](docs/cleaning.md): automatic cleaning decision, comparison with the stock firmware.
- [docs/motion.md](docs/motion.md): drum commands, destinations, before/after, hardware tests.
- [docs/mcu-protocol.md](docs/mcu-protocol.md): frame format, packet types, status bits, sensor words, init sequence, heartbeat and link supervision.
- [docs/stock-calibration.md](docs/stock-calibration.md): factory weight calibration and the tool that extracts it from a stock flash dump.
- [docs/weight-reference-shadow.md](docs/weight-reference-shadow.md): diagnostic simulation of the stock weight reference rebasing.


## Weight Sensor Calibration

1. Place the T4 on a reasonably flat surface, make sure it's standing on all four its feet, as load cells are attached to each of them.
2. Press the `Weight Calibration Zero` configuration button until the `Weight` diagnostic sensor reads positive zero exactly.
3. Put a precisely (to 0.01 kg) known weight on top of the T4.
4. Enter its exact weight in decimal kilograms to `Weight Calibration` configuration number input **and press enter**.
5. Remove the weight and check for exact positive zero again, press the button again to reach it.
6. Place the weight again and check for its weight drift, re-input the value if needed (you might need to add/remove a trailing non-significant zero in order for the value to be actually sent and saved).
7. Rinse, repeat as desired (a perfect 0.01 kg precision is known to be reachable on T4).


## Thanks to:

- @earlynerd with his [repo](https://github.com/earlynerd/petkit-pura-max-serial-bus) for the pioneering and _enormous_ help in the reverse engineering.
- @dwyschka for the idea and the upstart (earlynerd/petkit-pura-max-serial-bus#1).
- My loved girlfriend Sharea & my lovely kitty Xayah for the lots of testing.


---

https://github.com/user-attachments/assets/ad489f28-23f9-497a-a08b-c182428b3873/#Petkit.T4.ESPHome.Firmware.mp4
