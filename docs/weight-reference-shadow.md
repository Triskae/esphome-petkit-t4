# Weight-reference shadow diagnostics — individual-sample model (v2)

This is an observational simulation. It never changes actual calibration, litter
or waste accounting, occupancy, cleaning demand, or motor commands. Existing
entity names are retained; their history before the v2 installation describes
the older averaged-packet approximation and must not be mixed with v2 counters.

## Scope and remaining differences from stock

The simulation reproduces the identified **ordinary idle rebase block** and the
software barrel-removal detector. It receives each individual 24-bit ADC sample
from a validated type-7 UART packet. The existing averaged Weight sensor and
its filters remain separate. The internal sample sensor drops its filtered
output, so ADC samples are not exposed as HA entities or frontend updates.

The complete stock presence state machine, special `f_litter_add` modes, the
conditional reference write at visit exit, added-litter accounting and persistent
tare operations are **not ported**. Presence and process preconditions come from
ESPHome entities. In particular, ESPHome cleaning, maintenance, manual drum
movement, leveling and open tray are mapped to busy. This is an explicitly
bounded model, not a bit-for-bit simulation of the whole original firmware.
`Reference Model Scope Shadow` states that limitation in HA.

The four `shadow_stock_*` substitutions are diagnostic calibration values
extracted from **this device's** stock dump: zero 7217189, midpoint 6557189,
66/65 ADC counts per gram. They select the stock integer comparisons. They do
not overwrite ESPHome's restored `weight_zero` or `weight_scale`. Other devices
must supply their own extracted values before interpreting this model.

Candidate reference, residual and correction are displayed using the current
ESPHome conversion, so they remain comparable to the actual residual. Counter
thresholds and the barrel detector use the stock two-segment conversion.

## Reproduced rules

- Acquire an initial raw reference after more than 30 successive comparisons
  below 80 g. A local startup/known-state/absence/idle/cap guard prevents an
  uncertain boot baseline; that extra guard is not claimed to be stock code.
- Count successive differences below 100 g; a difference of at least 100 g
  resets the rebase counter. Integer conversion precedes the comparison.
- Use limit 200 normally, or 3500 when the converted increase is at least
  300 g. Commit is attempted only after exceeding the limit: 201 or 3501
  qualifying increments. A large decrease still uses the short counter.
- Presence resets the counter. A busy process pauses it in the modeled idle
  block; raw changes may still reset it. The stock full state machine can
  introduce additional resets that this model does not reproduce.
- The cap and simulated barrel-out state block **commit**, not counting.
  A blocked attempt has already consumed/reset its counter.
- Copy any different raw reference: the old experimental 5 g deadband is removed.
- A pending cleaning demand is recorded with the event, not used as an extra
  stock counting inhibition. This can reveal a rebase that would absorb a real
  deposit; it must not automatically become an active compensation.
- Accounting changes do not automatically re-anchor the raw reference. Real
  calibration changes restart initialization of the diagnostic reference.
- Invalid samples, unknown required HA states, OTA, or a UART gap over five
  seconds stop/reset the observation window. These fail-closed diagnostic
  guards are additional to the modeled stock block. All state is RAM-only.

The barrel detector uses the stock cap/home branches, 3000 g / 1000 g checks,
retained stable raw and the six observations that lead to its saturated
five-count state. Its result is used on the next observation, following the
stock task's ordering. It is a simulated inference, not a new physical sensor.

## Monitoring entities

| Entity name | Meaning |
| --- | --- |
| Weight Reference Shadow | Candidate total-weight reference, kg |
| Residual Weight Shadow | Latest individual weight minus candidate, g |
| Weight Drift Shadow | Candidate minus actual litter/waste accounting, g; not proof of physical drift |
| Reference Stability Shadow | Current counter, retained while paused |
| Reference Required Samples Shadow | Counter limit, 200 or 3500; attempt occurs after exceeding it |
| Reference Updates Shadow | Number of raw-reference changes, excluding initialization |
| Reference Last Update Shadow | Local time of latest simulated rebase |
| Reference Eligible Shadow | Currently counting, with fresh samples; commit may still be blocked |
| Reference Commit Allowed Shadow | Counting and cap/barrel permit commit |
| Reference Samples Seen Shadow | Individual observations received; includes rejected samples |
| Reference Attempts Shadow | Counter expirations, including unchanged or blocked attempts |
| Reference Blocked Attempts Shadow | Attempts refused by cap/barrel |
| Reference Status Shadow | Counting, paused, reset, blocked, initialization or invalid/stale input |
| Barrel Removed Shadow | Simulated stock barrel-out inference |
| Reference Last Decision Shadow | Timestamped event snapshot; authoritative event journal |
| Reference Last Actual Residual Shadow | Existing Pet Weight value sampled at latest decision, g |
| Reference Last Residual Before Shadow | Same actual value minus the old simulated correction, g |
| Reference Last Residual After Shadow | Same actual value minus the new simulated correction, g |
| Reference Would Clear Threshold Shadow | Latest event was a rebase causing a downward threshold crossing |
| Reference Model Scope Shadow | Reminder of implemented and delegated parts |

### Answering “would this rebase have resolved that threshold problem?”

A hypothetical event can look like:

```text
2026-10-26 12:34:56 +0100 rebase real=47.0g before=25.0g after=0.0g threshold=20.0g clear=yes pending=yes
```

These numbers are an example, **not a measured event**. It says that the latest
actual residual was 47 g, the existing simulated correction already reduced it
to 25 g, and this particular rebase would bring it to 0 g, crossing below 20 g.
`clear=yes` requires the actual value to be above the cleaning threshold and
below the pet threshold, the simulated before-value above the cleaning threshold,
and the after-value at or below it. Initialization, blocked attempts, unchanged
raw, missing real residual and an already-below-threshold simulation never count
as a newly successful crossing. The cleaning threshold used is in the journal.

This tests a **numerical counterfactual**, not whether the whole cleaning bug is
fixed. A correction could hide real waste. Correlate these events with actual
Pet Inside, Clean Pending, Clean, and the existing residual/alert histories.
The lost-cleaning-demand race remains an independent issue in the current YAML.

HA records each published Last Decision text when Recorder includes the entity.
The numeric snapshots, status and counters refresh once per second; the text
snapshot is published directly for each decision and preserves the values from
that event. The real Pet Weight snapshot itself comes from its existing one-second
update, so it is not synchronized to the individual ADC sample. The timestamp
uses HA time if available, otherwise uptime; unknown time is never fabricated.

After restart the simulated reference and counters restart. Ordinary attempts
with identical raw are journaled as `raw_unchanged`, not as corrections. Stale
Residual Weight becomes unknown; the reference, last event and historical
snapshots are retained. No scheduled notification or HA automation is added.

## Validation and rollback

Host checks include individual samples, initialization, both calibration slopes,
integer 299/300 g boundary, short/long counters, pause/reset/late commit guards,
barrel removal/reinsertion, subgram changes, accounting/calibration changes,
invalid/stale/unknown/OTA inputs, millis wrap and event threshold semantics.

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I. \
  tests/weight_reference_observer_test.cpp -o /tmp/petkit-weight-reference-test
/tmp/petkit-weight-reference-test
esphome compile petkit.yaml
```

A pre-v2 local checkpoint, including sources and the previous compiled OTA and
factory binaries, is preserved under `.esphome/checkpoints/before-shadow-v2-*`.
`SHA256.json` lists saved files. It contains no copy of `secrets.yaml`. Returning
to the saved OTA restores the older ESPHome observer version, not stock firmware.
Compilation/host tests do not replace runtime verification of sample reception,
HA entity availability, event history and representative visits/cycles.

## Runtime verification — 26 October 2026

Installed over native encrypted OTA on the same device (MAC
`a0:a3:b3:7b:68:48`). DeviceInfo confirmed compilation
`2026-10-26 09:32:44 +0100`; HA discovered the added entities and Recorder
returned actual Last Decision history. Initial reference was recorded at
09:38:04, followed by simulated rebase decisions. At the 09:38:59 check,
750 individual observations, three attempts and three reference updates were
reported. The 09:38:55 decision had real residual -3.9 g, simulated before
0.5 g and after -0.1 g, `clear=no`, `pending=no`: this is a small simulated
adjustment, not evidence of solving an above-threshold problem.

Actual litter mass remained 5759.1875 g and collected waste 301.948059 g,
matching the pre-upload snapshot; cleaning was off after reconnect. No
representative cat visit or cleaning-cycle validation has yet been performed
with v2. Ignored local validation snapshots and hashes are under
`.esphome/checkpoints/shadow-v2-validation/`.
