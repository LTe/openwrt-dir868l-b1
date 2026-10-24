# wl_apsta.o from FreshTomato (tomato64/freshtomato-arm, release/src-rt-6.x.4708/wl/linux), wl 6.37.14.126
# glue = wl_linux.c code: one cluster in .text, the fast-path cluster, and the module init/exit/probe sections.
{
 "GLUE": {
    ".text": [(0x129ac0, 0x12cf10)],
    ".text.fastpath": [(0x165c, 0x2158)],
    ".init.text": [(0, 0x244)], ".exit.text": [(0, 0xc)],
    ".devinit.text": [(0, 0xf4)], ".devexit.text": [(0, 0xdc)],
 },
 "RENAME": {
    "outer_cache": "wlblob_outer_cache",
    "__memzero": "wlblob_memzero",
    "skb_push": "wlblob_skb_push",
    "skb_pull": "wlblob_skb_pull",
    "warn_slowpath_null": "wlblob_warn_slowpath_null",
    "__copy_from_user": "wlblob_copy_from_user",
    "__copy_to_user": "wlblob_copy_to_user",
    "printk": "wlblob_printk",
    "_set_bit_le": "wlblob_set_bit_le",
    "_test_and_set_bit_le": "wlblob_test_and_set_bit_le",
    "_test_and_clear_bit_le": "wlblob_test_and_clear_bit_le",
 },
 "DROP_SECTIONS": ["__versions", ".modinfo", "__param", ".gnu.linkonce.this_module"],
}
