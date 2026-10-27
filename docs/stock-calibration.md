# Stock weight calibration

> **Disclaimer:** the extracted values are not used by the firmware yet. `Weight` still relies on the component's own calibration; only the shadow diagnostics carry the stock values as constants. Using them is in the todo list.

Each T4 is calibrated at the factory. The stock firmware keeps that calibration in the NVS partition `user_calibrt`, namespace `T4`, key `data`: a 20-byte blob of five little-endian 32-bit integers.

| Field | Meaning | Example (T4-MAIN-V2.3) |
|---|---|---|
| `zero_raw` | raw ADC value at 0 g | 7217189 |
| `low_reference_raw` | raw value at 10 kg | 6557189 |
| `high_reference_raw` | raw value at 20 kg | 5907189 |
| `slope_low_raw_per_gram` | raw units per gram, 0–10 kg | 66 |
| `slope_high_raw_per_gram` | raw units per gram, 10–20 kg | 65 |

A lower raw value means more weight. The stock conversion uses two ranges with integer divisions: `(7217189 - 6557189) / 66 = 10000 g`, then `(6557189 - 5907189) / 65 = 10000 g` more.

## Extracting it

[`tools/extract_stock_calibration.py`](../tools/extract_stock_calibration.py) reads a **full, unencrypted flash dump taken while the stock firmware was installed** (keep one before flashing ESPHome), finds the partition table, decodes the NVS pages and checks every CRC. It is read-only and needs only the Python standard library.

```sh
python3 tools/extract_stock_calibration.py petkit-stock-full.bin > calibration.json
```

Options: `--partition`, `--namespace`, `--key`, and `--table-offset 0x8000` when the dump holds several partition tables. Anything that doesn't match the known T4 layout (encrypted partition, truncated dump, other schema, inconsistent values) is rejected with an `Extraction failed` message. An OTA application image is not enough: it has no partition table or NVS data.

## How it is used

Only the shadow weight-reference diagnostics use these values for now (substitutions `shadow_stock_*` in `petkit.yaml`, see [weight-reference-shadow.md](weight-reference-shadow.md)). The real `Weight` still uses the component's own calibration (`Weight Calibration Zero` and `Weight Calibration`). Don't replace that zero with `zero_raw` as is: the factory zero is the bare load cells, while the assembled device, the drum and the litter add their own tare. Using the two-range stock conversion for the weight is in the todo list.
