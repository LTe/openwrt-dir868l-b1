# kbuild for the re-linked Broadcom wl driver (DIR-868L B1 / BCM4360)
obj-m := wl.o
# WLBLOB_VARIANT=dlink:           wl_ap.ko 6.37.14.56 from the DIR-868L B1 firmware
# WLBLOB_VARIANT=ft (default):    wl_apsta.o 6.37.14.126 from FreshTomato (AP+STA), different 2.6.36 sk_buff layout
WLBLOB_VARIANT ?= ft
ifeq ($(WLBLOB_VARIANT),ft)
WLBLOB_OBJ := wlc_blob_ft.o
else
WLBLOB_OBJ := wlc_blob.o
endif
wl-objs := $(WLBLOB_OBJ) wl_linux.o wl_compat.o linux_osl.o nvram.o wl_selftest.o \
	shared/aiutils.o shared/bcmotp.o shared/bcmsrom.o shared/bcmutils.o shared/hnddma.o \
	shared/hndpmu.o shared/nicpci.o shared/sbutils.o shared/siutils.o

ccflags-y := -I$(src)/include -I$(src) -I$(src)/shared \
	-DBCMDRIVER -Dlinux -DBCM47XX -DBCM5354 -DBCMNVRAMR -DBCMDMA32 -DWLC_LOW -DWLC_HIGH -DPHY_HAL \
	-DWL11AC -DPPR_API -DBCMDBG_TRAP -DBCMDBG_ERR -DWLTEST -DPKTC -DLINUX_HYBRID_POSTPROCESSING_REMOVAL \
	-D__LINUX_ARM_ARCH__=7 -DWLBLOB_PORT \
	-Wno-error -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable -Wno-format \
	-Wno-missing-prototypes -Wno-missing-declarations -Wno-implicit-fallthrough -Wno-array-bounds \
	-Wno-strict-prototypes -Wno-old-style-definition -Wno-cast-function-type -Wno-stringop-overflow \
	-Wno-dangling-pointer -Wno-address -Wno-enum-conversion -Wno-shadow

ifeq ($(WLBLOB_VARIANT),ft)
ccflags-y += -DWLSKB_LAYOUT_FT
endif
