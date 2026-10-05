#!/usr/bin/env bash
# Run inside the receiver namespace before each path (RX_PREPARE hook).
#
# DPDK's af_packet PMD (the only way to point DPDK at a veth) *taps* frames
# rather than stealing them, so the namespace's kernel also sees every feed
# packet, finds no socket on port 9000 and answers with ICMP port-unreachable --
# which then surfaces on the sender's connected socket as ECONNREFUSED and
# fails its next send. For the dpdk path only, drop the feed in INPUT (after
# AF_PACKET has already seen it). On real hardware with vfio-pci the kernel
# never sees the packets and this is not needed.
set -euo pipefail
path=$1
rule=(INPUT -p udp --dport "${BL_PORT:-9000}" -j DROP)
if [[ $path == dpdk ]]; then
  iptables -C "${rule[@]}" 2>/dev/null || iptables -A "${rule[@]}"
else
  while iptables -C "${rule[@]}" 2>/dev/null; do iptables -D "${rule[@]}"; done
fi
