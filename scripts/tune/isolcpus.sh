#!/usr/bin/env bash
# Tuning step: isolate the hot core from the scheduler and the timer tick
# (root; takes effect after a reboot).
#   isolcpus.sh on "3,7"   isolcpus + nohz_full + rcu_nocbs on CPUs 3,7
#   isolcpus.sh off        remove
# isolcpus=managed_irq also keeps managed (driver-allocated) IRQs off those CPUs
# where the driver allows it; ordinary IRQ steering is a separate step
# (irq_affinity.sh) so the two effects can be measured apart.
set -euo pipefail
f=/etc/default/grub.d/99-bypass-lab.cfg
case ${1:-} in
  on)
    c=${2:?cpu list}
    cat >"$f" <<EOF
GRUB_CMDLINE_LINUX_DEFAULT="\$GRUB_CMDLINE_LINUX_DEFAULT isolcpus=managed_irq,domain,$c nohz_full=$c rcu_nocbs=$c"
EOF
    ;;
  off) rm -f "$f" ;;
  *) echo "usage: $0 on CPUS | off" >&2; exit 2 ;;
esac
update-grub
echo "reboot required; current: $(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo none)"
