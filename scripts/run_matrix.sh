#!/usr/bin/env bash
# Run one tuning configuration: every receive path in $PATHS at every rate in
# $RATES, one (path, rate) pair at a time, receiver started fresh for each.
#
# The same script drives three environments; only how the receiver is reached
# changes:
#   RX_MODE=local  receiver on this host            (WSL loopback smoke runs)
#   RX_MODE=netns  receiver in network namespace    (CI: veth pair)
#   RX_MODE=ssh    receiver on another host         (AWS: second instance)
#
# Output: $OUT_DIR/$LABEL/{meta.json, <path>_<rate>.{tx.json,rx.json,samples.bin}}
#
# MODE=latency  echo every packet, fixed rates, latency samples
# MODE=maxrate  no echoes, rising rates, loss from the receiver's counters
set -euo pipefail

: "${BIN_DIR:=build}"
: "${RX_BIN_DIR:=$BIN_DIR}"
: "${FEED:=data/sample.NASDAQ_ITCH50}"
: "${OUT_DIR:=results/local}"
: "${LABEL:=baseline}"
: "${MODE:=latency}"
: "${DST_IP:=127.0.0.1}"
: "${RX_MODE:=local}"
: "${RX_HOST:=}"
: "${RX_NS:=bl-rx}"
: "${RX_SUDO:=}"
: "${TX_SUDO:=}"
: "${PATHS:=recvmsg recvmmsg}"
: "${RATES:=10000 100000}"
: "${SECONDS_PER_RUN:=10}"
: "${MSGS_PER_PKT:=1}"
: "${TX_THREADS:=1}"
: "${RX_CORE:=}"
: "${TX_CORE:=}"
: "${ECHO_CORE:=}"
: "${RX_IFACE:=}"
: "${XDP_MODE:=native}"
: "${XDP_EXTRA:=}"
: "${DPDK_EAL:=}"
: "${RX_EXTRA:=}"
: "${RX_PREPARE:=}"   # command run on the receiver before each path, given the path name

dest="$OUT_DIR/$LABEL"
mkdir -p "$dest"
rx_tmp=/tmp/bl_rx_run

# Run a shell snippet on the receiver.
rx() {
  case "$RX_MODE" in
    local) bash -c "$1" ;;
    netns) sudo ip netns exec "$RX_NS" bash -c "$1" ;;
    ssh)   ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$RX_HOST" "$1" ;;
    *) echo "bad RX_MODE $RX_MODE" >&2; exit 2 ;;
  esac
}

# Host facts as JSON (python3 does the escaping; /proc/cmdline can hold anything).
ENV_PY='import json,os,platform,subprocess
def rd(p,d=""):
    try: return open(p).read().strip()
    except OSError: return d
def sh(c):
    try: return subprocess.run(c,shell=True,capture_output=True,text=True).stdout.strip()
    except Exception: return ""
cpu=next((l.split(":",1)[1].strip() for l in rd("/proc/cpuinfo").splitlines() if l.startswith("model name")),"?")
nodes=[d for d in os.listdir("/sys/devices/system/node") if d.startswith("node")] if os.path.isdir("/sys/devices/system/node") else ["node0"]
print(json.dumps({"host":platform.node(),"kernel":platform.release(),"cpu":cpu,"nproc":os.cpu_count(),
 "cmdline":rd("/proc/cmdline"),"irqbalance":sh("systemctl is-active irqbalance") or "n/a",
 "isolated":rd("/sys/devices/system/cpu/isolated"),
 "hugepages_2M":int(rd("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages","0") or 0),
 "numa_nodes":len(nodes),"product":rd("/sys/devices/virtual/dmi/id/product_name")}))'
env_json() {
  if [[ $1 == rx ]]; then rx "python3 -c $(printf %q "$ENV_PY")"; else python3 -c "$ENV_PY"; fi
}

cat >"$dest/meta.json" <<EOF
{"label":"$LABEL","mode":"$MODE","rx_mode":"$RX_MODE","paths":"$PATHS","rates":"$RATES",
 "seconds":$SECONDS_PER_RUN,"msgs_per_pkt":$MSGS_PER_PKT,"rx_core":"$RX_CORE","tx_core":"$TX_CORE",
 "echo_core":"$ECHO_CORE","xdp_mode":"$XDP_MODE","xdp_extra":"$XDP_EXTRA","dpdk_eal":"$DPDK_EAL",
 "rx_extra":"$RX_EXTRA","feed":"$FEED","date":"$(date -u +%FT%TZ)",
 "sender":$(env_json tx),"receiver":$(env_json rx)}
EOF

rx_cmd() {
  local path=$1
  local c="$RX_SUDO $RX_BIN_DIR/bl_rx --path $path --out $rx_tmp.json --idle-exit-ms 2000 $RX_EXTRA"
  [[ -n $RX_CORE && $path != dpdk ]] && c+=" --core $RX_CORE"
  case $path in
    afxdp) c+=" --iface $RX_IFACE --xdp-mode $XDP_MODE $XDP_EXTRA" ;;
    dpdk)  c+=" -- $DPDK_EAL" ;;
  esac
  echo "$c"
}

for path in $PATHS; do
  [[ -n $RX_PREPARE ]] && rx "$RX_PREPARE $path"
  for rate in $RATES; do
    tag="${path}_${rate}"
    echo "== [$LABEL] $tag"
    rx "$RX_SUDO pkill -INT -x bl_rx 2>/dev/null; $RX_SUDO rm -f $rx_tmp.json $rx_tmp.log $rx_tmp.out; true"
    rx "nohup $(rx_cmd "$path") >$rx_tmp.out 2>$rx_tmp.log </dev/null &"

    ready=0
    for _ in $(seq 1 150); do
      if rx "grep -q READY $rx_tmp.log 2>/dev/null"; then ready=1; break; fi
      sleep 0.2
    done
    if [[ $ready != 1 ]]; then
      echo "receiver for $path never became ready:" >&2
      rx "cat $rx_tmp.log" >&2 || true
      continue
    fi
    sleep 0.3

    snd=(--dst "$DST_IP" --feed "$FEED" --rate "$rate" --seconds "$SECONDS_PER_RUN"
         --msgs-per-pkt "$MSGS_PER_PKT" --out "$dest/$tag" --label "$LABEL/$tag")
    [[ -n $TX_CORE ]] && snd+=(--tx-core "$TX_CORE")
    [[ -n $ECHO_CORE ]] && snd+=(--echo-core "$ECHO_CORE")
    if [[ $MODE == maxrate ]]; then
      snd+=(--echo-every 0 --tx-threads "$TX_THREADS")
    fi
    $TX_SUDO "$BIN_DIR/bl_sender" "${snd[@]}" >/dev/null

    for _ in $(seq 1 100); do
      rx "test -s $rx_tmp.json" && break
      sleep 0.1
    done
    if ! rx "test -s $rx_tmp.json"; then
      rx "$RX_SUDO pkill -INT -x bl_rx 2>/dev/null; true"
      sleep 1
    fi
    rx "cat $rx_tmp.json" >"$dest/$tag.rx.json" || echo '{}' >"$dest/$tag.rx.json"
    rx "cat $rx_tmp.log" >"$dest/$tag.rx.log" 2>/dev/null || true
    [[ -n $TX_SUDO ]] && $TX_SUDO chown "$(id -u):$(id -g)" "$dest/$tag".* 2>/dev/null || true
    python3 - "$dest/$tag" <<'PY' || true
import json, sys
p = sys.argv[1]
tx = json.load(open(p + ".tx.json"))
try: rx = json.load(open(p + ".rx.json"))
except Exception: rx = {}
ok = rx.get("checksum") == tx.get("expected_checksum")
print(f"   sent {tx['sent']} @ {tx['achieved_pps']:.0f} pps | rx {rx.get('packets','?')} "
      f"lost {rx.get('lost','?')} | echoes {tx['echoes_received']}/{tx['echoes_expected']} | "
      f"checksum {'OK' if ok else 'MISMATCH'}")
PY
  done
done
