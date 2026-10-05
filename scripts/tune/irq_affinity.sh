#!/usr/bin/env bash
# Tuning step: move NIC interrupts off the hot core (root).
#   irq_affinity.sh on  "0,4"   stop irqbalance, steer every ENA IRQ to CPUs 0,4
#   irq_affinity.sh off         restore: all CPUs allowed, irqbalance back on
#   irq_affinity.sh show        print where each ENA IRQ may run
# Both ENIs (management and data) are moved: either one interrupting the hot
# core is the noise being removed.
set -euo pipefail
irqs() { awk -F: '/ena|ens[0-9]|eth[0-9]/ {gsub(/ /, "", $1); print $1}' /proc/interrupts; }

case ${1:-show} in
  on)
    cpus=${2:?cpu list}
    systemctl stop irqbalance 2>/dev/null || true
    for i in $(irqs); do echo "$cpus" >"/proc/irq/$i/smp_affinity_list" 2>/dev/null || true; done
    ;;
  off)
    all="0-$(($(nproc --all) - 1))"
    for i in $(irqs); do echo "$all" >"/proc/irq/$i/smp_affinity_list" 2>/dev/null || true; done
    systemctl start irqbalance 2>/dev/null || true
    ;;
  show) ;;
  *) echo "usage: $0 on CPUS | off | show" >&2; exit 2 ;;
esac
for i in $(irqs); do
  printf '%s %s -> %s\n' "$i" "$(awk -F: -v n="$i" '$1+0==n {print $NF}' /proc/interrupts | xargs)" \
    "$(cat "/proc/irq/$i/effective_affinity_list" 2>/dev/null || cat "/proc/irq/$i/smp_affinity_list")"
done
