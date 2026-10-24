# OpenWrt for the D-Link DIR-868L B1, with WiFi

Builds an OpenWrt image for the DIR-868L B1 (BCM47081 + BCM4360 5 GHz + BCM4331 2.4 GHz) in which
both radios work: AP (one or more SSIDs), station mode, WPA2-PSK, LuCI status.  The in-kernel
`b43` driver cannot drive these radios; this uses Broadcom's proprietary `wl` core, re-linked
against the current kernel, with the surrounding userland built from the FreshTomato GPL sources.

Nothing proprietary is stored in this repository.  At build time the driver core (`wl_apsta.o`,
wl 6.37.14.126) and the `wl` control tool are downloaded from the public FreshTomato ARM source
tree at a pinned commit and verified by sha256.

## Layout

| path | what |
|---|---|
| `openwrt/` | upstream OpenWrt, git submodule pinned to a commit |
| `patches/` | applied on top of it: DIR-868L B1 device support (PR openwrt/openwrt#20684), a netifd fix, iwinfo `wl` backend |
| `feed/` | an OpenWrt package feed: `kmod-brcm-wl-blob` (driver), `brcm-wl-tools` (nas, eapd, wlconf, wl, libnvram), `wlblob-utils` (netifd integration) |
| `config.seed` | the `.config` delta: target, device, these packages, LuCI |
| `Makefile` | glue: submodule, patches, feed, config, build, copy images to `out/` |
| `.github/workflows/build.yml` | CI: same steps in upstream's build container with the prebuilt toolchain |

## Build

Needs the usual [OpenWrt build prerequisites](https://openwrt.org/docs/guide-developer/toolchain/install-buildsystem).

    make            # first build compiles the toolchain too; count on an hour or more
    ls out/         # openwrt-bcm53xx-generic-dlink_dir-868l-b1-squashfs.bin + manifest + sha256sums

The image is a SEAMA container: the stock D-Link web UI accepts it as a firmware update, and on
OpenWrt it is the `sysupgrade` image.

Useful knobs: `make menuconfig` then `make image`; `make saveconfig` to persist the config delta;
`V=s` for a verbose build; `TOOLCHAIN_DIR=$(scripts/get-toolchain.sh)` to use OpenWrt's prebuilt
toolchain instead of building one.

### Updating OpenWrt

    git -C openwrt fetch origin && git -C openwrt checkout origin/main
    make prepare    # patches must still apply; drop patches/0001 once the device PR is merged
    make image
    git add openwrt && git commit -m "openwrt: bump"

## Using the WiFi

The radios appear as ordinary `/etc/config/wireless` devices of type `wlblob` (`wl0` = 2.4 GHz,
`wl1` = 5 GHz), created by `wifi config`.  Supported: `mode ap` (several per radio), `mode sta`,
`encryption none|psk|psk2|psk-mixed`, `channel`, `htmode` (HT20/40, VHT20/40/80), `hidden`,
`isolate`, `maxassoc`, `txpower`, `country`.  Not supported: WPA3, 802.11r, mesh, monitor.

Under the hood: `kmod-brcm-wl-blob` exposes `wlN` netdevs and an emulated `/dev/nvram`;
`/lib/netifd/wireless/wlblob.sh` configures the radio through `wl` and writes the `wlN_*` nvram
variables; `eapd` and `nas` (authenticator for APs, supplicant for stations) do WPA.  Radio
calibration data comes from the stock firmware's NVRAM defaults (`/usr/share/wlblob/wl-nvram.tmpl`)
plus the MAC addresses from the `devdata` partition.

## Driver core variants

`kmod-brcm-wl-blob` defaults to the FreshTomato core.  The D-Link stock core (wl 6.37.14.56, AP
only) can be used instead by passing `WLBLOB_VARIANT=dlink WLBLOB_DLINK_KO=/path/to/wl_ap.ko`
(extracted from the 2.03B01 firmware) to `make`.

## Credits

- [northstar-broadcom-revival](https://github.com/umair-gititall/northstar-broadcom-revival): the
  same idea for Northstar boards; its vendored nas/eapd/wlconf/libshared sources and the uClibc
  shim for `wl` are what `brcm-wl-tools` builds.
- FreshTomato for keeping the SDK 6.37 sources and binaries public.
