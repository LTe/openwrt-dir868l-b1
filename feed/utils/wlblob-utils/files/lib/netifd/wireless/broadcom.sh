#!/bin/sh
# netifd wireless driver for the re-linked Broadcom wl core (kmod-brcm-wl-blob).
# The driver type is "broadcom" (the name LuCI knows for Broadcom wl radios: it offers the
# WPA-PSK choices only for the mac80211 and broadcom types).
# Radios are wlN netdevs created by the wl module; WPA is done by nas (authenticator for AP,
# supplicant for STA) and eapd, which read their configuration through /dev/nvram (emulated by
# the module).  Modes: one STA per radio, or one or more APs (MBSS).
. /lib/netifd/netifd-wireless.sh
. /lib/functions/system.sh

init_wireless_driver "$@"

drv_broadcom_init_device_config() {
	config_add_string ifname country macaddr
	config_add_int txpower frameburst distance
}

drv_broadcom_cleanup() { :; }
drv_broadcom_init_vlan_config() { :; }
drv_broadcom_init_station_config() { :; }

drv_broadcom_init_iface_config() {
	config_add_string macaddr
	config_add_boolean isolate hidden
	config_add_int maxassoc wpa_group_rekey
}

wlblob_log() { logger -t wlblob "$@"; }

# channel + htmode -> wl chanspec string
wlblob_chanspec() {
	local channel="$1" htmode="$2" band="$3" side
	case "$htmode" in
		HT40*|VHT40|HE40)
			case "$htmode" in
				HT40+) side=l;;
				HT40-) side=u;;
				*)
					if [ "$band" = "2g" ]; then
						[ "$channel" -lt 7 ] && side=l || side=u
					else
						[ $(( ($channel / 4) % 2 )) -eq 1 ] && side=l || side=u
					fi
				;;
			esac
			echo "${channel}${side}"
		;;
		VHT80|HE80)   echo "${channel}/80";;
		VHT160|HE160) echo "${channel}/160";;
		*)            echo "$channel";;
	esac
}

wlblob_wl() { wl -i "$ifn" "$@" >/dev/null 2>&1; }

# wsec / wpa_auth / nvram akm+crypto from the parsed encryption
wlblob_security() {
	local wsec=0 wpa_auth=0 akm= crypto=
	case "$wpa_cipher" in
		*CCMP*TKIP*|*TKIP*CCMP*) wsec=6; crypto=tkip+aes;;
		*CCMP*) wsec=4; crypto=aes;;
		*TKIP*) wsec=2; crypto=tkip;;
	esac
	case "$wpa" in
		1) wpa_auth=0x4; akm=psk;;
		2) wpa_auth=0x80; akm=psk2;;
		3) wpa_auth=0x84; akm="psk psk2";;
	esac
	[ "$auth_type" = "psk" ] || { wpa_auth=0; wsec=0; akm=; crypto=; }
	echo "$wsec $wpa_auth $akm $crypto"
}

# eapd opens its EAPOL/event sockets per nvram "lan" group: AP BSSes are reached through the lan
# bridge (lan_ifname), a station radio is not bridged and gets a group of its own (lanN_ifname=wlN).
# Only wl interfaces are listed: eapd scans every name in these lists, and an ethernet port such as
# "lan1" would both collide with the group names and send its wl<N>.<M>_ifname lookup through
# 65000 nvram reads (about 20 s during which no EAPOL/event socket is open yet).
wlblob_eapd_ifnames() {
	local lanif="$(uci -q get network.lan.device)" w u aps=""
	lanif="${lanif:-br-lan}"
	wlnvram set "lan_ifname=$lanif"; wlnvram set "${lanif}_ifname=$lanif"
	for w in $(ls /sys/class/net | grep -E '^wl[0-9](\.[0-9]+)?$'); do
		case "$w" in
			wl[0-9])
				u="${w#wl}"
				if [ "$(wlnvram get ${w}_mode)" = sta ]; then
					wlnvram set "lan$((u + 1))_ifname=$w"; wlnvram set "lan$((u + 1))_ifnames=$w"
					continue
				fi
				wlnvram set "lan$((u + 1))_ifname="; wlnvram set "lan$((u + 1))_ifnames="
			;;
		esac
		aps="$aps $w"
	done
	wlnvram set "lan_ifnames=${aps# }"
}

# eapd/nas restart and the station joins happen once per `wifi` run, debounced in wlblob-daemons
wlblob_daemons_restart() {
	local tok="$(cat /proc/sys/kernel/random/uuid)"
	echo "$tok" > /var/run/wlblob.daemons
	setsid /usr/sbin/wlblob-daemons "$tok" </dev/null >/dev/null 2>&1 &
}

wlblob_setup_ap() {
	local name="$1" vif bss
	json_select config
	json_get_vars mode ssid key isolate:0 hidden:0 maxassoc wpa_group_rekey:3600 macaddr
	wireless_vif_parse_encryption
	[ "$encryption" = "psk+psk2" ] && wpa=3	# LuCI spells psk-mixed this way for type broadcom
	json_select ..

	# first AP lives on the radio interface itself, the others on wlN.k (MBSS)
	bss=$ap_count
	ap_count=$((ap_count + 1))
	if [ "$bss" = 0 ]; then
		vif="$ifn"
	else
		vif="$ifn.$bss"
		wl -i "$ifn" ssid -C "$bss" "$ssid" >/dev/null 2>&1
		local n=0
		while [ $n -lt 30 ] && [ ! -d "/sys/class/net/$vif" ]; do sleep 0.1; n=$((n + 1)); done
		[ -d "/sys/class/net/$vif" ] || { wireless_setup_vif_failed "no $vif netdev"; return 1; }
		vifs="$vifs $vif"
	fi

	[ -n "$ssid" ] || { wireless_setup_vif_failed NO_SSID; return 1; }
	set -- $(wlblob_security)
	local wsec="$1" wpa_auth="$2" akm="$3" crypto="$4"
	[ "$auth_type" = "psk" -a -z "$key" ] && { wireless_setup_vif_failed NO_KEY; return 1; }
	[ "$wpa" -gt 0 -a "$auth_type" != "psk" ] && { wireless_setup_vif_failed "unsupported encryption $encryption (only none/psk/psk2/psk-mixed/psk+psk2)"; return 1; }

	[ "$bss" = 0 ] && wlblob_wl ap 1
	wl -i "$vif" ssid "$ssid" >/dev/null 2>&1
	wl -i "$vif" closed "$hidden" >/dev/null 2>&1
	wl -i "$vif" ap_isolate "$isolate" >/dev/null 2>&1
	[ -n "$maxassoc" ] && wl -i "$vif" maxassoc "$maxassoc" >/dev/null 2>&1
	wl -i "$vif" wsec "$wsec" >/dev/null 2>&1
	wl -i "$vif" wpa_auth "$wpa_auth" >/dev/null 2>&1
	wl -i "$vif" wsec_restrict $([ "$wsec" != 0 ] && echo 1 || echo 0) >/dev/null 2>&1
	wl -i "$vif" eap_restrict $([ "$wsec" != 0 ] && echo 1 || echo 0) >/dev/null 2>&1

	# nvram view for nas/eapd (wlN_* for the primary BSS, wlN.k_* for the others)
	local u="${ifn#wl}" hw=$(cat /sys/class/net/$vif/address 2>/dev/null)
	wlnvram set "${vif}_ifname=$vif"; wlnvram set "${vif}_unit=$u"; wlnvram set "${vif}_mode=ap"
	wlnvram set "${vif}_radio=1"; wlnvram set "${vif}_bss_enabled=1"; wlnvram set "${vif}_ssid=$ssid"
	wlnvram set "${vif}_hwaddr=$hw"; wlnvram set "${vif}_closed=$hidden"
	wlnvram set "${vif}_auth=0"; wlnvram set "${vif}_auth_mode=none"; wlnvram set "${vif}_preauth=0"
	wlnvram set "${vif}_net_reauth=36000"; wlnvram set "${vif}_wps_mode=disabled"
	wlnvram set "${vif}_akm=$akm"; wlnvram set "${vif}_crypto=$crypto"; wlnvram set "${vif}_wpa_psk=$key"
	wlnvram set "${vif}_wpa_gtk_rekey=$wpa_group_rekey"

	wireless_add_vif "$name" "$vif"
	wlblob_log "$vif: AP '$ssid' ${akm:-open}${crypto:+/$crypto}"
}


wlblob_setup_sta() {
	local name="$1"
	json_select config
	json_get_vars mode ssid key bssid
	wireless_vif_parse_encryption
	[ "$encryption" = "psk+psk2" ] && wpa=3	# LuCI spells psk-mixed this way for type broadcom
	json_select ..

	[ -n "$ssid" ] || { wireless_setup_vif_failed NO_SSID; return 1; }
	set -- $(wlblob_security)
	local wsec="$1" wpa_auth="$2" akm="$3" crypto="$4"
	[ "$auth_type" = "psk" -a -z "$key" ] && { wireless_setup_vif_failed NO_KEY; return 1; }

	# station: the core associates on `wl ssid`, nas (supplicant role, wlN_mode=sta) does the 4-way handshake
	wlblob_wl ap 0
	wlblob_wl infra 1
	wlblob_wl mpc 0		# switching to STA re-enables MPC, which powers the radio down while unassociated
	wlblob_wl PM 0		# with power save on the station never completes the association here
	wlblob_wl wsec "$wsec"
	wlblob_wl wpa_auth "$wpa_auth"
	wlblob_wl wsec_restrict $([ "$wsec" != 0 ] && echo 1 || echo 0)
	wlblob_wl eap_restrict $([ "$wsec" != 0 ] && echo 1 || echo 0)
	wlblob_wl sup_wpa 0

	local u="${ifn#wl}" hw=$(cat /sys/class/net/$ifn/address 2>/dev/null)
	wlnvram set "${ifn}_ifname=$ifn"; wlnvram set "${ifn}_unit=$u"; wlnvram set "${ifn}_mode=sta"
	wlnvram set "${ifn}_radio=1"; wlnvram set "${ifn}_bss_enabled=1"; wlnvram set "${ifn}_ssid=$ssid"
	wlnvram set "${ifn}_hwaddr=$hw"; wlnvram set "${ifn}_infra=1"; wlnvram set "${ifn}_vifs="
	wlnvram set "${ifn}_bssid=$bssid"
	wlnvram set "${ifn}_auth=0"; wlnvram set "${ifn}_auth_mode=none"; wlnvram set "${ifn}_wps_mode=disabled"
	wlnvram set "${ifn}_akm=$akm"; wlnvram set "${ifn}_crypto=$crypto"; wlnvram set "${ifn}_wpa_psk=$key"
	has_sta=1

	wireless_add_vif "$name" "$ifn"
	wlblob_log "$ifn: STA '$ssid' ${akm:-open}${crypto:+/$crypto}"
}

drv_broadcom_setup() {
	json_select config
	json_get_vars ifname channel band htmode country txpower frameburst:1 distance macaddr
	_wdev_prepare_channel
	json_select ..

	ifn="$ifname"
	[ -n "$ifn" ] || { wlblob_log "no ifname for $1"; wireless_set_retry 0; return 1; }
	[ -d "/sys/class/net/$ifn" ] || {
		[ -x /etc/init.d/wlblob ] && /etc/init.d/wlblob start
		[ -d "/sys/class/net/$ifn" ] || { wlblob_log "$ifn not present"; wireless_set_retry 0; return 1; }
	}
	# device-level data must not be called "ifname": netifd treats that as a link to bring up
	wireless_set_data wlif="$ifn"

	wlblob_wl down
	[ -n "$macaddr" ] && wlblob_wl cur_etheraddr "$macaddr"
	[ "$auto_channel" -gt 0 ] && channel=$([ "$band" = "2g" ] && echo 6 || echo 36)
	[ -n "$country" ] && wlblob_wl country "$country"
	wlblob_wl chanspec "$(wlblob_chanspec "$channel" "$htmode" "$band")"
	wlblob_wl frameburst "$frameburst"
	[ -n "$txpower" ] && wlblob_wl txpwr1 -d "$txpower"

	# MBSS mode must be chosen while the radio is down
	local n_ap=0
	wlblob_count_ap() { n_ap=$((n_ap + 1)); }
	for_each_interface "ap" wlblob_count_ap
	wlblob_wl mbss $([ "$n_ap" -gt 1 ] && echo 1 || echo 0)

	has_sta=0; ap_count=0; vifs=""
	for_each_interface "sta" wlblob_setup_sta
	[ "$has_sta" = 0 ] && for_each_interface "ap" wlblob_setup_ap
	wlnvram set "${ifn}_vifs=${vifs# }"
	wlblob_wl up
	ip link set "$ifn" up
	local v b
	for v in $vifs; do
		b="${v##*.}"
		wlblob_wl bss -C "$b" up
		ip link set "$v" up
	done
	wlblob_eapd_ifnames
	wlblob_daemons_restart
	wlblob_log "$ifn up: band $band channel $channel ($htmode)"
	wireless_set_up
}

drv_broadcom_teardown() {
	json_select data
	json_get_vars wlif
	json_select ..
	[ -n "$wlif" ] || return 0
	local v
	for v in /sys/class/net/$wlif.[0-9]*; do
		[ -d "$v" ] || continue
		v="${v##*/}"
		wl -i "$wlif" bss -C "${v##*.}" down >/dev/null 2>&1
		ip link set "$v" down 2>/dev/null
		wlnvram set "${v}_bss_enabled=0"; wlnvram set "${v}_akm="
	done
	wl -i "$wlif" down >/dev/null 2>&1
	ip link set "$wlif" down 2>/dev/null
	wlnvram set "${wlif}_bss_enabled=0"
	wlnvram set "${wlif}_akm="
	wlnvram set "${wlif}_mode="
	wlnvram set "${wlif}_vifs="
}

add_driver broadcom
