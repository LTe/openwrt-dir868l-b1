# Performance and driver research

Development measurements and experimental findings, kept separately from the user guide.

## Integration status

Current package validation is recorded in the [v0.3.1 report](releases/v0.3.1-validation.md).

The v0.3.1 package integrates the later checksum, selective TCP GRO and TX buffer
experiments described below. On the default FreshTomato core, `cso_offload`,
`cso_stack`, `rx_batch`, `tx_batch`, `rx_tcp_gro`, `rx_tcp_gro_local_data` and
`tx_expand` default to enabled; `tx_expand_min_len` defaults to 256 bytes.
The all-protocol `rx_gro` and `amsdu_pool` options remain disabled. Hardware
checksums apply to supported BCM4360 IPv4 TCP packets; other traffic uses software.
Neither TSO nor the BGMAC headroom experiment is included.

Release integration also exposed a short-stream boundary missed by the earlier
bulk tests: with A-MSDU and the packet optimizations enabled, an 8-byte-record TCP
transfer could stall after delivering only part of its data. Captured packets
had valid checksums. The legacy Linux 2.6.36 transmit path orphaned socket-owned
skbs before invoking the driver; the modern port now explicitly restores that
ownership release before the core's aggregation queues. Shared skb structures
are copied first, and private buffers are orphaned before headroom expansion.
Consequently the earlier experiment's claim that expansion preserves socket
ownership describes the prototype, not the integrated implementation.

The sections below are a chronological research record: statements that a feature
was isolated or disabled by default describe its state at the time of that test.
They do not override the v0.3.1 defaults listed above. The old 284/216 and 308/229
Mbit/s measurements are baseline results, not measurements of the new package.

## Earlier baseline measurements

Measured on 2026-10-25 with the OpenWrt r37659 (kernel 6.18.55) image, both radios as APs, a
GL-MT6000 as the client about two metres away, `iperf3` server running on the DIR-868L itself
(TCP, 15 s, one band active at a time).  "client → router" is the direction a client uploads.

| band | mode | streams | client → router | router → client | router CPU idle |
|---|---|---|---|---|---|
| 5 GHz (BCM4360) | VHT80, channel 36 | 1 | 284 Mbit/s | 216 Mbit/s | 8 % / 3 % |
| 5 GHz (BCM4360) | VHT80, channel 36 | 4 | 308 Mbit/s | 229 Mbit/s | 1 % / 0 % |
| 2.4 GHz (BCM4331) | HT20, channel 1 | 1 | 54 Mbit/s | 103 Mbit/s | 61 % / 54 % |
| 2.4 GHz (BCM4331) | HT20, channel 1 | 4 | 51 Mbit/s | 112 Mbit/s | 58 % / 48 % |

On 5 GHz the single Cortex-A9 core of the BCM47081 is the limit (softirq 60 to 80 %), not the
radio: the client negotiates 3 spatial streams (VHT-MCS 9, 1300 Mbit/s PHY rate).  The 2.4 GHz
upload is bounded by the client's 2-stream HT20 rate (MCS 12, 78 Mbit/s).  The stock firmware's
CTF forwarding and PKTC packet-chaining paths are not used.  These results are close to the
earlier kernel 6.12 measurements (273/216 Mbit/s); they do not establish a kernel-version
performance difference.

### GRO and driver profiling

The glue can now feed received packets through per-interface GRO cells.  This adds NAPI/GRO
delivery to the Linux stack; the hardware-facing Broadcom DPC still runs as a tasklet.
GRO is experimental and disabled by default; toggle it between tests without reloading the module:

```sh
echo 0 > /sys/module/wl/parameters/rx_gro  # original netif_rx path
echo 1 > /sys/module/wl/parameters/rx_gro  # GRO cells
```

On the same 5 GHz link, two 15-second single-stream measurements per setting, with GRO-off
runs before and after GRO-on, gave these receiver-side means on 2026-10-25:

| setting | client → router | router → client |
|---|---|---|
| GRO off | 305 Mbit/s | 223 Mbit/s |
| GRO on | 320 Mbit/s | 219 Mbit/s |

The receive gain was about 5% in this comparison and 9% in an earlier paired comparison.
There was no measured transmit improvement.  These are short, variable RF measurements,
not a long-duration stability test.  Traffic goes directly over Wi-Fi between the MT6000
client and the DIR-868L server, bypassing the 100 Mbit/s switch in the wired management
path.  Endpoint results include iperf's CPU cost and do not measure bridge/NAT throughput.

UDP needs further evaluation before enabling GRO by default: at 100 Mbit/s with 1400-byte
datagrams and default socket buffers, a paired run lost 0.55% with GRO off and 3.02% with
GRO on.  Across those runs and an initial GRO-on run, all 6,901 missing datagrams matched
the UDP `RcvbufErrors` counter; the radio reported the same received-frame counts in
the paired runs.  This points to receive-socket pressure during batched delivery rather
than loss on the wireless link.  The TCP gain alone is not sufficient to enable this for
all workloads.  Repeating with `iperf3 -w 200K` reduced losses to 0% off versus 0.69% on,
so increasing the socket buffer did not fully resolve the difference.

The initial code review and `perf` sampling identified the following candidates;
the subsequent implementation experiments are recorded below:

- **DMA/cache ownership:** RX cache invalidation accounts for about 10% of sampled cycles;
  TX cache cleaning for about 6.5%.  In `shared/hnddma.c`, the Cortex-A9 DMA64 receive path
  skips unmapping, while the blob's `wlc_bmac_recv` calls `osl_dma_map` again on received
  data.  The old SDK uses mapping calls for cache maintenance.  Any modernization needs
  coordinated buffer ownership, sync and unmap changes across OSL, DMA and blob callers;
  deleting an apparently redundant map would remove required invalidation.
- **Transmit checksum/segmentation:** the active blob's `wlc_toe_add_hdr` emits a four-byte
  `TOE_F0_PASSTHROUGH` header; `toe` and `toe_ol` report unsupported, but that does not
  establish absence of hardware support (see the later checksum prototype below). Checksum/copy code is
  the largest individual TX sample (about 13%).  Enabling Linux HW_CSUM/TSO flags requires
  a working descriptor/metadata implementation first.  A-MPDU and A-MSDU are already on.
- **Packet allocations and copies:** `linux_osl.c` allocates a native skb plus a 224-byte
  compatibility shadow for RX.  `wl_start` copies cloned, nonlinear or short-headroom TX
  skbs.  Buffer recycling and fewer copies are candidates, but their individual benefit
  is unmeasured; the blob's writable-buffer and old skb-layout requirements must survive.
- **Scheduling:** moving the hardware DPC to NAPI could remove an extra queueing stage,
  but requires a correct budget and interrupt rearm protocol around `wlc_dpc`.  Current
  IRQ rates are roughly 1,500–2,100/s, already below packet rates; one IRQ per packet is
  not the observed bottleneck.
- **MMIO barriers:** `W_REG` preserves the SDK's PL310 synchronization after writes.
  Linux's pre-write barriers are not an equivalent replacement.  Removing these barriers
  without checking the hardware workaround is not a justified optimization.

The GRO build passes both probe-time glue/OSL selftests and `wlblob-check` (31 passes,
zero failures, three existing country-list/scan warnings).  IPv6 TCP received 312 Mbit/s;
simultaneous IPv4 TCP received 181/106 Mbit/s.  The temporary MT6000 test interface needs
ICMPv6 neighbor discovery allowed through its firewall for IPv6 testing.

### Blob/DMA audit (2026-10-25)

The FreshTomato ELF contains 12 call-site relocations to `osl_dma_map`, one of which belongs
to the removed `wl_sendup` glue, and none to `osl_dma_unmap`.  The remaining 11 sites are
in eight core functions.  `wlc_bmac_recv` is the normal RX completion path; the others
cover deferred CTF invalidation and software-security paths.  The ARM instructions at
`.text.fastpath+0x181d0` confirm the normal call uses direction 2 (`DMA_RX`) and discards
the returned DMA address: this is cache maintenance, not a new device submission.

Temporary call-site counters on the running driver confirmed the following on BCM4360
during a five-second TCP upload, with GRO off:

| operation | calls | bytes per call |
|---|---:|---|
| `_dma_rxfill`: clean the cleared length marker | 90,411 | 2 |
| `_dma_rxfill`: prepare the receive buffer | 90,411 | 1,844 |
| `_dma_rxfill`: descriptor lookup, already coherent | 90,411 | 32 |
| `wlc_bmac_recv`: invalidate the received data | 90,333 | 76–1,790 |
| `_dma_rx`: retry reading a zero length | 1 | 2 |

The refill/completion difference includes dropped frames and ring occupancy.  Neither RX
nor TX issued an unmap during the measured traffic.  A subsequent five-second download
issued 141,435 TX buffer mappings and the same number of coherent descriptor lookups.
Its RX side, mostly acknowledgements, prepared 50.7 MB of buffer ranges for just 3.67 MB
of received ranges (27,513 frames, average 133 bytes).  This is cache-maintenance work,
not PCIe traffic volume.  Counter instrumentation changes timing and was removed before
throughput comparisons.

`dma64_dd_upd` now skips the unused per-descriptor `DMA_MAP` in this port: descriptor
rings come from `dma_alloc_coherent`, and OSL previously only translated the address,
which the caller ignored.  Packet-buffer synchronization and the `W_REG` publication
barriers remain necessary.  The original SDK and secure-DMA paths retain their handling.
Disassembly confirms `_dma_rxfill` now has two map calls instead of three, and
`dma64_txfast` one instead of two.  Short before/after/before TCP trials did not establish
a throughput gain: the two control runs received 303.55/287.30 Mbit/s on upload and
222.37/223.47 on download; the two changed-driver runs received 267.00/300.81 and
227.32/222.43 respectively.  The 267 Mbit/s run had 33 TCP retransmissions and lower CPU
utilization.  The change removes verified redundant work, but an improvement smaller
than RF/run-to-run variation remains unproven.
The changed build passed both probe selftests and all 31 `wlblob-check` checks, with the
same three known warnings.  Ten-second UDP tests at 100 Mbit/s in each direction,
1400-byte datagrams and `-w 200K`, had zero reported packet loss with GRO disabled.

The larger mapping conversion has these constraints:

1. **Separate submission from CPU synchronization explicitly.** Both the old Broadcom
   kernel and the current ARM kernel's map path invalidate L1 before L2, while the CPU
   acquisition path invalidates L2 before L1.  Reusing a map operation after reception is
   inherited SDK behavior, not a newly introduced ordering change.  No corruption was
   demonstrated by these tests, but the port does not express DMA ownership correctly.
2. **Track mappings outside the blob ABI.** Preserve the 224-byte FreshTomato shadow skb
   layout.  Private sidecar metadata can hold the original DMA address, mapped length,
   direction and ownership.  Pair submissions with unmaps on completion, discard and
   teardown, and acquire the length/header before inspecting it.  A DMA mapping failure
   must stop descriptor submission; the current OSL returns zero and callers proceed.
3. **Do not infer the operation from `dmah != NULL`.** Scatter/gather is disabled, so the
   `rxp_dmah`/`txp_dmah` arrays are not allocated, yet shared code passes indexed addresses
   derived from them.  OSL currently ignores that argument.  Split shared-code APIs or
   retarget verified blob call sites instead of treating it as reliable metadata.
4. **Account for the exceptional callers.** Most secondary blob invalidations are behind
   the CTF flag, currently unset, but `wlc_wsec_miccheck` also invalidates a padding range
   with `p == NULL` after computing a software TKIP MIC.  Replacing every blob map with
   a no-op or assuming every call has a packet would miss this case.
5. **Optimize recycling after ownership is correct.** The two bulk RX operations straddle
   hardware ownership and are not interchangeable duplicates.  Persistent buffer pools
   could reduce allocation and cache work, especially for small ACKs, but must handle
   CPU writes, speculative cache fills, alignment and each radio's DMA device separately.
   The near-absent zero-length retries do not justify optimizing the polling loop first.

### Controlled tests of the throughput limit (2026-10-25)

The measured local-endpoint limit is CPU processing, with a reproducible cost from
software A-MSDU assembly and additional TCP/socket work.  All tests below used the direct
5 GHz MT6000 link, GRO disabled, and no traffic through the wired 100 Mbit/s switch.

Temporary TX counters ruled out the full-packet copy in `wl_start` as the TCP bottleneck:
189,384 packets (286.7 MB including headers) reached it with sufficient headroom, linear
data and no shared data buffer.  Only nine incidental packets needed copying on that
interface.  OSL packet duplication and headroom-expansion counts were both zero.  A
matching zero-copy sender trial behaved similarly and gave 218.2 versus 218.8 Mbit/s.

The copies/checksums visible in `perf` occur earlier in Linux.  The port does not advertise
SG or hardware checksum offload; `skb_segment` takes the copy-and-checksum path when those
features are absent.  The zero-copy profile still spent about 14.7% of sampled cycles in
`csum_partial_copy_nocheck`.  ARM assembly unwinding was incomplete, so inclusive call
graphs cannot reliably assign every sample to its parent.

Two independent interventions establish costs beyond that profile:

| intervention | control | changed | scope |
|---|---|---|---|
| Disable TX A-MSDU, keep A-MPDU enabled | 218.66 / 213.56 Mbit/s | 245.09 / 239.85 Mbit/s | TCP download, 15 s after 2 s warmup, alternating on/off/on/off, diagnostic code removed |
| Disable TX A-MSDU, keep A-MPDU enabled | 289.31 / 285.33 Mbit/s | 356.66 Mbit/s | Raw Ethernet TX, 1514-byte frames, 10 s, on/off/on; no received-frame losses |
| Skip copying received TCP data to iperf | 281.44 / 284.89 Mbit/s | 334.13 Mbit/s | Upload, normal/MSG_TRUNC/normal, same diagnostic build with counting disabled |

The raw Ethernet test used a temporary AF_PACKET sender/receiver with experimental
EtherType 0x88b5, unicast only, `sendmmsg` batches of 32, and truncated reads at the receiver.
It bypasses TCP/IP and the bridge, so its absolute rate is not directly comparable to TCP
payload throughput.  The A-MSDU on/off comparison within that test changes only the radio
setting.  D-Link CPU occupancy was 99.5–99.6%; the TCP comparisons used 92–96% of the CPU.

In the blob, `wlc_amsdu_agg` allocates a separate 192-byte header packet for each new
aggregate and links the data buffers behind it.  That header adds a native skb, an ABI
shadow, a DMA descriptor/map and eventual reclamation.  With A-MSDU disabled, the observed
OSL allocation traffic fell to approximately one 2048-byte allocation per received ACK
frame instead of also allocating aggregation headers.  The intervention proves the cost
of the complete A-MSDU path; it does not isolate allocation cost from its other processing.

Disabling A-MSDU improved TCP TX by about 12% and the raw TX test by about 24% on this
nearby, strong-signal link.  It is not a universal RF tuning result: the original setting
was restored after testing.  RX application's copy cost and missing TX offloads also
remain, so disabling aggregation alone cannot remove the whole endpoint limit.  The
header-reuse experiment below addresses part of that allocation cost. A correctly
implemented SG/checksum path still needs the DMA ownership constraints described above.

### Batching and allocation experiments (2026-10-25)

Three experimental paths are implemented in the feed sources, all **disabled by
default**. These settings can be changed between traffic tests:

```sh
echo 1 > /sys/module/wl/parameters/rx_batch
echo 1 > /sys/module/wl/parameters/tx_batch
echo 1 > /sys/module/wl/parameters/amsdu_pool
cat /sys/module/wl/parameters/tx_batch_stats
cat /sys/module/wl/parameters/amsdu_pool_stats
```

`rx_batch` collects packets during the existing DPC and calls
`netif_receive_skb_list` after releasing the driver lock. `rx_gro` takes precedence
when both receive options are enabled. `tx_batch` uses `netdev_xmit_more()` to pass
up to 16 same-flow IPv4 TCP packets to the core together, flushing at the end of the
network-stack burst, on a flow change or when the queue stops. Other traffic uses
the existing path. Closing an interface discards any unfinished batch.

`amsdu_pool` reuses up to 64 internal 192-byte header buffers per radio, including
their compatibility wrappers. Buffers shared with a clone or handed to Linux are
not recycled. Probe selftests cover reuse, metadata reset, clones, and escape to
the native skb API. Private pool metadata follows the unchanged blob skb layout.

| experiment | control | changed | interpretation |
|---|---|---|---|
| RX list delivery, TCP | 290 / 302 Mbit/s | 315 / 320 Mbit/s | approximately 7% gain in this paired comparison |
| A-MSDU header pool, raw Ethernet TX | 286 / 291 Mbit/s | 302 / 301 Mbit/s | approximately 4.5% gain; all transmitted frames received |
| A-MSDU header pool, TCP TX | 213 / 200 Mbit/s | 219 / 219 Mbit/s | variable control; smaller and less certain gain |
| RX PKTC after ABI repair | 307 / 295 Mbit/s | 295 / 307 Mbit/s | neutral; multi-packet RX chains remained absent |
| Full-page `page_pool` | 263 / 287 Mbit/s RX | 227 / 263 Mbit/s RX | rejected: regression |
| Per-CPU full-page pool | 274 / 288 Mbit/s RX | 223 / 268 Mbit/s RX | rejected: regression |
| Fragment page pool with matching skb truesize | 292 / 314 Mbit/s RX | 299 / 315 Mbit/s RX | no convincing gain; TX regressed |
| Extra compatibility-wrapper pool | 296 / 294 Mbit/s RX | 272 / 284 Mbit/s RX | rejected: regression |
| NAPI skb allocation/consumption in a full NAPI prototype | 303 / 304 Mbit/s RX | 304 / 307 Mbit/s RX | less than 1%; TX unchanged |

The full NAPI, page-pool and wrapper-pool prototypes are not included in the feed.
Their tests retained the existing DMA cache operations. Changing allocation alone
does not establish that a future pool with explicit DMA ownership cannot help.

A combined prototype reached 238 Mbit/s TCP TX with the header pool, TX batching
and four A-MSDU subframes, versus 225 / 228 Mbit/s controls. Disabling A-MSDU gave
248 Mbit/s, and adding TX batching gave 253 Mbit/s. The batching gain itself is
modest: grouping approximately 12–15 packets per core call does not eliminate
per-packet DMA and radio processing. One earlier control run stalled and averaged
93 Mbit/s; it is excluded from performance claims.

The candidate built from feed sources, with RX/TX batching and A-MSDU disabled,
subsequently measured 300 / 250 Mbit/s (RX/TX) under `perf`. Without sampling,
CUBIC RX measured 323 Mbit/s; four streams measured 317 Mbit/s RX and 251 Mbit/s TX.
Single-stream TX measured 246 Mbit/s. These are local-endpoint results on the
strong-signal link, not a gigabit result or a forwarding benchmark. Radio variation
and different TCP congestion controls prevent treating all rows as one A/B trial.
Changing A-MPDU limits, RTS, or reducing `netdev_budget` did not establish a gain.

The latest profile still spends approximately 16% of RX samples in `csum_partial`,
11% copying to the application and 9% invalidating DMA caches. On TX, copy/checksum
accounts for 13%, copying from the application 8%, and DMA cache cleaning 9%.

**PKTC ABI repair:** both vendor cores expect a CTF handle at `wl_info + 0x10` and
the primary netdev at `+0x14`. The port previously placed the netdev at `+0x10`,
which made PKTC mistake it for an active CTF instance and dereference a null
`pub->brc_hot`. A reserved null CTF field and compile-time offset checks fix this.
Traffic with `pktc=1` passed after the repair, but it did not improve speed and the
normal configuration keeps RX PKTC disabled.

**Data validation:** the feed candidate transferred 134,158,336 bytes in each TCP
direction and another 134,158,336 bytes while sharing the downlink with iperf.
All records matched and none were duplicated. It also received all 100,000
1400-byte UDP datagrams in each direction at 100 Mbit/s with a forced 4 MiB socket
buffer. Fragmented UDP (500 datagrams of 65,507 bytes in each direction at
15 Mbit/s) also passed. These tests do not remove the small-buffer UDP limitation:
the earlier RX-list comparison lost 0.34% / 0.50% with batching off/on, entirely
accounted for by `UdpRcvbufErrors`.

The standalone verifier checks deterministic payloads, missing/duplicate records,
and TCP record order. It is deliberately separate from throughput measurement:

```sh
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7,softfloat \
  go build -o wlcheck-arm feed/kernel/brcm-wl-blob/tools/packet-integrity.go
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 \
  go build -o wlcheck-arm64 feed/kernel/brcm-wl-blob/tools/packet-integrity.go
# Start on the receiver, then send from the other device:
./wlcheck recv tcp 10.99.0.1:5522 65507 2048 0
./wlcheck send tcp 10.99.0.1:5522 65507 2048 0
# UDP example: recv uses root/CAP_NET_ADMIN for SO_RCVBUFFORCE.
./wlcheck recv udp 10.99.0.1:5522 1400 100000 0
./wlcheck send udp 10.99.0.1:5522 1400 100000 100
```

Use the ARM soft-float build on this router: the hard-float Go binary faults with
an illegal instruction. The verifier itself was checked with intentionally
corrupted and reordered TCP records, which correctly produced failure results.

### Hardware TX checksum prototype and 6.30 source bring-up

A separate experiment, saved under `../perf-results/2026-10-25/wl-cso-probe`,
implemented the 16-byte TOE header for selected IPv4 TCP test flows on BCM4360.
It is **not included in the feed's normal module**. Linux `CHECKSUM_PARTIAL`
packets reached the DMA path and hardware supplied correct IP/TCP checksums.
Replacing only `wlc_toe_add_hdr` was insufficient: header-cache hits bypass that
function. The prototype reserves a full-size passthrough header and refreshes
offload metadata immediately before TX DMA mapping, including cached packets.
The initial cache-unaware version produced invalid checksums and was rejected.

With TX A-MSDU off and RX/TX batching enabled, valid 20-second runs measured
253 / 257 Mbit/s with checksum offload versus 242 Mbit/s without. CPU stayed
around 96%; this is approximately 5% less CPU time per transferred Mbit, not
an idle CPU. An earlier control stalled and averaged 97 Mbit/s, so it cannot
support a speedup claim; longer paired measurements remain necessary.
`csum_partial_copy_nocheck` disappeared from the offload profile, but `memcpy`
remained. Three 134 MB bidirectional/mixed-traffic checks and smaller tests
passed payload validation. SG and segmentation offload are not implemented.

An isolated `../wl-source-port` now builds 107 vendor C units plus five Linux
glue units from Broadcom 6.30.163.23. The source is the impl14 tree in
`Olipro/VMG1312-B10A`, pinned at `9ae84bc248340f0cb3fb349afffd95a9939a0f02`.
It includes the relevant AC PHY code and D11 core-42 microcode. The resulting
host module links without `wlc_blob.o`; embedded radio microcode is still binary.

Both BCM4331 and BCM4360 attach and pass the OSL selftests. Typed source
headers replace the old glue's hardcoded field offsets. `/dev/nvram` must
retain the Northstar userspace 64 KiB mapping ABI: the vendor source's 32 KiB
default initially prevented `nas`/`eapd` configuration access. After fixing it,
WPA2-CCMP and ping worked. FORTIFY remains enabled; variable-size packet
headers and cross-field copies were corrected rather than disabling checks.

This source build is **experimental**. Under load, nested RX A-MSDU/A-MPDU
produced DMA frame-length and A-MSDU subframe-length errors. Correcting the
ARM streaming DMA ownership sequence alone did not resolve them. With
`rx_amsdu_in_ampdu=0`, a short run measured 295 / 218 Mbit/s RX/TX with no TCP
retransmits, and both directions passed 134 MB TCP, 100,000 x 1400-byte UDP
at 100 Mbit/s, and 500 x 65,507-byte fragmented UDP at 15 Mbit/s. Increasing
RX allocations from 2048 to 4096 bytes with nested aggregation enabled did
not restore throughput. A subsequent 6.37 reload also had poor RX, so the
failure is not yet proven exclusive to 6.30; hardware/peer-state controls are
required. Rebooting into the installed 6.37 module restored 290 / 219 Mbit/s
RX/TX with zero retransmits and nested aggregation enabled. A subsequent
attempt to use the PCI endpoint's advertised `bus` reset method caused an
unexpected board restart during the next 6.30 probe. That reset method must
not be repeated for this bring-up. The board recovered into its unchanged
installed 6.37 module. Logs, module hashes and current limitations are in
`../wl-source-port/BUILD-STATUS.txt` and `runtime-*`.

The later isolated `../wl-cso-general` experiment removes the checksum
prototype's address/port restrictions for ordinary IPv4 TCP. Slow preparation
and chained packets finish checksums in software before fragmentation or
encryption; cached hardware headers are refreshed at DMA submission. This is
still outside the package feed. Paired off/on/on/off TX trials measured
240.54 vs 256.79 Mbit/s, with zero TCP retransmits. At a fixed 150 Mbit/s,
mean CPU utilization fell from 62.57% to 59.36%. TCP/UDP payload tests pass
with TX A-MSDU both on and off. Receiver captures with GRO disabled verified
all 10,229 TCP/IP checksums on different addresses and a different port.
Forced 512-byte Wi-Fi fragmentation stalls even with the entire checksum
experiment disabled; that boundary remains unresolved. See its
`BUILD-STATUS.txt`, source, selftests and `runtime-01` for evidence and limits.

Linux software flowtable was also tested after loading Netfilter modules from
the matching kernel build (the installed modules have a `this_module` size
mismatch). At 80 Mbit/s routed traffic, CPU averaged 60.37% -> 35.49% for
Wi-Fi to Ethernet and 81.48% -> 62.23% in reverse, with `[OFFLOAD]` verified
and zero retransmits. These are **routing** gains: ordinary AP bridging
already measured 35.10% / 58.37% CPU at that rate. The wired path still has
a 100 Mbit/s bottleneck. Test routes/rules were removed; no persistent
firewall or module installation was changed. This is Linux software flow
offload, not Broadcom CTF or hardware routing offload.

Further isolated experiments (2026-10-25) are retained alongside the checkout.
`wl-dma-modern` fixes streaming DMA ownership and passes the payload suite,
but its paired tests worsen CPU per Mbit by about 1.4% RX / 3.4% TX, so it is
not selected as a performance optimization. `wl-gro-tcp` restricts GRO to TCP;
at offered 200 Mbit/s RX, mean CPU changes from 67.18% to 63.22%, with received
rate changing from 199.88 to 197.94 Mbit/s (about 5% better CPU per Mbit).
IPv4 payload tests pass. Small UDP socket buffers still lose packets in all
tested GRO modes, and IPv6 validation is incomplete due to failed neighbor
discovery. These limitations are recorded in each experiment's BUILD-STATUS.txt.

`wl-tso-probe` provides a concrete but incomplete BCM4360 TSO result: setting
TCPSEG_EN and MSS on exactly one test packet produces a correctly checksummed
first 600- or 700-byte TCP segment. The remaining bytes arrive only after a
normal full-packet retransmission; setting the descriptor MSDU length does
not resolve it. Thus hardware responds to the segmentation fields, but usable
multi-segment offload is not demonstrated and NETIF_F_TSO is not advertised.
Captures and sequence analysis are saved in `wl-tso-probe/runtime-01`.
The probe was unloaded and the checksum/TCP-GRO candidate restored in RAM.


## Forwarding-specific follow-up (2026-10-25)

An isolated follow-up measured nearly one full packet copy per Ethernet-to-WiFi
packet because incoming headroom is below the driver's256-byte requirement.
Reusing the skb structure with `pskb_expand_head` reduced mean CPU from54.62%
to51.23% at roughly80Mbit/s in an off/on/on/off comparison.

TCP GRO also proved workload-dependent: forwarding WiFi data to Ethernet was
more expensive with GRO. An experimental selector that bypasses GRO for data
addressed to other bridge hosts reduced CPU from42.94% to36.60% at roughly
80Mbit/s. Reverse-direction measurements did not show a gain from this selector.
These percentages are separate comparisons and are not additive.

The final experimental combination passed bidirectional TCP, UDP and fragmented
UDP payload tests;30942 captured TCP packets had valid IP/TCP checksums.
Large UDP test bursts also exposed loss in both baseline and modified modes;
individually paced large datagrams passed. The complete evidence and limits are
in [the forwarding experiment report](forwarding-experiment-2026-10-25.txt).
These follow-up sources were tested in a separate development build and are
not enabled by the normal package in this commit.
