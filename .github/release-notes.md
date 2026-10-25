## What is in the image

`openwrt-bcm53xx-generic-dlink_dir-868l-b1-squashfs.bin` is both the factory image (the stock D-Link web UI and the recovery page accept it) and the `sysupgrade` image. It includes LuCI (HTTP and HTTPS) and the Broadcom `wl` WiFi stack for both radios.

## Install from the stock D-Link firmware

1. Connect a PC by cable to a LAN port and log in to http://192.168.0.1.
2. Tools → Firmware → Upload, select the `.bin`, start the upgrade.
3. Wait about 3 minutes without powering off. The router reboots into OpenWrt.

## Recovery mode (from any firmware, also to go back to stock)

1. Power off. Hold Reset, power on, keep holding until the power LED blinks (about 10 s), release.
2. Set the PC to a static address such as 192.168.0.2/24 and open http://192.168.0.1.
3. Upload the `.bin` (or D-Link's stock `DIR868LB1_FW*.bin` to revert) and wait for the reboot.

## From OpenWrt

LuCI: System → Backup / Flash Firmware → Flash image. Or from a shell:

```
sysupgrade -n openwrt-bcm53xx-generic-dlink_dir-868l-b1-squashfs.bin
```

Keeping settings is fine between releases from this repository. Use `-n` (or untick "Keep settings") when coming from a different OpenWrt build.

## First boot

- http://192.168.1.1 (also https://), user `root`, no password. Set one under System → Administration.
- Both radios are present but disabled. Set an SSID and WPA2 key under Network → Wireless and enable them.
- Full instructions and WiFi details: the README.

## Checksums
