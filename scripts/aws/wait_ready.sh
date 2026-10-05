#!/usr/bin/env bash
# Wait until both nodes have finished the Terraform bootstrap (build included).
set -euo pipefail
for host in "$@"; do
  echo -n "waiting for $host "
  for _ in $(seq 1 120); do
    if ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 -o BatchMode=yes \
         "ubuntu@$host" test -f /var/lib/bypass-lab-ready 2>/dev/null; then
      echo " ready"
      continue 2
    fi
    echo -n "."
    sleep 10
  done
  echo " timed out; see /var/log/bypass-lab-bootstrap.log on $host" >&2
  exit 1
done
