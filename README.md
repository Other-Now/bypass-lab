# bypass-lab — one feed handler, four receive paths, measured honestly

A NASDAQ ITCH 5.0 feed replayed as UDP into **the same feed handler behind four
Linux receive paths**:

| path | what it removes |
|---|---|
| `recvmsg` | nothing: blocking socket, one syscall per packet, thread sleeps between packets |
| `recvmmsg` + busy poll | sleeping and per-packet syscalls: 64 packets/syscall, thread spins, `SO_BUSY_POLL` |
| **AF_XDP** | the network stack: an XDP program redirects feed packets into a user-space ring before an skb exists |
| **DPDK** | the kernel: NIC on vfio-pci, user-space poll-mode driver, huge-page mempool |

Plus a **compile-time generated ITCH decoder** (templates + `constexpr`, spec
table checked by `static_assert`), **Linux tuning applied one change at a
time**, **Python tooling** that turns raw samples into tables and plots, and
**Terraform** for a two-node AWS run.

> **Status, stated plainly.** All four paths build and run end to end in CI on
> a veth pair, repeated 3×, and those are the numbers below. **The AWS/ENA run
> has not been executed yet**: the Terraform, bootstrap and run scripts are
> complete and pass `terraform validate` + shellcheck in CI, but they've never
> run against a real account. No number in this README comes from AWS.

---

## Results (CI: GitHub ubuntu-24.04 runner, AMD EPYC 7763, 4 vCPU, veth pair, 3 repetitions)

Round trip from **scheduled** send time to echo arrival, measured on the
sender's clock, with warm-up dropped. Each cell is the median of 3 repetitions
(min–max in brackets). **60 latency runs, 0 packets lost, and the receiver's
checksum matched the sender's prediction on every one.**

**Pinned** (receiver alone on one physical core, sibling idle):

| path | p50 @ 10k pps | p99 @ 10k | p50 @ 100k pps | p99 @ 100k | p99.9 @ 100k |
|---|---:|---:|---:|---:|---:|
| recvmsg | 25.0 µs (25.0–25.0) | 30.1 | 17.5 µs (17.4–17.7) | 36.6 | 61.7 (47.7–504.6) |
| recvmmsg + busy poll | 12.3 µs (12.3–12.3) | 16.9 | 12.5 µs (12.4–12.5) | 25.2 | 58.6 (31.8–93.6) |
| **AF_XDP** (copy mode) | **9.2 µs** (9.1–9.2) | **11.8** | **9.0 µs** (8.9–9.0) | **21.2** | **47.7** (32.0–68.3) |
| DPDK via `net_af_packet`* | 10.1 µs (10.1–10.2) | 22.8 | 10.0 µs (10.0–10.0) | 33.6 | 173.2 (171.6–247.9) |

\* On veth, DPDK can only attach through the `af_packet` PMD, which reads an
AF_PACKET ring. **That is not kernel bypass.** This row checks that the DPDK
code path works; it is not a DPDK performance number.

What survives the repetitions:

1. **The median ordering is real.** The p50 ranges for recvmsg > recvmmsg >
   DPDK/af_packet > AF_XDP don't overlap at either rate. AF_XDP's median round
   trip is **2.7× lower than blocking `recvmsg` at 10k pps** (9.2 vs 25.0 µs)
   and 1.9× lower at 100k. Most of the gap is the sleep/wake-up:
   busy-polling `recvmmsg` alone gets to 12.3 µs.
2. **Blocking `recvmsg` slows down at *lower* rate** (25.0 µs at 10k vs 17.5 at
   100k). At 10k pps the thread is asleep when each packet arrives, so every
   packet pays the full wake-up. At 100k there is often another packet queued.
3. **Pinning's only robust tail effect is on the blocking path.** Unpinned
   `recvmsg` at 10k pps: p99.9 784 µs vs 72 µs pinned, with disjoint ranges.
   Unpinning raised the median on every path (e.g. AF_XDP 9.2 → 9.6 µs, ranges
   disjoint).
4. **Most tuning "effects" are noise at this scale, and the report says so.**
   AF_XDP UMEM on 2 MB pages (−57% / −29% p99.9) and DPDK `--no-huge`
   (+130% / +21%) both look like big effects. Their 3-rep ranges overlap the
   baseline, so `analysis/report.py` marks them **within noise**. A first,
   single-repetition CI pass had shown pinning both helping and hurting by tens
   of percent, which is why repetitions were added before any tuning claim.

**Max packets/s per core: not measured.** Over veth the sender's `sendmmsg`
also runs the receiver's softirq work, so the sender saturated at
**~335–420k pps** for every path. Every receive path took everything it was
offered with no loss up to that point (≥ 250k pps per core). The report
excludes sender-limited runs rather than presenting the sender's ceiling as the
receiver's. Finding the real per-core limit is what the AWS run is for.

Full tables and percentile plots: [results/ci-veth/REPORT.md](results/ci-veth/REPORT.md).
Raw per-packet samples are CI artifacts (too large for git).

### The generated decoder costs nothing (7,628,856 real messages)

Full NASDAQ trading day (2019-01-30, all 23 message types, 97.1% decoded),
best of 11 interleaved passes, i5-9300H / GCC 13:

| decoder | ns/msg | instructions in loop | dispatch |
|---|---:|---:|---|
| framing only (length table) | 2.8 | 19 | — |
| hand-written switch | 11.3–11.6 | 519 | 1 jump table |
| **generated** (fold expression) | **11.3–11.6** | **509** | 1 jump table |
| generated (constexpr fn-ptr table) | 11.0–11.5 | 42 | 1 indirect call |

Identical output for every message across all three implementations (0
unknown types, 0 bad lengths). The same holds on GCC and Clang in CI, where the
function-pointer table was 5–9% faster on both.

**The honest wrinkle:** the generated decoder first measured a few percent
*faster* than hand-written. The disassembly
([scripts/dispatch_asm.sh](scripts/dispatch_asm.sh)) showed GCC had left the
hand-written switch out of line, costing 2 calls per message. With the same
`always_inline` on both, the difference disappears. The claim is zero-cost
generation plus compile-time checking of the spec, not "faster than
hand-written".

---

## How it works

```
 sender host                                            receiver host
 ┌──────────────────────────────┐   UDP feed :9000   ┌──────────────────────────────┐
 │ bl_sender                    │ ─────────────────▶ │ bl_rx --path recvmsg|recvmmsg│
 │  tx thread: open-loop        │                    │        |afxdp|dpdk           │
 │   schedule, stamps sched_ns  │                    │  FeedHandler (identical)     │
 │  echo thread: busy-poll      │ ◀───────────────── │   decode → checksum → echo   │
 │   recvmmsg, rtt = rx - sched │   echo :9001       │                              │
 └──────────────────────────────┘                    └──────────────────────────────┘
```

- **Latency from the schedule, not from the send** ([src/sender.cpp](src/sender.cpp)).
  Packet *i* is due at `t0 + i/rate`. A late packet keeps its due time, so
  backlog is charged to latency (no coordinated omission, same correction as
  tick2trade). Both timestamps are on the sender's clock, so no PTP is needed.
- **Correctness on every run.** The sender predicts the receiver's
  order-independent checksum from exactly the packets it sent. Any lost,
  duplicated or corrupted message produces `MISMATCH`.
- **The handler is shared** ([include/bl/handler.hpp](include/bl/handler.hpp)),
  so the paths differ only in I/O.
- **AF_XDP** ([src/rx_afxdp.cpp](src/rx_afxdp.cpp)). A custom XDP program
  ([src/bpf/xdp_udp_redirect.bpf.c](src/bpf/xdp_udp_redirect.bpf.c)) redirects
  only the feed port, so SSH/ARP keep working. FILL/RX/TX/COMPLETION rings, with
  the echo rewritten **in place** in the received UMEM frame. Zero-copy is tried
  first, then copy mode; `--umem-huge` puts UMEM on 2 MB pages.
- **DPDK** ([src/rx_dpdk.cpp](src/rx_dpdk.cpp)). One RX/TX queue, a mempool on
  huge pages, the echo rewritten in place in the received mbuf, and all device
  and memory choices passed as EAL arguments.
- **Decoder** ([include/bl/schema.hpp](include/bl/schema.hpp),
  [include/bl/itch.hpp](include/bl/itch.hpp)). Each ITCH message is declared as
  a type listing its fields (`Message<'A', 36, AddOrder, Field<&AddOrder::ref, 11>, …>`).
  From that the compiler generates the loads, a 256-entry length table and the
  dispatch. It also rejects bad spec tables: overlapping fields, a field past
  the end, a duplicate type byte, a field bound to the wrong struct, a bad
  width, offset 0. Each of these is a CTest that must *fail to compile* with
  the right message ([tests/schema_static_checks.cpp](tests/schema_static_checks.cpp)).

## Tuning, one change at a time

| step | script | CI (veth) | AWS (`run_aws.sh`) |
|---|---|---|---|
| pin receive loop (sibling hyperthread idle) | `pthread_setaffinity_np` / EAL `-l` | ✔ measured | ready |
| NIC IRQs → housekeeping cores, irqbalance off | [scripts/tune/irq_affinity.sh](scripts/tune/irq_affinity.sh) | n/a (veth has no IRQs) | ready |
| `isolcpus` + `nohz_full` + `rcu_nocbs` | [scripts/tune/isolcpus.sh](scripts/tune/isolcpus.sh) | n/a (needs reboot) | ready (reboots receiver) |
| huge pages: AF_XDP UMEM 4K→2M, DPDK 2M→4K | `--umem-huge`, `--no-huge` | ✔ measured (within noise) | ready |

Core roles come from the real topology ([scripts/tune/cpu_layout.sh](scripts/tune/cpu_layout.sh)),
not assumed numbering. On the CI runner, hard-coded cores would have put the
receiver on the same physical core as the sender's echo thread.

## Running it

```bash
# build + tests (Linux; AF_XDP/DPDK auto-detected, needs libxdp-dev libbpf-dev clang libdpdk-dev)
make test                      # 314 unit checks + 7 must-fail compiles + pytest
make bench                     # generated vs hand-written decoder

# the CI experiment on any Linux box (needs root for netns/XDP/huge pages)
sudo bash scripts/ci/veth_setup.sh && bash scripts/ci/run_ci_matrix.sh
python3 analysis/report.py results/ci-veth --out results/ci-veth/REPORT.md --plots results/ci-veth/plots

# AWS (two c6in.2xlarge spot instances, cluster placement group; ~$0.2-0.4/h for the pair)
make aws-up && make aws-run && make aws-fetch && make aws-down
```

The AWS stack ([infra/terraform](infra/terraform)) gives each node a second ENI
in its own subnet as the data plane, so unbinding it for DPDK can't cut off
SSH. Each instance also schedules its own termination (`ttl_minutes`, default
240) as a guard against a forgotten `aws-down`.

## Honest scope

- **AWS cloud VMs, not colocation.** ENA is not a Solarflare/ExaNIC: no
  ef_vi/TCPDirect, no RDMA, and the VPC shapes traffic (ENA's
  `*_allowance_exceeded` counters are captured per config). **Not yet run.**
- **No NUMA claims.** c6in.2xlarge is single-socket; the CI runner reports 1
  NUMA node.
- **CI numbers are veth numbers.** They come from a shared VM with no NIC, no
  DMA and no interrupts. DPDK runs through `net_af_packet`, and AF_XDP runs in
  copy mode (`"zerocopy":false` in every receiver JSON). They show the paths
  work and rank them; they are not hardware latencies.
- **Timestamps are user-space `CLOCK_MONOTONIC`**, not NIC hardware
  timestamps, so the sender's own receive path is in every sample (identically
  for all four paths).
- WSL2 loopback runs ([results/wsl-loopback](results/wsl-loopback)) were
  smoke tests only. The Hyper-V host puts millisecond noise in the tail.

## Layout

```
include/bl/   schema.hpp (decoder generator) itch.hpp (ITCH schema) handler.hpp wire.hpp net.hpp feed.hpp
src/          sender.cpp rx_main.cpp rx_socket.cpp rx_afxdp.cpp rx_dpdk.cpp bpf/xdp_udp_redirect.bpf.c
bench/        decoder_bench.cpp
tests/        test_main.cpp schema_static_checks.cpp
analysis/     report.py (tables, noise verdicts, plots) test_report.py
scripts/      run_matrix.sh  ci/  tune/  aws/
infra/terraform/
docs/NOTES.md design notes and likely interview questions
```

`tools/gen_sample.cpp` and `data/sample.NASDAQ_ITCH50` come from
[tick2trade](https://github.com/Other-Now/tick2trade). The full-day capture is
not redistributable and is not in the repo.
