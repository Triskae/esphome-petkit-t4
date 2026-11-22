# Local menu

The two buttons drive a menu shaped like the stock one. Each page shows an icon above its name, with dots giving the position in the menu.

## Behavior

| Press | Menu closed | Menu open |
|---|---|---|
| Menu, click | opens the menu on `Clean` | next item; after `Wi-Fi` the menu closes |
| OK, click | `Level` (or ends maintenance) | starts the selected item and closes the menu |
| Menu, hold 2 s | reboot | reboot |

| Item | OK does | Stock page |
|---|---|---|
| Clean | turns `Clean` on | 22, process 0 |
| Level | presses `Level` | 24, process 4 (smooth) |
| Empty Litter | asks to confirm ("OK to empty", 10 s); a second OK turns `Empty Litter` on | 23, process 1, not in the stock menu ([details](motion.md#empty-litter)) |
| Maintenance | toggles `Maintenance` | 26, process 9 |
| Bin Emptied | presses `Bin Emptied` | none: replaces stock page 18 (process 3, drum reset) |
| Wi-Fi | shows the fallback AP QR code for 60 s; any button closes it | 17, starts the PetKit app pairing (page 27) |

The menu closes after 10 s without a press, like the stock pages (timeout `0x2710` ms in the page table). It does not open while a cleaning, the litter emptying or an OTA runs, and closes if one starts. The actions keep their own safety checks: an item refused by a check (pet, cover, bin, full bin, motor error) simply does nothing.

Differences with stock:
- The stock drum reset (process 3) brings the drum back to its start position; `Level` already does that here, so the item is replaced by `Bin Emptied`, which otherwise needs Home Assistant.
- `Empty Litter` is added after `Level`. Its stock page (23, `i_dmp`/`s_dmp`) exists but no menu transition reaches it in this build: stock starts it from the app only. Since it tips all the litter into the bin, the menu asks for a second OK.
- Stock Menu double click is the same event as a click (18); the former double click to enter maintenance is gone, maintenance is in the menu.
- Stock Menu hold turns the software power off; here it still reboots.
- Not reproduced: the child lock (pages 3 and 4), and the odor and light accessory pages (19 to 21), only reachable when an accessory is detected.
- Labels are text in Ubuntu Mono and Material Design icons. The stock bitmaps (`i_cln`, `s_cln`, …) are not copied.

## Stock evidence

Application `petkit-stock-ota_1.bin`, SHA-256 `6e3263521cfad147b645c69245add407dba73df82b6575a7ab27aa13ea05c0b1`. Research notes and exports are in the separate `petkit-ghidra-setup/analysis` folder (`LOCAL_CONTROL_RESEARCH.md`, `full-fw-evidence/local-0004`).

- Page switch `FUN_40118cc8` builds a page from the page table at `0x3f40f680` (pointer literal `0x40109f6c`, 0x150 bytes per page: id, priority, timeout in ms at +8, widget count at +0xc, widgets from +0x10). Widgets come from the table at `0x3f4151f8` (0x5c bytes each, bitmap names from +0xc).
- Menu pages are an icon widget (y 22, 73 px high) and a text widget (y 95, 30 px high), with a 10 s timeout: 17 `i_wifi`/`s_wific`, 18 `i_rst`/`s_rst`, 19 `i_rmv_od`/`s_od_rmv`, 20 and 21 `i_light`, 22 `i_cln`/`s_cln`, 24 `i_smth`/`s_smth`, 26 `i_mten`/`s_mten`. Page 23 (`i_dmp`/`s_dmp`) and 25 (`i_bk2main`) have no menu transition in this build.
- Per-page handlers: table `0x3f40e2a0` (literal `0x4010a340`), indexed by page id minus one, in the interaction task `FUN_4011a5dc`. Event 18 is a Menu click, 19 an OK click, 23 the page timeout.

| Page | Menu click (18) | OK click (19) | Handler |
|---|---|---|---|
| 1 main | page 22, or lock page 4 when locked | lock page 4 when locked | `4011ae2e` |
| 22 Clean | page 19 if an accessory is present, else 24 | `40132764(1,3,0,0)` | `4011af4b` |
| 19 Odor | page 20 or 21 (light state) if an accessory is present, else 24 | `40132764(1,3,2,0)` | `4011afcf` |
| 20 / 21 Light | page 24 | `40132764(1,3,7,0)` | `4011b060` / `4011b0bc` |
| 24 Level | page 26 | `40132764(1,3,4,0)` | `4011b0d4` |
| 26 Maintenance | page 18 | `40132764(1,3,9,0)` | `4011b1bb` |
| 18 Reset | page 17 | `40132764(1,3,3,0)` | `4011b118` |
| 17 Wi-Fi | page 1 | page 27, then `401326fc` | `4011b15b` |

The third argument of `40132764` matches the process recipe indices of the motion research: 0 cleaning, 3 / 4 reset and alignment, 9 maintenance.

Not validated on hardware yet.
