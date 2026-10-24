#!/bin/sh
# `wifi config` detection for wlblob radios (wlN netdevs of the re-linked Broadcom core)
append DRIVERS "wlblob"

detect_wlblob() {
	local dev i band channel htmode devid
	[ -d /sys/module/wl ] || [ -x /etc/init.d/wlblob ] && /etc/init.d/wlblob start >/dev/null 2>&1
	for dev in /sys/class/net/wl[0-9]; do
		[ -d "$dev" ] || continue
		i="${dev##*/wl}"
		uci -q get wireless.wlblob$i >/dev/null && continue
		devid="$(wl -i wl$i revinfo 2>/dev/null | sed -n 's/^deviceid //p')"
		case "$devid" in
			0x43a1|0x4331|0x43b9) band=2g; channel=6; htmode=HT20;;
			*) band=5g; channel=36; htmode=VHT80;;
		esac
		uci -q batch <<-EOT
			set wireless.wlblob$i=wifi-device
			set wireless.wlblob$i.type=wlblob
			set wireless.wlblob$i.ifname=wl$i
			set wireless.wlblob$i.band=$band
			set wireless.wlblob$i.channel=$channel
			set wireless.wlblob$i.htmode=$htmode
			set wireless.wlblob$i.disabled=1
			set wireless.default_wlblob$i=wifi-iface
			set wireless.default_wlblob$i.device=wlblob$i
			set wireless.default_wlblob$i.network=lan
			set wireless.default_wlblob$i.mode=ap
			set wireless.default_wlblob$i.ssid=OpenWrt
			set wireless.default_wlblob$i.encryption=none
		EOT
	done
	uci -q commit wireless
}
