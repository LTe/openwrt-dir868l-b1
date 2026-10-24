# libnvram for the emulated /dev/nvram of kmod-brcm-wl-blob (musl, replaces the uClibc one) + wlnvram CLI
all: libnvram.so wlnvram

libnvram.so: nvram_dev.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared -Wl,-soname,libnvram.so $(LDFLAGS) -o $@ $<

wlnvram: wlnvram.c libnvram.so
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -L. -o $@ $< -lnvram

clean:
	rm -f libnvram.so wlnvram
