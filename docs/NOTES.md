# bypass-lab — study notes

Interview prep for this repo: what each piece does, why, and the questions it
invites. Numbers quoted here are only ones the repo actually produced; see the
README for where each came from.

## 1. The one-paragraph pitch

A NASDAQ ITCH feed is replayed as UDP at a fixed rate. One feed handler (decode
+ checksum + echo) runs behind four receive paths: a blocking `recvmsg` socket,
batched busy-polling `recvmmsg`, AF_XDP, and DPDK. The sender stamps each packet
with the time it was *scheduled* to leave, the receiver echoes it, and the
sender measures the round trip on its own clock, so there is no clock-sync
problem and no coordinated omission. The decoder is generated at compile time
from a declared ITCH schema. Tuning (pinning, IRQ affinity, isolcpus, huge
pages) is applied one change at a time and repeated, and a change only counts
when its min–max range across repetitions is disjoint from the baseline's.

## 2. The four paths, mechanically

| | where the packet is first touched by our code | per-packet kernel work | wake-up |
|---|---|---|---|
| recvmsg | socket receive queue, copied out by a syscall | driver → skb alloc → GRO → IP → UDP → socket queue → copy | IRQ → softirq → wake the sleeping thread → context switch |
| recvmmsg + busy poll | same, but up to 64 per syscall | same stack; `SO_BUSY_POLL` lets the spinning thread run the NIC's NAPI poll itself | none: the thread never sleeps |
| AF_XDP | UMEM frame, in place | XDP program runs in the driver before skb allocation and redirects to an XSK ring; no skb, no IP/UDP stack | needs_wakeup / busy poll; zero-copy if the driver supports it, copy mode otherwise |
| DPDK | mbuf from a huge-page mempool | none: NIC is bound to vfio-pci, the PMD polls descriptor rings from user space | none: pure polling |

Things worth being able to say precisely:

- **Why AF_XDP needs an XDP program here at all.** The socket binds to one
  queue. Without a filter, everything on that queue (SSH, ARP) would be
  redirected away from the kernel. `src/bpf/xdp_udp_redirect.bpf.c` redirects
  only UDP to the feed port and passes everything else.
- **The four AF_XDP rings.** FILL (we give the kernel empty frames), RX (kernel
  gives us filled frames), TX (we give frames to send), COMPLETION (kernel
  returns sent frames). The echo reuses the RX frame for TX, so a round trip
  touches exactly one UMEM frame (`src/rx_afxdp.cpp`).
- **Zero-copy vs copy mode.** Zero-copy means the NIC DMAs straight into UMEM.
  It needs driver support. veth (CI) has none, so CI is in copy mode and the
  receiver JSON records `"zerocopy":false`. ENA supports native XDP and AF_XDP
  zero-copy on recent kernels, with an MTU limit (≤ 3498), which is why
  `scripts/aws/net_setup.sh` sets MTU 1500.
- **Why one queue (`ethtool -L combined 1`).** The feed is one flow. AF_XDP and
  DPDK here each own one queue, so RSS must not spread the flow. All four paths
  use the same queue count so none gets an advantage.
- **The echo is built in place** (`net::make_reply_inplace`): swap MACs, IPs and
  ports, rewrite the payload, recompute the IPv4 header checksum, and set the
  UDP checksum to 0 (legal for IPv4). No allocation, no copy of the request.

## 3. Measurement design

- **Latency from scheduled send** (`src/sender.cpp`). Packet *i* is due at
  `t0 + i/rate`. If the sender falls behind it sends late but keeps the original
  due time, so the backlog counts against latency. `rtt_send` (from actual send)
  is also recorded. The gap between the two tells you whether the *sender*
  added queueing.
- **Single clock.** Both timestamps of every sample are read on the sender
  (`CLOCK_MONOTONIC` via vDSO), so cross-host clock offset never enters. This is
  why the number is a *round trip*.
- **Correctness check on every run.** The sender predicts the receiver's final
  checksum (an order-independent sum of per-message hashes over exactly the
  packets it sent). The receiver must match it exactly, and any lost,
  duplicated or corrupted message breaks the match. A latency number from a run
  whose checksum doesn't match is flagged `MISMATCH` in the report.
- **Sender-limited runs are excluded from max-rate.** If the sender delivered
  less than 95% of the offered rate, the run measured the sender. On loopback
  and veth the sender's `sendmmsg` also does the receiver's softirq work, so
  every CI/WSL max-rate run above ~0.4 Mpps is sender-limited. The report then
  says "limit not reached / not measured" instead of inventing a ceiling.
- **Repetitions and the noise verdict.** CI repeats every latency configuration
  3 times, with repetitions as the outer loop so host drift spreads across all
  configs. A tuning step is only "better" or "worse" when its min–max p99.9
  range across reps is disjoint from the pinned baseline's; otherwise it is
  "within noise". This is what kept me from claiming effects that were one-run
  artifacts (the first single-run CI pass showed pinning both helping and
  hurting by tens of percent).

## 4. The compile-time decoder

`include/bl/schema.hpp` + `include/bl/itch.hpp`:

- A message is a type: `Message<'A', 36, AddOrder, Field<&AddOrder::ref, 11>, ...>`.
  A field binds a pointer-to-member, an offset, a wire width and an encoding.
- Generated from that: the per-field load (a 48-bit timestamp becomes a 16- and a
  32-bit bswap load), a 256-entry `constexpr` length table for framing checks,
  and two dispatch strategies: a fold-expression compare chain the compiler
  lowers to a jump table, and a `constexpr` function-pointer table.
- **Checked at compile time** (`tests/schema_static_checks.cpp`, run by CTest
  as must-fail compiles that must also emit the right message): field past end
  of message, overlapping fields, wrong struct, width too wide for the
  destination, unsupported big-endian width, offset 0, and duplicate type byte.
- **Result:** on the 7,628,856-message 2019-01-30 capture, generated and
  hand-written decoders produce identical output for every message and run at
  the same speed (11.3–11.6 ns/msg best-of on an i5-9300H). The disassembly is
  essentially identical: ~510 instructions and one indirect jump each.
- **The honest wrinkle.** At first the generated decoder looked a few percent
  *faster*. The disassembly showed why: GCC 13 had left the hand-written switch
  out of line (2 calls per message) while the generated one was force-inlined.
  With the same `always_inline` on both, the gap vanishes. The claim is
  "zero-cost abstraction plus compile-time spec checking", not "faster than
  hand-written".
- The function-pointer table (one indirect call per message, visitor not
  inlined) was 5–9% faster than both on GCC and clang in CI. The likely reason
  is a smaller loop body with a well-predicted indirect target. That's a
  hypothesis; no perf counters were collected.

## 5. Linux tuning: what each knob does

- **Pinning** (`pthread_setaffinity_np` / EAL `-l`). Stops migrations and the
  cold caches and TLB that come with them. For a *blocking* receiver it also
  fixes where the wake-up lands.
- **IRQ affinity** (`scripts/tune/irq_affinity.sh`). Steer the NIC's interrupts
  to housekeeping CPUs and stop irqbalance. Subtle point: for a *blocking*
  socket, moving the IRQ off the app's core can *raise* latency (cross-core
  wake-up, softirq on another core, cache lines bouncing), while for a spinning
  receiver it removes interruptions. That's why it is measured, not assumed.
  DPDK has no interrupts on the data NIC, so its exposure is only to the
  management ENI's IRQs.
- **isolcpus / nohz_full / rcu_nocbs** (`scripts/tune/isolcpus.sh`, needs a
  reboot). Removes the hot core from the scheduler's load balancing, stops the
  periodic tick when one task runs, and offloads RCU callbacks. The hyperthread
  sibling is isolated too and left idle, or the "isolated" core still shares
  its execution units.
- **Huge pages.** For DPDK the mempool lives in 2 MB pages (fewer TLB misses,
  physically contiguous for DMA). `--no-huge` is the 4 KB control, and it may be
  refused under vfio no-IOMMU on EC2, which is recorded either way. For AF_XDP
  the UMEM is 16 MB: 4096 TLB entries with 4 KB pages vs 8 with 2 MB.

## 6. Honest scope

- AWS cloud VMs, not colocation. ENA is not a Solarflare/ExaNIC; there's no
  ef_vi/TCPDirect, no RDMA, and the VPC adds its own hops and shaping (ENA's
  `pps_allowance_exceeded` counters are captured per config for this reason).
- **The AWS run has not been done** (no AWS account on the build machine at
  the time). Terraform, bootstrap and `run_aws.sh` exist and pass
  `terraform validate`/shellcheck in CI but have never executed.
- c6in.2xlarge is single-socket: no NUMA claims.
- CI numbers come from a veth pair on a shared 4-vCPU VM. DPDK there runs
  through the `net_af_packet` PMD, which is *not* kernel bypass (it reads an
  AF_PACKET ring), and AF_XDP runs in copy mode. CI proves the four code paths
  work end to end and gives a relative ordering. It does not give NIC numbers.

## 7. Likely questions

- *Why measure round trip instead of one-way?* One-way needs synchronized
  clocks (PTP) good to well under the latency being measured. A round trip on
  one clock needs none. The cost is that the sender's own receive path is
  inside every number, and it's identical across the four receiver paths, so
  comparisons stay fair.
- *Why is DPDK not obviously faster than AF_XDP in CI?* In CI it isn't DPDK's
  real data path: af_packet PMD over veth. At 100k pps its p99.9 was 173 µs vs AF_XDP's 48 µs (ranges disjoint over 3 reps).
  On ENA with vfio-pci it would be a different machine entirely, so that
  comparison is the main thing the AWS run would answer.
- *What would you change for production?* Hardware timestamps
  (`SO_TIMESTAMPING` / NIC PTP clock) instead of user-space time, a book behind
  the decoder, A/B feed arbitration with gap recovery (the seq/gap counters are
  the start of that), multicast instead of unicast, and a DPDK sender so the
  sender isn't the throughput ceiling.
- *Why is your max-pps number "not measured"?* Because every run above ~0.4
  Mpps was sender-limited, and reporting the sender's ceiling as the receiver's
  would be wrong.
- *How do you know the decoder is right?* Every message of a full real trading
  day decodes identically through three independent implementations, all 23
  message types are length-validated (0 unknown, 0 bad length), hand-crafted
  known-value tests pass, and the spec table is checked at compile time.
