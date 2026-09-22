#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/tests/out"
DATA="$ROOT/tests/data/current"
BIN="$ROOT/build/inc_1star"
HOSTS=(host1 host2 host3 host4)
ROUTERS=(router1)
CFG=topology/star1/ranks.cfg
N=4
NINTS=32768
LOSS_RATE="20%"

cleanup() {
  set +e
  for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
    docker exec "$c" pkill -f /app/build/inc_1star >/dev/null 2>&1 || true
  done
  bash "$ROOT/topology/star1/setup.sh" clean >/dev/null 2>&1 || true
}
trap cleanup EXIT

cd "$ROOT"
rm -rf "$OUT"
mkdir -p "$OUT" "$DATA"
rm -f "$DATA"/input-*.data "$DATA"/expected-allreduce.data
rm -f "$OUT"/output-*.data "$OUT"/*.log

SRC=(
  protocol/main.c
  config/app_config.c
  protocol/allreduce_workload.c
  wire/arbor_wire.c
  runtime/runtime_common.c
  protocol/host.c
  protocol/requester.c
  protocol/responder.c
  protocol/router.c
  config/arbor_fabric.c
)
gcc -Wall -Wextra -O2 -DSUBCHANNEL_COUNT=1 \
  -I. -Iconfig -Iprotocol -Iwire -Iruntime \
  -o "$BIN" "${SRC[@]}" -lpcap -lpthread

bash tests/scripts/helper.sh gen "$N" "$NINTS"
bash tests/scripts/helper.sh sum "$N"
bash topology/star1/setup.sh clean >/dev/null 2>&1 || true
bash topology/star1/setup.sh setup

for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
  for dev in $(docker exec "$c" ip -o link show |
      awk -F": " '$2 != "lo" {print $2}' | cut -d@ -f1); do
    case "$dev" in
      eth0|lo) continue ;;
    esac
    docker exec "$c" tc qdisc replace dev "$dev" root netem loss "$LOSS_RATE"
  done
done
echo "configured ${LOSS_RATE} packet loss on topology interfaces"

for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
  docker exec "$c" mkdir -p /app/build /app/tests/out /app/tests/data/current
  docker exec "$c" rm -f /app/tests/out/output-*.data /app/tests/out/*.log
done

docker exec -d router1 bash -lc \
  "cd /app && ./build/inc_1star router1 $CFG allreduce > tests/out/router1.log 2>&1"
sleep 1
for r in 0 1 2 3; do
  h=${HOSTS[$r]}
  docker exec -d "$h" bash -lc \
    "cd /app && ./build/inc_1star $h $CFG allreduce > tests/out/$h.log 2>&1"
done

finished=0
for _ in $(seq 1 600); do
  ready=1
  for r in 0 1 2 3; do
    if ! docker exec "${HOSTS[$r]}" test -f "/app/tests/out/output-$r.data"; then
      ready=0
      break
    fi
  done
  if [ "$ready" -eq 1 ]; then finished=1; break; fi
  sleep 1
done
if [ "$finished" -ne 1 ]; then
  echo 'allreduce loss 1star test timed out waiting for output files' >&2
  exit 1
fi

end_closed=1
for r in 0 1 2 3; do
  h=${HOSTS[$r]}
  if ! grep -q "\[host\] rank${r} allreduce done" "$OUT/$h.log"; then
    echo "workers did not finish on $h" >&2
    end_closed=0
  fi
  summary=$(awk -v channel="ch=$r" 'index($0,"[responder-summary]") && index($0,channel) { found=1; for (i=1;i<=NF;i++) { if ($i ~ /^request_commit=/) { split($i,a,"="); req+=a[2] } if ($i ~ /^repair_commit=/) { split($i,a,"="); rep+=a[2] } } } END { if (found) print req+rep; else print "NA" }' "$OUT/$h.log")
  if [ "$summary" = "NA" ]; then
    echo "responder ch=$r produced no summary (likely timeout or crash)" >&2
    end_closed=0
  elif [ "$summary" -ne 4 ]; then
    echo "responder ch=$r committed ${summary}/4 credits" >&2
    end_closed=0
  fi
  if [ ! -f "$OUT/output-$r.data" ]; then
    echo "missing output-$r.data from $h" >&2
    end_closed=0
  fi
done
if [ "$end_closed" -ne 1 ]; then
  echo 'allreduce loss 1star protocol did not close cleanly; artifacts were collected under tests/out' >&2
  exit 1
fi
bash tests/scripts/helper.sh check allreduce "$N"
