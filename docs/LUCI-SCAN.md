# Native Broadcom scanning in LuCI

`patches/0005-iwinfo-wl-native-scan.patch` adds scanning to the iwinfo `wl`
backend. The previous implementation called Wireless Extensions, which the
relinked driver does not implement. LuCI also passes UCI radio section names
(`wlblob0`, `wlblob1`), so the backend now resolves a `broadcom` radio's `ifname`
before issuing ioctls. UCI contexts are released after each lookup.

The backend starts `WLC_SCAN` with a broadcast BSSID, polls `WLC_GET_CHANNEL`
for completion (up to eight seconds), and retrieves `WLC_SCAN_RESULTS` into a
64 KiB buffer. It does not restart the radio, change AP/STA mode or modify UCI.
An active scan temporarily takes the radio off its serving channel.

BSS records in formats 108/109 are translated into iwinfo entries: SSID, BSSID,
control channel, frequency, signal, quality, security and HT/VHT operation.
WPA/RSN elements use iwinfo's existing security parser. Seeing an SAE/WPA3
network in scan results does not add WPA3 client support to the driver.

Record lengths, SSID length, IE offsets and IE lengths are checked before use.
Unaligned records are supported. Unknown record versions, malformed results,
failed ioctls and scan timeouts return an error to the caller. The backend does
not silently reuse cached scan results. The existing rpcd scan endpoint may
represent backend errors as an empty result list.

## Validation

Validated on DIR-868L B1 with the FreshTomato 6.37.14.126 core, Linux 6.18.55:

- The OpenWrt iwinfo package builds successfully as release `r2`.
- The package was installed on the router and rpcd restarted.
- Both raw netdev names and the UCI radio names used by LuCI return networks
  through `ubus call iwinfo scan`, with signal, channel and security details.
- The UCI-name check returned 24 networks on 2.4 GHz and 12 on 5 GHz in one
  pass; counts vary between scans.
- The final installed library matches the stripped package artifact (SHA-256
  `1d7f899ae7105f9e01234cd524762845e30f80c4de3eb60c863881b28981895d`).
- `wlblob-check` reports 33 passes, zero failures and one existing country-list
  warning on the 2.4 GHz radio.
- Host parser tests pass under AddressSanitizer and UndefinedBehaviorSanitizer:
  ABI offsets, WPA/WPA2, open/WEP, hidden SSIDs, invalid lengths/versions,
  truncated IEs, empty results and 131,072 single-byte mutations.

The existing client configuration was not changed and no new network was
joined as part of this test. Validation exercised LuCI's RPC calls; it did not
automate a browser click.

To reproduce parser tests after preparing/building iwinfo, pass the actual
source and target staging directories from your build:

```sh
tests/test-iwinfo-wl-scan.sh \
  openwrt/build_dir/target-arm-openwrt-linux-muslgnueabi_musl/libiwinfo-2026.05.26~66bdd1a0 \
  openwrt/staging_dir/target-arm-openwrt-linux-muslgnueabi_musl
```

To check the same radio names LuCI uses on the router:

```sh
ubus -t 15 call iwinfo scan '{"device":"wlblob0"}'
ubus -t 15 call iwinfo scan '{"device":"wlblob1"}'
```
