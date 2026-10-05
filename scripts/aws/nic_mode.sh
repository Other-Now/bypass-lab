#!/usr/bin/env bash
# Put the receiver's data ENI in the mode a receive path needs (root).
#   nic_mode.sh dpdk                         -> bind to vfio-pci (kernel loses the NIC)
#   nic_mode.sh kernel|recvmsg|recvmmsg|afxdp -> bind back to ena, re-apply IP/queues
# Used as run_matrix.sh's RX_PREPARE hook, so it is called with the path name.
set -euo pipefail
# shellcheck source=/dev/null
source /etc/bypass-lab.env
want=kernel
[[ $1 == dpdk ]] && want=dpdk
drv=$(basename "$(readlink -f "/sys/bus/pci/devices/$BL_DATA_PCI/driver" 2>/dev/null)" 2>/dev/null || echo none)

if [[ $want == dpdk && $drv != vfio-pci ]]; then
  ip link set "$BL_DATA_IF" down 2>/dev/null || true
  dpdk-devbind.py -b vfio-pci "$BL_DATA_PCI"
elif [[ $want == kernel && $drv != ena ]]; then
  dpdk-devbind.py -b ena "$BL_DATA_PCI"
  sleep 2
  bash "$BL_REPO/scripts/aws/net_setup.sh"
fi
dpdk-devbind.py -s | grep -F "$BL_DATA_PCI" || true
