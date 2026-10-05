#!/usr/bin/env bash
# The CI experiment: all four receive paths over the veth pair, plus the tuning
# steps that are meaningful on a 4-vCPU CI VM (pinning, huge pages). IRQ
# affinity and isolcpus need real NIC interrupts and a reboot respectively, so
# they only exist in the AWS run (scripts/aws/run_aws.sh).
#
# Core layout on a 4-vCPU runner: 0 = housekeeping, 1 = sender tx,
# 2 = sender echo rx, 3 = receiver.
set -euo pipefail
cd "$(dirname "$0")/../.."

export BIN_DIR=${BIN_DIR:-build}
export OUT_DIR=${OUT_DIR:-results/ci-veth}
export RX_MODE=netns RX_NS=bl-rx DST_IP=10.77.0.2 RX_IFACE=veth-rx
export RX_PREPARE="$PWD/scripts/ci/rx_prepare.sh"
export FEED=${FEED:-data/sample.NASDAQ_ITCH50}
export XDP_MODE=${XDP_MODE:-native}
RATES_LAT=${RATES_LAT:-"10000 100000"}
SECS=${SECS:-10}
ALL="recvmsg recvmmsg afxdp dpdk"
EAL_COMMON="--no-pci --vdev=net_af_packet0,iface=veth-rx --file-prefix=bl --log-level=lib.eal:warning"

rm -rf "$OUT_DIR"
run() { env "$@" bash scripts/run_matrix.sh; }

# 1. unpinned: the scheduler decides. DPDK always pins its lcore, so let it float
#    over all four CPUs instead.
run LABEL=unpinned PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    DPDK_EAL="--lcores='0@(0-3)' $EAL_COMMON"  # quoted: bash -c re-parses it

# 2. pinned: every path on core 3, sender on 1/2. The reference for the rest.
run LABEL=pinned PATHS="$ALL" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    RX_CORE=3 TX_CORE=1 ECHO_CORE=2 DPDK_EAL="-l 3 $EAL_COMMON"

# 3. huge-page step, one path at a time against `pinned`:
#    AF_XDP UMEM 4K -> 2M pages; DPDK mempool 2M pages -> --no-huge (4K).
run LABEL=pinned+umem2M PATHS="afxdp" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    RX_CORE=3 TX_CORE=1 ECHO_CORE=2 XDP_EXTRA="--umem-huge"
run LABEL=pinned+dpdk-nohuge PATHS="dpdk" RATES="$RATES_LAT" SECONDS_PER_RUN="$SECS" \
    RX_CORE=3 TX_CORE=1 ECHO_CORE=2 DPDK_EAL="-l 3 --no-huge -m 512 $EAL_COMMON"

# 4. throughput: no echoes, rising rate, two sender threads (cores 1-2).
run LABEL=maxrate MODE=maxrate PATHS="$ALL" RATES="${RATES_MAX:-250000 500000 1000000 1500000 2000000}" \
    SECONDS_PER_RUN=5 TX_THREADS=2 RX_CORE=3 TX_CORE=1 DPDK_EAL="-l 3 $EAL_COMMON"
