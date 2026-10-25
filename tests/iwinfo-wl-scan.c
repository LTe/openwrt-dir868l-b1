/* Build against patched iwinfo sources; see docs/LUCI-SCAN.md. */
#include <assert.h>
#include <stddef.h>
#define iwinfo_uci_get_radio test_get_radio
#define iwinfo_uci_free test_uci_free
#include "iwinfo_wl.c"
#undef iwinfo_uci_get_radio
#undef iwinfo_uci_free
void test_uci_free(void) {}
struct uci_section *test_get_radio(const char *name, const char *type) { return NULL; }
const struct iwinfo_ops wext_ops = { 0 };

static unsigned char fixture[512];
static char output[IWINFO_BUFSIZE];
static struct wl_scan_results *res = (void *)fixture;
static struct wl_scan_bss *bss;
static int len;

static void setup(const unsigned char *ies, size_t size)
{
	memset(fixture, 0, sizeof(fixture));
	res->version = 109;
	res->count = 1;
	res->buflen = sizeof(*res) + sizeof(*bss) + size;
	bss = (void *)res->records;
	bss->version = 109;
	bss->length = sizeof(*bss) + size;
	bss->ie_offset = sizeof(*bss);
	bss->ie_length = size;
	bss->ssid_len = 4;
	memcpy(bss->ssid, "test", 4);
	bss->ctl_ch = 36;
	bss->rssi = -60;
	bss->capability = 0x11;
	if (size)
		memcpy(res->records + sizeof(*bss), ies, size);
}

static struct iwinfo_scanlist_entry *parse(void)
{
	assert(!wl_scan_parse(res, sizeof(fixture), output, &len));
	assert(len == sizeof(struct iwinfo_scanlist_entry));
	return (void *)output;
}

static void invalid(void)
{
	assert(wl_scan_parse(res, sizeof(fixture), output, &len) == -1);
	assert(len == 0);
}

int main(void)
{
	static const unsigned char rsn[] = {
		48, 18, 1, 0, 0, 15, 172, 4, 1, 0, 0, 15, 172, 4,
		1, 0, 0, 15, 172, 2,
	};
	static const unsigned char wpa[] = {
		221, 22, 0, 80, 242, 1, 1, 0, 0, 80, 242, 2,
		1, 0, 0, 80, 242, 2, 1, 0, 0, 80, 242, 2,
	};
	struct iwinfo_scanlist_entry *e;
	unsigned char mutation[sizeof(fixture)];
	unsigned i, j;

	assert(sizeof(struct wl_scan_params) == 64);
	assert(sizeof(*bss) == 124);
	assert(offsetof(struct wl_scan_bss, rssi) == 78);
	assert(offsetof(struct wl_scan_bss, ie_offset) == 116);
	setup(rsn, sizeof(rsn));
	e = parse();
	assert(!strcmp(e->ssid, "test") && e->channel == 36 && e->mhz == 5180);
	assert((int8_t)e->signal == -60 && e->quality == 50);
	assert(e->crypto.enabled && e->crypto.wpa_version == 2);
	assert(e->crypto.auth_suites == IWINFO_KMGMT_PSK);
	assert(e->crypto.pair_ciphers == IWINFO_CIPHER_CCMP);
	setup(wpa, sizeof(wpa));
	assert(parse()->crypto.wpa_version == 1);
	setup(NULL, 0);
	assert(parse()->crypto.pair_ciphers & IWINFO_CIPHER_WEP40);
	bss->capability = 1;
	assert(!parse()->crypto.enabled);
	bss->ssid_len = 0;
	assert(!parse()->ssid[0]);
	bss->ssid_len = 33; invalid();
	setup(rsn, sizeof(rsn)); bss->length = 0; invalid();
	setup(rsn, sizeof(rsn)); bss->length = UINT32_MAX; invalid();
	setup(rsn, sizeof(rsn)); bss->ie_offset = 0; invalid();
	setup(rsn, sizeof(rsn)); bss->ie_length = UINT32_MAX; invalid();
	setup(rsn, sizeof(rsn)); res->count = 2; invalid();
	setup(rsn, sizeof(rsn)); res->buflen = UINT32_MAX; invalid();
	setup(rsn, sizeof(rsn)); bss->version = 107; invalid();
	setup(rsn, sizeof(rsn)); res->version = 108; bss->version = 108; parse();
	setup((const unsigned char *)"\x30\xff", 2); invalid();
	setup((const unsigned char *)"\x30", 1); invalid();
	setup((const unsigned char *)"\x30\x00", 2); parse();
	setup(NULL, 0); res->count = 0; res->buflen = sizeof(*res);
	assert(!wl_scan_parse(res, sizeof(fixture), output, &len) && len == 0);
	assert(wl_scan_parse(res, 0, output, &len) == -1);

	/* Exercise every byte/bit boundary, including malformed RSN counts. */
	setup(rsn, sizeof(rsn));
	memcpy(mutation, fixture, sizeof(fixture));
	for (i = 0; i < sizeof(fixture); i++)
		for (j = 0; j < 256; j++)
		{
			memcpy(fixture, mutation, sizeof(fixture));
			fixture[i] = j;
			wl_scan_parse(res, sizeof(fixture), output, &len);
		}
	puts("wl scan parser: PASS (ABI, WPA/WPA2, open/WEP, malformed input, 131072 mutations)");
	return 0;
}
