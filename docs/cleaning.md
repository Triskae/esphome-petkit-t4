# Automatic cleaning decision

## Clean After Every Visit (default)

Like the stock firmware, a cleaning is planned after every valid visit, whether the pet left waste or not. The waste weight is not looked at.

| Step | Behavior |
|---|---|
| Visit | `Pet Inside` on, then off |
| Valid visit | `Pet Inside` stayed on for at least 15 s (this includes its 5 s `delayed_off`); shorter visits are ignored, like the stock `PETIN TOO SHORT` |
| Pending clean | `Clean Pending` on for `Clean After`; restarted when `Approach` clears, and waits for the pet to leave if it is still in front or inside when the delay ends ([details](motion.md#fixed-pending-clean-dropped-when-a-pet-is-in-front)) |
| Cleaning | `Clean Times` cleanings, unless maintenance, `Tray Full`, `Block Movement` or an OTA prevents it |

Turning `Clean After Every Visit` off restores the weight-based decision: a cleaning is planned only when the residual weight exceeds `Clean Threshold`.

## Why

The stock firmware decides once, when the pet leaves, from the visit alone (`FUN_4011fc14`): visit duration, auto-clean settings and mode, minimum pet weight, interval since the last cleaning and do-not-disturb hours. None of its branches compares the weight after the visit with the weight before it. Its weight reference keeps tracking slow changes and also absorbs the deposit some tens of seconds after the visit, which does not matter since the cleaning request already exists.

ESPHome used the residual weight instead. In practice it caused problems: the residual drifts by tens of grams (it sat between -15 and -22 g after cleanings on 27 October 2026, hiding part of an 18 g deposit), and an unexplained +35 g step without a visit armed a cleaning. Decisions based on the visit avoid both.

Not ported from the stock decision: minimum interval between cleanings, do-not-disturb hours and the stock minimum pet weight (1.5 kg; `Pet Threshold` is used instead).

Waste accounting (`Waste Collected`) still uses the weight; `Tray Full` also uses it, see below.

## Full waste bin

The stock firmware has no fill sensor: it infers a full bin when the waste flap fails to close at the end of a cleaning (`MOTO_DOOR_CLOSE timeout` → `clean WASTEBOX_FULL!`). This component does the same, and also blocks the drum.

| Entity | Meaning |
|---|---|
| `Tray Close Failed` (diagnostic) | result of the last flap close: on when the tray motor stopped before the closed position (status position ≠ 9); the first failure reopens and closes the flap again, the second one sets the full state |
| `Tray Full` | on when the flap failed to close (latched, survives reboots) **or** `Waste Collected` ≥ `Tray Capacity` |
| `Bin Emptied` (button) | to press after emptying the bin: resets `Waste Collected` and clears the latched full state |

While `Tray Full` is on, no cleaning starts, automatic or manual. A cleaning already running can still resume after a pet interruption, so the drum is never left at dump. Leveling stays allowed.

Differences with the stock firmware:
- Stock reports the full bin (display, app) and clears it after the next successful close; no path blocking the next cleaning was found. Here the full state is latched until `Bin Emptied`, so a later close that pushes through doesn't dump again into a full bin.
- A close interrupted by the cover, the bin removal or maintenance is not counted as a full bin.
- `Bin Emptied` is needed because removing the bin is not reported on every unit (`Bin` never changes on a T4-MAIN-V2.3).

Validated on hardware with an object blocking the flap (retry, then `Tray Full`, see [motion.md](motion.md#abnormal-stops)); not yet with a really full bin. Don't block the flap by hand to test it.
