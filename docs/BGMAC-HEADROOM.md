# Ethernet receive headroom experiment

Status: kernel patch for builds after v0.3.1, locally compiled. It is absent
from the v0.3.1 tag. Measurements below are from 2026-10-25; raw records are in
[bgmac-headroom-measurements.json](bgmac-headroom-measurements.json).

## Change and scope

Ethernet RX buffers normally reach the wireless transmit path with less than
the required 256 bytes of headroom. The wl driver then allocates a larger data
buffer and copies the packet. Reserving another 192 bytes in BGMAC's RX buffers
removes that copy for most bridged traffic. Shared or nonlinear packets still
use wl's existing fallback.

The DMA start address and host allocation grow together. The mapped length,
hardware frame offset and accepted maximum frame size remain unchanged. The
length guard now checks the hardware frame limit and rejects lengths below the
FCS size, rather than accepting lengths up to the larger host allocation.
Per-buffer allocation grows from 1920 to 2112 bytes on the tested ARM build.

The runtime comparison used an isolated BGMAC replacement with a read-only
headroom parameter. Its module SHA256 is
`471e299d1ec9c14eed5f0032235390d3262d0781b44072d8e4ddaaa3d81de573`.
The normal firmware patch uses a constant 192-byte addition and does not include
the temporary driver-switching or fixed-PHY reuse machinery.

## CPU and copying

DIR-868L B1 bridges Ethernet and a GL-MT6000 station about two metres away.
WPA2-CCMP, TX A-MSDU off, A-MPDU on, v0.3.1 wl optimizations enabled. Each trial
uses one TCP stream for 15 seconds at a fixed offered rate. Four trials per
setting/direction on 5 GHz combine off/on/on/off and on/off/off/on series;
2.4 GHz has two trials per setting/direction in off/on/on/off order.

| Band and direction | Offered rate | CPU, ordinary buffers | CPU, extra headroom |
|---|---:|---:|---:|
| 5 GHz Wi-Fi → Ethernet | 80 Mbit/s | 39.45% | 37.29% |
| 5 GHz Ethernet → Wi-Fi | 80 Mbit/s | 59.01% | 51.67% |
| 2.4 GHz Wi-Fi → Ethernet | 30 Mbit/s | 19.03% | 18.24% |
| 2.4 GHz Ethernet → Wi-Fi | 30 Mbit/s | 21.88% | 18.25% |

The 5 GHz Ethernet-to-Wi-Fi copy fraction fell from 99.997% to 0.092%.
Throughput remained approximately 79.97 Mbit/s. CPU varied: ordinary-buffer
trials ranged from 53.29% to 61.33%, and candidate trials from 48.19% to 54.06%.
The first series settled for two seconds after carrier returned; the second
used ten seconds. All sixteen 5 GHz TCP trials had zero retransmissions.

The 2.4 GHz TCP retransmission totals were 4/1 for ordinary/candidate buffers
in Wi-Fi-to-Ethernet traffic and 65/71 in Ethernet-to-Wi-Fi traffic. Received
rates were approximately 29.95 and 29.99 Mbit/s, respectively. These are CPU
measurements below the wired path's 100 Mbit/s bottleneck, not peak bridge or
NAT throughput. Local Wi-Fi endpoints do not traverse Ethernet RX and are not
accelerated by this headroom change.

## Packet validation

With extra headroom on 5 GHz, IPv4 and IPv6 each passed both directions of:

- TCP: 512 records of 65,507 bytes.
- UDP: 50,000 datagrams of 1400 bytes at 50 Mbit/s.
- Fragmented UDP: 128 datagrams of 65,507 bytes, individually paced at 15 Mbit/s.

IPv4 was repeated with TX A-MSDU enabled and also passed. All expected records
arrived with correct payloads, no duplicates and no out-of-order records.
An IPv4 peer capture with GRO disabled checked 10,000 TCP packets, including
1463 data packets carrying 2,118,424 bytes. All IP/TCP checksums were correct;
there were no truncated packets or capture drops. GRO was restored afterward.

On 2.4 GHz, both TCP directions passed, but the initial UDP upload received
19,999 of 20,000 datagrams at 20 Mbit/s. A subsequent off/on/on/off control
received all 160,000 ordinary UDP datagrams across both directions/settings.
One candidate upload received 127 of 128 fragmented datagrams; the other seven
fragmented runs received all 128. The host observed 5759 of the expected 5760 IP
fragments in the failing run, without checksum or receive-buffer errors. All
received payloads were correct. The ordinary-buffer controls in this series
had no loss, so this series alone does not rule out a regression. Earlier
v0.3.1 tests also observed occasional 2.4 GHz UDP loss without this change.
A longer follow-up captured test traffic at wl0, before bridge forwarding,
and at the receiving Ethernet host. Both used 96-byte snapshots and reported
zero capture drops. With ordinary buffers, 99,998 of 100,000 small UDP datagrams
arrived; with extra headroom, 99,993 arrived. For each setting the frame counts
and captured bytes matched exactly at both points. Both settings also delivered
all 1024 large UDP datagrams, with all 46,080 fragments matching at both capture
points. Thus these observed losses occurred before the bridge, with no additional
loss across Ethernet forwarding. This does not distinguish the radio, firmware
and wireless receive-driver stages or prove equal loss probabilities. The
headroom change is not a fix for those occasional wireless receive losses.

The capture test is separate from the CPU trials: packet taps were not running
during performance measurements. It checks where packets disappear, not full
transport checksums of the truncated capture. Application payload validation
still covered the complete received datagrams.

The tests introduced no logged kernel warning, oops, DMA poison or invalid
frame-length report. Radio error/overflow counters can increase with either
buffer setting; these results do not claim an error-free radio link. The
original Ethernet driver and 5 GHz client configuration were restored.

## Build and rejected alternative

The generated patch adds an OpenWrt BCM53xx kernel patch. Incremental compilation
of the actual modified BGMAC objects, vmlinux, zImage and device trees completed.
The existing prepared kernel tree needed the generated patch applied explicitly;
an initial make invocation had reused the old objects. That initial invocation
is not evidence that the new code compiled. The patched kernel has not been
flashed or booted; runtime evidence comes from the isolated replacement.

A separate test combined `napi_build_skb` and `napi_consume_skb` at fixed extra
headroom. CPU changed from 35.73% to 34.64% for Wi-Fi-to-Ethernet and from 47.08%
to 48.47% for Ethernet-to-Wi-Fi at 80 Mbit/s. All eight TCP trials had zero
retransmissions, but the result does not justify enabling that combination.
It is not included in the firmware patch.
