#!/usr/bin/env bash
# The CI experiment: all four receive paths over the veth pair, plus the tuning
# steps that are meaningful on a 4-vCPU CI VM (pinning, huge pages). IRQ
# affinity and isolcpus need real NIC interrupts and a reboot respectively, so
# they only exist in the AWS run (scripts/aws/run_aws.sh).
#
# Core layout comes from the topology (scripts/tune/cpu_layout.sh). A 4-vCPU
# runner is 2 cores x 2 threads: the receiver gets the first thread of the last
# core with its sibling left idle; the sender's tx and echo threads share the
# other core with housekeeping. Hard-coding 1/2/3 would have put the receiver
# on the same physical core as the sender's echo thread.
set -euo pipefail
cd "$(dirname "$0")/../.."

export BIN_DIR=${BIN_DIR:-build}
export OUT_DIR=${OUT_DIR:-results/ci-veth}
export RX_MODE=netns RX_NS=bl-rx DST_IP=10.77.0.2 RX_IFACE=veth-rx
export RX_PREPARE="bash $PWD/scripts/ci/rx_prepare.sh"
export FEED=${FEED:-data/sample.NASDAQ_ITCH50}
export XDP_MODE=${XDP_MODE:-native}
RATES_LAT=${RATES_LAT:-"10000 100000"}
SECS=${SECS:-10}
ALL="recvmsg recvmmsg afxdp dpdk"
EAL_COMMON="--no-pci --vdev=net_af_packet0,iface=veth-rx --file-prefix=bl --log-level=lib.eal:warning"

eval "$(bash scripts/tune/cpu_layout.sh)"
SND_TX=${HK##*[,-]}   # second thread of core 0
SND_ECHO=${HK%%[,-]*} # first thread of core 0
echo "layout: receiver $HOT (sibling $SIBLING idle), sender tx $SND_TX echo $SND_ECHO"

rm -rf "$OUT_DIR"
run() { env "$@" bash scripts/run_matrix.sh; }

# 1. unpinned: the scheduler decides. DPDK always pins its lcore, so let it float
#    over all four CPUs instead.
run LABEL=unpinned PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    DPDK_EAL="--lcores='0@(0-3)' $EAL_COMMON"  # quoted: bash -c re-parses it

# 2. pinned: every path on $HOT, sender on core 0. The reference for the rest.
run LABEL=pinned PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    RX_CORE=$HOT TX_CORE=$SND_TX ECHO_CORE=$SND_ECHO DPDK_EAL="-l $HOT $EAL_COMMON"

# 3. huge-page step, one path at a time against `pinned`:
#    AF_XDP UMEM 4K -> 2M pages; DPDK mempool 2M pages -> --no-huge (4K).
run LABEL=pinned+umem2M PATHS="afxdp" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    RX_CORE=$HOT TX_CORE=$SND_TX ECHO_CORE=$SND_ECHO XDP_EXTRA="--umem-huge"
run LABEL=pinned+dpdk-nohuge PATHS="dpdk" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    RX_CORE=$HOT TX_CORE=$SND_TX ECHO_CORE=$SND_ECHO DPDK_EAL="-l $HOT --no-huge -m 512 $EAL_COMMON"

# 4. throughput: no echoes, rising rate, two sender threads on core 0's two
#    hyperthreads (tx threads take TX_CORE, TX_CORE+1).
run LABEL=maxrate MODE=maxrate PATHS="$ALL" RATES="${RATES_MAX:-250000 500000 1000000 1500000 2000000}" \
    SECONDS_PER_RUN=5 TX_THREADS=2 RX_CORE=$HOT TX_CORE=$SND_ECHO DPDK_EAL="-l $HOT $EAL_COMMON"
