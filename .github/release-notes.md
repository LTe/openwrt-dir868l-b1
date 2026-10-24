The image `openwrt-bcm53xx-generic-dlink_dir-868l-b1-squashfs.bin` is both the factory image (the
stock D-Link web UI and recovery page accept it) and the `sysupgrade` image.  It includes LuCI
(HTTP and HTTPS) and the Broadcom `wl` WiFi stack for both radios.

**Install from stock D-Link firmware:** log in to http://192.168.0.1, Tools → Firmware, upload
the `.bin`, wait about 3 minutes.  **Recovery mode** (works from any firmware): power off, hold
Reset, power on and keep holding until the power LED blinks, set your PC to 192.168.0.2/24, open
http://192.168.0.1 and upload the `.bin`.  **From OpenWrt:** LuCI System → Backup / Flash Firmware,
or `sysupgrade -n <file>.bin` (use `-n` when coming from a different build).

First boot: http://192.168.1.1, user `root`, no password.  Both radios are present but disabled;
enable them under Network → Wireless.  Full instructions: the README.
