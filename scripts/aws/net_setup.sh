#!/usr/bin/env bash
# Data-ENI setup on an AWS node (root; run by bypass-lab-net.service at every
# boot). Finds the data ENI by MAC, gives it its fixed IP, and records the
# interface name and PCI address in /etc/bypass-lab.env for the other scripts.
set -euo pipefail
# shellcheck source=/dev/null
source /etc/bypass-lab.env

# The ENI is attached by Terraform after the instance boots: wait for hotplug.
IF=""
for _ in $(seq 1 300); do
  IF=$(ip -o link | awk -v m="${BL_DATA_MAC,,}" 'tolower($0) ~ m {sub(":", "", $2); print $2; exit}')
  [[ -n $IF ]] && break
  # Already bound to vfio-pci (a DPDK run was in progress): nothing to do.
  if [[ -n ${BL_DATA_PCI:-} && -e /sys/bus/pci/drivers/vfio-pci/$BL_DATA_PCI ]]; then exit 0; fi
  sleep 1
done
[[ -n $IF ]] || { echo "data ENI with MAC $BL_DATA_MAC never appeared" >&2; exit 1; }
PCI=$(ethtool -i "$IF" | awk '/bus-info/ {print $2}')

# ENA native XDP requires MTU <= 3498 (VPC default is 9001). Same MTU for every
# path so no path gets a framing advantage.
ip link set "$IF" mtu 1500
ip addr replace "$BL_DATA_IP/24" dev "$IF"
ip link set "$IF" up
# One combined queue: the whole feed is one flow, so one queue on one core, and
# AF_XDP (bound to queue 0) is guaranteed to see every packet.
ethtool -L "$IF" combined 1 || true
# DPDK on the far side does not answer ARP; pin the neighbour.
ip neigh replace "$BL_PEER_DATA_IP" lladdr "$BL_PEER_MAC" dev "$IF" nud permanent

sed -i '/^BL_DATA_IF=/d;/^BL_DATA_PCI=/d' /etc/bypass-lab.env
printf 'BL_DATA_IF=%s\nBL_DATA_PCI=%s\n' "$IF" "$PCI" >>/etc/bypass-lab.env

# vfio-pci for DPDK. EC2 instances expose no IOMMU to the guest, so vfio must
# run in no-IOMMU mode (AWS's documented setup for DPDK on ENA).
modprobe vfio-pci || true
echo 1 >/sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true
echo "data ENI: $IF ($PCI) $BL_DATA_IP"
