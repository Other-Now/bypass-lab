#!/usr/bin/env bash
# Two-"host" topology on one Linux box: the receiver lives in network namespace
# bl-rx behind a veth pair, so its traffic crosses a real (virtual) NIC with its
# own XDP hook instead of loopback.
#
#   root ns: bl_sender  10.77.0.1 [veth-tx] <====> [veth-rx] 10.77.0.2  netns bl-rx: bl_rx
#
# This is for functional end-to-end testing of all four paths in CI. veth has no
# hardware queue, no interrupts and no DMA, so its numbers say nothing about a
# real NIC; see README "What the CI numbers are".
set -euo pipefail
NS=${RX_NS:-bl-rx}

ip netns del "$NS" 2>/dev/null || true
ip link del veth-tx 2>/dev/null || true

ip netns add "$NS"
ip link add veth-tx type veth peer name veth-rx
ip link set veth-rx netns "$NS"
ip addr add 10.77.0.1/24 dev veth-tx
ip link set veth-tx up
ip netns exec "$NS" ip addr add 10.77.0.2/24 dev veth-rx
ip netns exec "$NS" ip link set veth-rx up
ip netns exec "$NS" ip link set lo up
# One queue each way, as on the AWS data ENI (ethtool -L combined 1).
ethtool -L veth-tx tx 1 rx 1 2>/dev/null || true
ip netns exec "$NS" ethtool -L veth-rx tx 1 rx 1 2>/dev/null || true

ping -c 1 -W 2 10.77.0.2 >/dev/null
echo "veth ready: 10.77.0.1 (veth-tx) -> 10.77.0.2 (veth-rx in $NS)"
