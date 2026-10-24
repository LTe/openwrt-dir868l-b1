#
# nas / eapd / wlconf for the Broadcom "wl" core, built from source.
#
# The sources are the GPL ones FreshTomato / Tomato64 ship for the SDK 6.37 wl driver, as
# vendored (and made to build standalone) by the northstar-broadcom-revival project.  nas is the
# WPA authenticator (AP) and supplicant (STA): the 4-way handshake ends in the driver's own
# WLC_SET_KEY ioctl, which is why hostapd/wpa_supplicant cannot be used with this core.  eapd
# relays EAPOL frames and driver events to nas.  All three read their configuration from
# wlN_* NVRAM variables through /dev/nvram, which kmod-brcm-wl-blob emulates; libnvram here is a
# small musl client for that device, wlnvram its CLI.
#
# The `wl` control tool exists only as Broadcom's prebuilt uClibc binary.  The one from the same
# FreshTomato tree as the driver core is downloaded at build time (pinned, sha256-checked) and run
# on musl through northstar's uclibc-compat shim (/lib/uclibc.so, DT_NEEDED renamed in place).
#
include $(TOPDIR)/rules.mk

PKG_NAME:=brcm-wl-tools
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/umair-gititall/northstar-broadcom-revival.git
PKG_SOURCE_DATE:=2026-10-20
PKG_SOURCE_VERSION:=861ad43ccb70731b765e029228061947ab2890e8
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=GPL-2.0-only
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_FLAGS:=no-lto

include $(INCLUDE_DIR)/package.mk

FT_COMMIT:=2b34e8520426d86377b51a9df465f10737ed49f9
WL_CLI:=freshtomato-wl-cli-6.37.14.126-$(FT_COMMIT)
define Download/wl-cli
  FILE:=$(WL_CLI)
  URL:=https://raw.githubusercontent.com/tomato64/freshtomato-arm/$(FT_COMMIT)/release/src-rt-6.x.4708/wl/arm-uclibc/
  URL_FILE:=wl
  HASH:=d10a057feda563300b49e185ed627e0419e432febbbe23e2a25929246da0f519
endef
$(eval $(call Download,wl-cli))

define Package/brcm-wl-tools
  SECTION:=net
  CATEGORY:=Network
  SUBMENU:=Wireless
  TITLE:=nas/eapd/wlconf/wl userland for the Broadcom wl core
  DEPENDS:=@TARGET_bcm53xx +kmod-brcm-wl-blob
endef

define Package/brcm-wl-tools/description
  WPA authenticator/supplicant (nas), EAPOL/event relay (eapd) and radio configurator (wlconf)
  for the re-linked Broadcom wl driver (kmod-brcm-wl-blob), built from the FreshTomato GPL
  sources together with libshared and a musl libnvram (+ wlnvram CLI) talking to the emulated
  /dev/nvram, plus Broadcom's prebuilt `wl` control tool running on a uClibc ABI shim.
endef

TOOLS_MAKE_VARS := \
	CC="$(TARGET_CC)" AR="$(TARGET_AR)" \
	CFLAGS="$(TARGET_CFLAGS)" CPPFLAGS="$(TARGET_CPPFLAGS)" LDFLAGS="$(TARGET_LDFLAGS)" \
	LIBNVRAM="$(PKG_BUILD_DIR)/libnvram"

define Build/Prepare
	$(call Build/Prepare/Default)
	mkdir -p $(PKG_BUILD_DIR)/libnvram
	$(CP) ./src/* $(PKG_BUILD_DIR)/libnvram/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR)/libnvram $(TOOLS_MAKE_VARS)
	$(MAKE) -C $(PKG_BUILD_DIR)/vendor/libshared $(TOOLS_MAKE_VARS)
	$(MAKE) -C $(PKG_BUILD_DIR)/userspace/libbcmcrypto $(TOOLS_MAKE_VARS)
	$(MAKE) -C $(PKG_BUILD_DIR)/userspace/nas $(TOOLS_MAKE_VARS)
	$(MAKE) -C $(PKG_BUILD_DIR)/userspace/eapd $(TOOLS_MAKE_VARS)
	$(MAKE) -C $(PKG_BUILD_DIR)/userspace/wlconf $(TOOLS_MAKE_VARS)
	$(MAKE) -C $(PKG_BUILD_DIR)/userspace/uclibc-compat $(TOOLS_MAKE_VARS) VENDOR_WL="$(DL_DIR)/$(WL_CLI)"
endef

define Package/brcm-wl-tools/install
	$(INSTALL_DIR) $(1)/lib $(1)/usr/lib $(1)/usr/sbin
	$(CP) $(PKG_BUILD_DIR)/libnvram/libnvram.so $(1)/usr/lib/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/libnvram/wlnvram $(1)/usr/sbin/
	$(CP) $(PKG_BUILD_DIR)/userspace/uclibc-compat/uclibc.so $(1)/lib/
	$(LN) ld-musl-arm.so.1 $(1)/lib/ld-uClibc.so.0
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/userspace/uclibc-compat/wl $(1)/usr/sbin/wl
	$(CP) $(PKG_BUILD_DIR)/vendor/libshared/libshared.so $(1)/usr/lib/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/userspace/nas/nas $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/userspace/eapd/eapd $(1)/usr/sbin/
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/userspace/wlconf/wlconf $(1)/usr/sbin/
endef

$(eval $(call BuildPackage,brcm-wl-tools))
