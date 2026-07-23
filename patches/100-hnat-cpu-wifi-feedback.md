# CPU-to-Wi-Fi HNAT feedback path

## Why this patch exists

The original MT7986 CPU-to-Wi-Fi fast path steals locally generated packets
from the bridge `LOCAL_OUT` hook and immediately reinjects them through
PPE/QDMA. That happens before the Wi-Fi netdev qdisc, GSO feature validation
and MediaTek driver's queue flow control. The direct WDMA/WED producer
therefore has no `NETDEV_TX_BUSY` feedback when the downstream Wi-Fi path
temporarily falls behind.

`100-hnat-cpu-wifi-feedback.patch` keeps the PPE -> WDMA -> WED hardware path,
but changes its admission point:

1. HNAT marks eligible local packets at bridge `LOCAL_OUT` and returns
   `NF_ACCEPT`.
2. Linux runs Wi-Fi qdisc and validates the packet against the Wi-Fi netdev's
   GSO limits.
3. `rt28xx_send_packets()` asks WARP to admit the packet.
4. WARP accounts for both WED TX buffer credits and WPDMA TX1 ring occupancy.
5. If the bounded window is full, WARP stops the netdev queue and the ndo
   returns `NETDEV_TX_BUSY` without consuming the skb.
6. A high-resolution timer polls WED and wakes the same queue at the low
   watermark.
7. An admitted skb is handed to HNAT and reinjected into PPE. The entire flow
   remains on the HNAT path; GSO packets are not selectively diverted.

The proprietary WO/Wi-Fi firmware blobs are not modified. Their source is not
present in this tree. The host driver now provides the missing admission and
backpressure boundary before submitting work to that firmware/hardware path.

## Defaults and runtime tuning

The `mtk_warp` module exposes:

- `cpu_wifi_credit_window=128`
- `cpu_wifi_ring_window=128`
- `cpu_wifi_gso_max_size=16384`
- `cpu_wifi_gso_max_segs=16`
- `cpu_wifi_credit_poll_us=250`

They are available below `/sys/module/mtk_warp/parameters/`. GSO limits are
applied when a Wi-Fi netdev is attached, so changing those two parameters
requires recreating the Wi-Fi interfaces or rebooting. Credit/ring windows and
the poll interval are read at runtime.

`cat /proc/warp_ctrl/warp0/cfg` now also reports:

- current and baseline WED free credits;
- current WPDMA TX1 queued descriptors;
- software reservations not yet reflected by WED MIB;
- admitted segments, busy events, fallbacks and wake events.

For the target case, `fallback_events` should stay at zero after Wi-Fi and HNAT
are fully initialized. `busy_events` may increase under load; that is expected
and proves that backpressure is reaching the Wi-Fi qdisc instead of silently
overrunning the direct path.

## Validation

Use the same single-stream reverse test:

```sh
# router
iperf3 -s

# Wi-Fi client
iperf3 -c 192.168.3.1 -R -t 600 -P 1 -i 1
```

During the run, collect:

```sh
cat /proc/warp_ctrl/warp0/cfg
cat /proc/warp_ctrl/warp0/tx
cat /sys/module/mtk_warp/parameters/cpu_wifi_credit_window
cat /sys/module/mtk_warp/parameters/cpu_wifi_ring_window
cat /sys/module/mtk_warp/parameters/cpu_wifi_credit_poll_us
```

The nftables `mark 0x99` rule remains a valid A/B bypass and is not required by
the new path.
