#!/usr/bin/env bash
# The AWS experiment. Runs on the SENDER instance (as ubuntu); drives the
# receiver over SSH ("Host rx", set up by the Terraform bootstrap).
#
# Tuning ladder, one change at a time against `pinned`:
#   unpinned            scheduler places the receive loop; irqbalance on
#   pinned              receive loop pinned to HOT                  <- reference
#   pinned+irq          + all ENA IRQs steered to housekeeping CPUs
#   pinned+umem2M       + AF_XDP UMEM on 2 MB pages        (afxdp only)
#   pinned+dpdk-nohuge  DPDK mempool on 4 KB pages instead (dpdk only; may be
#                       refused under vfio no-IOMMU -- recorded either way)
#   pinned+isol         + isolcpus/nohz_full/rcu_nocbs on HOT,SIBLING (reboot)
#   all                 isol + irq (+ umem2M for afxdp)
#   maxrate             pinned, no echoes, rising rates
set -euo pipefail
# shellcheck source=/dev/null
source /etc/bypass-lab.env
cd "$BL_REPO"

RATES_LAT=${RATES_LAT:-"10000 100000 500000"}
RATES_MAX=${RATES_MAX:-"500000 1000000 1500000 2000000 3000000 4000000"}
SECS=${SECS:-20}
ALL="recvmsg recvmmsg afxdp dpdk"

rxs() { ssh rx "$@"; }
eval "$(rxs cat /etc/bypass-lab.env | grep -E '^BL_DATA_(IF|PCI)=' | sed 's/^BL_DATA_/RX_/')"
eval "$(rxs bash "$BL_REPO/scripts/tune/cpu_layout.sh")"   # HOT SIBLING HK SOCKETS (receiver)
echo "receiver: data $RX_IF ($RX_PCI); hot=$HOT sibling=$SIBLING housekeeping=$HK sockets=$SOCKETS"

export BIN_DIR=build RX_BIN_DIR="$BL_REPO/build" OUT_DIR=${OUT_DIR:-results/aws}
export RX_MODE=ssh RX_HOST=rx RX_SUDO=sudo TX_SUDO=sudo
export DST_IP=$BL_PEER_DATA_IP RX_IFACE=$RX_IF XDP_MODE=native
export RX_PREPARE="sudo bash $BL_REPO/scripts/aws/nic_mode.sh"
export FEED=${FEED:-data/sample.NASDAQ_ITCH50}
# Sender: tx and echo on different physical cores, away from cpu0.
SND=(TX_CORE=1 ECHO_CORE=2)
EAL="-l $HOT -a $RX_PCI${DPDK_DEVARGS:+,$DPDK_DEVARGS} --file-prefix=bl"

sudo sysctl -qw net.core.busy_read=50 net.core.busy_poll=50

snapshot() {  # ENA's own counters: did AWS shape us (pps/bw allowance)?
  local label=$1
  rxs "sudo bash $BL_REPO/scripts/aws/nic_mode.sh kernel >/dev/null; ethtool -S $RX_IF" \
    | grep -E 'allowance|queue_0_rx_(cnt|drops)|rx_drops' >"$OUT_DIR/$label/ena_stats.txt" || true
}
run() {
  local label=$1; shift
  env LABEL="$label" "${SND[@]}" "$@" bash scripts/run_matrix.sh
  snapshot "$label"
}
irq() { rxs "sudo bash $BL_REPO/scripts/tune/irq_affinity.sh $*" >/dev/null; }
reboot_rx() {
  rxs "sudo reboot" || true
  sleep 20
  for _ in $(seq 1 60); do rxs "systemctl is-active bypass-lab-net" >/dev/null 2>&1 && return 0; sleep 5; done
  echo "receiver did not come back" >&2; exit 1
}

# ---- phase A: stock kernel command line ------------------------------------
rxs "sudo bash $BL_REPO/scripts/tune/isolcpus.sh off" >/dev/null
if [[ -n $(rxs cat /sys/devices/system/cpu/isolated) ]]; then reboot_rx; fi
irq off

run unpinned PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    DPDK_EAL="--lcores='0@(0-$(($(rxs nproc) - 1)))' -a $RX_PCI --file-prefix=bl"
run pinned PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" RX_CORE="$HOT" DPDK_EAL="$EAL"
irq on "$HK"
run pinned+irq PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" RX_CORE="$HOT" DPDK_EAL="$EAL"
irq off
run pinned+umem2M PATHS=afxdp RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" RX_CORE="$HOT" XDP_EXTRA=--umem-huge
run pinned+dpdk-nohuge PATHS=dpdk RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    DPDK_EAL="-l $HOT -a $RX_PCI --no-huge -m 512 --file-prefix=bl"
run maxrate MODE=maxrate PATHS="$ALL" RATES="$RATES_MAX" SECONDS_PER_RUN=5 TX_THREADS=3 RX_CORE="$HOT" \
    DPDK_EAL="$EAL"

# ---- phase B: isolated hot core ------------------------------------------
rxs "sudo bash $BL_REPO/scripts/tune/isolcpus.sh on $HOT${SIBLING:+,$SIBLING}" >/dev/null
reboot_rx
echo "receiver isolated: $(rxs cat /sys/devices/system/cpu/isolated)"
irq off
run pinned+isol PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" RX_CORE="$HOT" DPDK_EAL="$EAL"
irq on "$HK"
run all PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" RX_CORE="$HOT" DPDK_EAL="$EAL" \
    XDP_EXTRA=--umem-huge

python3 analysis/report.py "$OUT_DIR" --out "$OUT_DIR/REPORT.md" --plots "$OUT_DIR/plots" \
  --title "AWS $(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null) pair, cluster placement group" \
  --baseline pinned
echo "done: $OUT_DIR/REPORT.md"
