#
# netifd/uci integration for the re-linked Broadcom wl driver (kmod-brcm-wl-blob): wireless
# driver script, radio detection, boot-time init (unbind b43/bcma, radio NVRAM, modprobe) and the
# DIR-868L B1 radio calibration template.  No binaries; wl/nas/eapd come from brcm-wl-tools.
#
include $(TOPDIR)/rules.mk

PKG_NAME:=wlblob-utils
PKG_RELEASE:=6
PKG_LICENSE:=GPL-2.0-only

include $(INCLUDE_DIR)/package.mk

define Package/wlblob-utils
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Wireless
  TITLE:=netifd integration for kmod-brcm-wl-blob (DIR-868L B1)
  DEPENDS:=@TARGET_bcm53xx +kmod-brcm-wl-blob +brcm-wl-tools +wifi-scripts
endef

define Package/wlblob-utils/description
  Makes the wl radios ordinary /etc/config/wireless devices (type broadcom): AP with one or more
  SSIDs, station mode, none/psk/psk2 via nas/eapd, LuCI status through the iwinfo wl backend.
endef

define Build/Compile
endef

define Package/wlblob-utils/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/share/wlblob $(1)/etc/init.d $(1)/lib/firmware/brcm
	$(INSTALL_BIN) ./files/usr/sbin/* $(1)/usr/sbin/
	$(INSTALL_DATA) ./files/usr/share/wlblob/wl-nvram.tmpl $(1)/usr/share/wlblob/
	$(INSTALL_BIN) ./files/etc/init.d/wl-nvram $(1)/etc/init.d/
	$(INSTALL_BIN) ./files/etc/init.d/wlblob $(1)/etc/init.d/
	$(LN) /tmp/wl-nvram.txt $(1)/lib/firmware/brcm/wl-nvram.txt
	$(INSTALL_DIR) $(1)/lib/netifd/wireless $(1)/lib/wifi
	$(INSTALL_BIN) ./files/lib/netifd/wireless/broadcom.sh $(1)/lib/netifd/wireless/
	$(INSTALL_DATA) ./files/lib/wifi/broadcom.sh $(1)/lib/wifi/
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/etc/uci-defaults/60-wlblob-type-broadcom $(1)/etc/uci-defaults/
endef

$(eval $(call BuildPackage,wlblob-utils))
