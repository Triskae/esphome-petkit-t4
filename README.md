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
* [ ] Extra unknown packets (all packet types sent by the MCU are now identified, see [docs/mcu-protocol.md](docs/mcu-protocol.md))
* [ ] Better & more failsafe motor control (drum stops are now done by the MCU and motor statuses are exposed to HA, see [Drum motion](#drum-motion); they do not drive retries yet)
* [ ] Parametrized MCU init
* [ ] Automated calibration
* [ ] Dumping the stock calibration data from factory binaries (per-chip)
* [x] Weight as additional safety check
* [ ] Tune the timeouts and speeds


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
| Now | `pos: level`, stopped by the MCU; if already level, leveling just finishes | `pos: dump` → 3 s pause → `pos: smooth` → 1.5 s pause → `pos: level`, every stop by the MCU |

The MCU UART is also read more robustly (2048-byte RX buffer, every buffered frame parsed each loop, resync on the frame magic); `RX bytes dropped` is shown in the component config dump. Details and hardware validation: [docs/motion.md](docs/motion.md).

Motor statuses reported by the MCU are decoded and exposed as diagnostic sensors: `Drum Position` / `Tray Position` (position marker), `Drum Stop Code` / `Tray Stop Code` (why the motor stopped: destination, stop command, MCU-side protection) and `Drum Current` (raw, disabled by default). See [docs/motion.md](docs/motion.md#motor-status-feedback-in-home-assistant).

When a pet is detected in front of the entrance, the MCU's own proximity protection (scram mask bits 12/13) stops the drum by itself (verified on hardware, ~140 ms before ESPHome's own stop on `Approach`); ESPHome also stops it on `Pet Inside` (weight) and resumes once `Approach` has been off for 5 s. Passing the level sensor during a cleaning no longer triggers a spurious "resume" ([details](docs/motion.md#fixed-resume-command-while-passing-level)). A pending automatic clean is no longer dropped when a pet is in front as its delay ends: the delay restarts each time `Approach` clears, and the clean waits for the pet to leave ([details](docs/motion.md#fixed-pending-clean-dropped-when-a-pet-is-in-front)). Comparison with the stock behavior: [docs/motion.md](docs/motion.md#stopping-the-drum-when-a-pet-is-detected).


## Documentation

- [docs/motion.md](docs/motion.md): drum commands, destinations, before/after, hardware tests.
- [docs/mcu-protocol.md](docs/mcu-protocol.md): frame format, packet types, status bits, sensor words, init sequence, heartbeat and link supervision.
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
