#
# Broadcom "wl" 802.11ac driver for the BCM4360/BCM4331 radios of the D-Link DIR-868L B1: the
# proprietary SDK 6.37 core is taken from a vendor binary and re-linked against new Linux glue,
# OSL and a rebuilt hnd shared library.
#
# Core variants (WLBLOB_VARIANT):
#   ft    (default) wl_apsta.o 6.37.14.126, AP+STA, downloaded at build time from the public
#         FreshTomato ARM source tree (pinned commit, sha256-checked); nothing is redistributed here.
#   dlink wl_ap.ko 6.37.14.56 (AP only) from the DIR-868L B1 2.03B01 stock firmware; give the path
#         to the extracted module in WLBLOB_DLINK_KO.
# tools/blobsurgery.py cuts the 2.6.36 Linux glue out of the vendor object and retargets the
# remaining imports to the new glue; the result is linked into wl.ko.
#
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=brcm-wl-blob
PKG_RELEASE:=2
PKG_LICENSE:=Proprietary
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)

WLBLOB_VARIANT?=ft
WLBLOB_DLINK_KO?=

FT_COMMIT:=2b34e8520426d86377b51a9df465f10737ed49f9
ifeq ($(WLBLOB_VARIANT),ft)
PKG_SOURCE:=freshtomato-wl_apsta-6.37.14.126-$(FT_COMMIT).o
PKG_SOURCE_URL:=https://raw.githubusercontent.com/tomato64/freshtomato-arm/$(FT_COMMIT)/release/src-rt-6.x.4708/wl/linux/
PKG_SOURCE_URL_FILE:=wl_apsta.o
PKG_HASH:=247bafff4856a1e571d91364b88477ad93099356e549218419d87a95f01ef98e
endif

include $(INCLUDE_DIR)/package.mk

define KernelPackage/brcm-wl-blob
  SUBMENU:=Wireless Drivers
  TITLE:=Broadcom BCM4360/BCM4331 wl driver (prebuilt SDK 6.37 core)
  DEPENDS:=@TARGET_bcm53xx
  FILES:=$(PKG_BUILD_DIR)/wl.ko
endef

define KernelPackage/brcm-wl-blob/description
  Proprietary Broadcom 802.11ac driver core (wl 6.37.14.x) re-linked with Linux glue, OSL and hnd
  shared library rebuilt for the current kernel.  Configured through netifd (wlblob-utils) with
  nas/eapd from brcm-wl-tools; the core is fetched from the FreshTomato source tree at build time.
endef

# $(1) vendor object, $(2) output name, $(3) surgery profile (empty = built-in D-Link 6.37.14.56)
define Build/Prepare/blob
	python3 -I ./tools/blobsurgery.py $(1) $(PKG_BUILD_DIR)/blob.stage1.o $(3) > $(PKG_BUILD_DIR)/surgery.log
	$(TARGET_CROSS)objcopy -R __versions -R .modinfo -R __param -R .rel__param \
		-R .gnu.linkonce.this_module -R .rel.gnu.linkonce.this_module -R .note.gnu.build-id -R .comment \
		$(PKG_BUILD_DIR)/blob.stage1.o $(PKG_BUILD_DIR)/$(2)
	$(TARGET_CROSS)ld -r -o /dev/null $(PKG_BUILD_DIR)/$(2)
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/. $(PKG_BUILD_DIR)/
ifeq ($(WLBLOB_VARIANT),ft)
	$(call Build/Prepare/blob,$(DL_DIR)/$(PKG_SOURCE),wlc_blob_ft.o,./tools/profile-freshtomato-6.37.14.126.py)
else
	@[ -f "$(WLBLOB_DLINK_KO)" ] || { echo "WLBLOB_VARIANT=dlink needs WLBLOB_DLINK_KO=<path to wl_ap.ko from the 2.03B01 firmware>" >&2; exit 1; }
	$(call Build/Prepare/blob,$(WLBLOB_DLINK_KO),wlc_blob.o,)
endif
endef

define Build/Compile
	+$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" WLBLOB_VARIANT="$(WLBLOB_VARIANT)" modules
endef

$(eval $(call KernelPackage,brcm-wl-blob))
