# Wi-Fi 5 GHz status, 2026-11-24

**Status: partial.** The iOS 18 Wi-Fi stack scans 2.4/5 GHz, authenticates to `306` on
5 GHz (ch44 / 5220 MHz), then the AP rejects **association with 802.11 status 18**
("requested basic rate set is not supported"). `en0` stays on `169.254.x.x`, 0 RX.

## Device

- Redmi K20 Pro (`raphael`, SM8150), UniXNU + iOS 18.7.x, Heartwood `xnu-11215`.
- Boot args: `wifi=1 net_trace=9 wifi_lx=2 wifi_bdf=bd_f11cn.bin wifi_smmu=2 wifi_iokit=1
  wifi_scan=1 wifi_station=1 wifi_listen=15 wifi_ba=0 wifi_file=0 wifi_sae=0 wifi_wide=0`.
- `wifi_wide=0` keeps the request at 20 MHz; the AP is 80 MHz.

## Evidence

- 2.4 GHz data path works on `jiangjieshi` (U20): DHCP `10.250.85.151/24`, 18,518 RX frames /
  27.5 MB. The driver/netif layer itself is usable.
- `306` 5 GHz: `AUTHENTICATION status 0`, **`ASSOCIATION REFUSES status 18`**, 160 bytes of
  elements, station number 10812 (U24) / 16383 (U25).
- `en0`: `inet 169.254.25.97`, `status: active`, `link rate 100 Mbps`, `0 Ipkts`.
- `CMCC-3.306` / `.5G`: `wifid` refuses before the driver (TKIP / unsupported RSN); no `lx:`.

The 5 GHz AP: BSSID `3c:6a:48:98:bc:11`, `capabilities 0x1511`, HT/VHT/WMM yes,
`RATES [6,9,12,18,24,36,48,54]`, WPA2 PSK + CCMP, RSNE `30 14 01 00 00 0f ac 04 01 00 00 0f ac 04 01 00 00 0f ac 02 00 00`.

## Experiments

### U24 — restore VHT request element

File `0xC2DAD4`: `10 00 00 14` → `08 02 00 34`. VHT request is back; result still
**status 18**. Image MD5 `B4C73BBB9B38810D35A264FDDE147D1B`; kernel SHA256
`76E1F26FF4E3C26B0935DAB8170A4976AAB7DAEF2D92DEDD6F3D7552B16398F8`.

### U25 — mark every requested rate basic

- `0xC2D7DC`: `ce 25 c8 1a` → `0e 10 80 52` (`mov w14,#0x80`)
- `0xC2D7E0`: `ce 61 19 53` → `1f 20 03 d5` (`nop`)

Result: **still status 18**, station 16383, 160 bytes. So simply marking all rates basic
does not satisfy the AP; the request likely still contains 2.4-only rates (1/2/5.5/11), or
the 5 GHz `wiphy` band table is not the one `request_elements()` reads.
Image MD5 `EBBEF4C463AFE6680BC3B2CC65E9B424`; kernel SHA256
`97968031939F21637FB3B77E2A23B7B1CB1987D1773FCF49907CE3D52EC76267`.

### U26 — force a 5 GHz 8-rate table (built, **not flashed**)

Branches at `request_elements+0x4c` (`0x7C31758`) to code cave `0x754D3D8`, loads a fake band
descriptor at `0x754D420` with `+8` → kernel 5 GHz rate table `0xfffffe0007d393f0` and
`+28` → `n_bitrates=8`; forces basic mask `0x15` (6/12/24). This emits exactly
`6,9,12,18,24,36,48,54`. Image MD5 `26AC519A4B7CA60A11F81F7ED3871346`; kernel SHA256
`ED4EF7967F0C846F05881C25E546AE826C90E9A8C154276B9CA6A3116EC5DD0D`.

## Code path

`wifi/lx/src/lx_80211.c`:

- `station_abilities()` (~1313) reads the beacon rates and writes
  `sta->deflink.supp_rates[band->band]` / `vif->bss_conf.basic_rates`.
- `request_elements()` (~1368) uses `radio->wiphy->bands[channel->band]`, walks
  `band->n_bitrates` / `band->bitrates` into element 1 and element 50.
- `join()` (~1816) selects `channel` from `target->frequency`, then calls `request_elements()`.

## Questions for the original project

1. Is `wiphy->bands[NL80211_BAND_5GHZ]` populated with the 8-entry 5 GHz `ieee80211_rate`
   table, or does it share the 2.4 GHz table?
2. Is `vif->bss_conf.basic_rates` still valid when `request_elements()` runs, or is it reset
   by `bss_changed()` / `station_state()` between `station_abilities()` and `request_elements()`?
3. Has the iOS 16 tree seen 5 GHz status 18, and what fixed it?
4. With `wifi_wide=0`, should the request carry an HT bandwidth element for the 80 MHz AP?

## Reproduction

U24/U25 are single 4-byte edits above. U26 has a portable patcher:
`patch_u26.py` (same directory), usage
`python3 patch_u26.py kernel.u24-5g-vht kernel.u26-5g-rates`.

Boot images are 70,631,424 bytes and contain the iOS root/firmware; they are not published.
Use `tools/raphael/mkboot-raphael.sh` with the boot arguments above.

## Other known issues

- Sileo byg index restored but not retested.
- Safari exits after ~0.2 s (`NSURL initFileURLWithPath:isDirectory:` nil).
- U21 PSCI shutdown patch not verified on real power-off.
- `hfs_mark_inconsistent` on `iOSRoot` continues; `fsck` not run.
