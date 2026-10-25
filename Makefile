# OpenWrt for the D-Link DIR-868L B1 with working Broadcom WiFi.
#
#   make            # = make image: everything below, firmware ends up in out/
#   make prepare    # submodule, patches, feed, .config (idempotent; rerun after editing feed/ or config.seed)
#   make menuconfig # tweak the config, then `make image`; `make saveconfig` writes it back to config.seed
#   make clean      # drop build output, keep downloads and toolchain
#
# Variables: NPROC (default: all cores), V=s (verbose OpenWrt build), TOOLCHAIN_DIR (use an
# external OpenWrt toolchain, see scripts/get-toolchain.sh; CI does this to skip building gcc).
# TOOLCHAIN_DIR is remembered in .toolchain-dir, so a later plain `make image` keeps using it;
# `make prepare TOOLCHAIN_DIR=` (empty) goes back to building the toolchain.
OPENWRT := $(CURDIR)/openwrt
TOOLCHAIN_DIR ?= $(shell cat .toolchain-dir 2>/dev/null)
FEED    := wlblob
TARGET  := bcm53xx/generic
NPROC   ?= $(shell nproc)
PATCHES := $(sort $(wildcard patches/*.patch))

.PHONY: all image prepare submodule patches feeds config menuconfig saveconfig clean distclean

all: image

submodule:
	@[ -f $(OPENWRT)/Makefile ] || git submodule update --init --depth 1 openwrt || git submodule update --init openwrt

# a patch that reverse-applies cleanly is already in; anything else must apply or we stop
patches: submodule
	@for p in $(PATCHES); do \
		if git -C $(OPENWRT) apply --check -R $(CURDIR)/$$p 2>/dev/null; then continue; fi; \
		echo "applying $$p"; git -C $(OPENWRT) apply $(CURDIR)/$$p || exit 1; \
	done

feeds: patches
	@{ cat $(OPENWRT)/feeds.conf.default; echo "src-link $(FEED) $(CURDIR)/feed"; } > $(OPENWRT)/feeds.conf
	cd $(OPENWRT) && ./scripts/feeds update -a && ./scripts/feeds install -a

config: feeds
	cp config.seed $(OPENWRT)/.config
ifneq ($(TOOLCHAIN_DIR),)
	echo "$(TOOLCHAIN_DIR)" > .toolchain-dir
	echo CONFIG_DEVEL=y >> $(OPENWRT)/.config
	cd $(OPENWRT) && ./scripts/ext-toolchain.sh --toolchain $(abspath $(TOOLCHAIN_DIR))/toolchain-* --overwrite-config --config $(TARGET)
else
	rm -f .toolchain-dir
endif
	$(MAKE) -C $(OPENWRT) defconfig

prepare: config

image: prepare
	$(MAKE) -C $(OPENWRT) -j$(NPROC) $(if $(V),V=$(V))
	mkdir -p out && cp $(OPENWRT)/bin/targets/$(TARGET)/*dir-868l-b1* $(OPENWRT)/bin/targets/$(TARGET)/sha256sums out/
	@ls -l out/

menuconfig: prepare
	$(MAKE) -C $(OPENWRT) menuconfig

saveconfig:
	cd $(OPENWRT) && ./scripts/diffconfig.sh > $(CURDIR)/config.seed

clean:
	rm -rf out
	-$(MAKE) -C $(OPENWRT) clean

distclean:
	rm -rf out toolchain .toolchain-dir
	-$(MAKE) -C $(OPENWRT) distclean
