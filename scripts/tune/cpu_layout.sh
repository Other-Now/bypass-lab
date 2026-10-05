#!/usr/bin/env bash
# Pick the core roles from the real topology instead of assuming a numbering.
# Prints shell assignments:
#   HOT      the receive loop's CPU (first thread of the last physical core)
#   SIBLING  its hyperthread sibling -- kept idle, so the hot loop owns the core
#   HK       housekeeping CPUs (first physical core, both threads): IRQs, daemons
#   SOCKETS  physical packages (1 => no NUMA claims are possible)
set -euo pipefail
last_core_cpus=""
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  sib=$(cat "$c/topology/thread_siblings_list")
  last_core_cpus=$sib
done
HOT=${last_core_cpus%%[,-]*}
SIBLING=${last_core_cpus#*[,-]}
[[ $SIBLING == "$last_core_cpus" ]] && SIBLING=""
HK=$(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list)
SOCKETS=$(cat /sys/devices/system/cpu/cpu*/topology/physical_package_id | sort -u | wc -l)
echo "HOT=$HOT"
echo "SIBLING=$SIBLING"
echo "HK=$HK"
echo "SOCKETS=$SOCKETS"
