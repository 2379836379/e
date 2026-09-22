#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TEST_ROOT="$ROOT/tests"
OUT_DIR="$TEST_ROOT/out"
DATA_DIR="$TEST_ROOT/data/current"
SCRIPT_DIR="$TEST_ROOT/scripts"
CFG_PATH="topology/tree3/ranks.cfg"
HOSTS=(host1 host2 host3 host4 host5 host6 host7 host8)
ROUTERS=(router-root router-l router-r router-ll router-lr router-rl router-rr)
N=8
NINTS=32768
LOSS_RATE="10%"
# The workload uses 8192-byte packets, i.e. 2048 int32 values per packet.
PACKET_INTS=2048
TOTAL_PACKETS=$(( (NINTS + PACKET_INTS - 1) / PACKET_INTS ))
cleanup_in_progress=0

cleanup() {
  if [ "$cleanup_in_progress" -ne 0 ]; then
    return
  fi
  cleanup_in_progress=1
  # Do not let a second Ctrl-C interrupt container removal midway through.
  trap '' INT TERM
  set +e
  for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
    docker exec "$c" pkill -f /app/build/inc >/dev/null 2>&1 || true
    docker exec "$c" pkill -f /app/inc >/dev/null 2>&1 || true
  done
  bash "$ROOT/topology/tree3/setup.sh" clean >/dev/null 2>&1 || true
}

abort() {
  local status=$1
  cleanup
  trap - EXIT
  exit "$status"
}

trap cleanup EXIT
trap 'abort 130' INT
trap 'abort 143' TERM

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR" "$DATA_DIR"
cd "$ROOT"
rm -f "$DATA_DIR"/input-*.data "$DATA_DIR"/expected-allreduce.data
rm -f "$OUT_DIR"/output-*.data "$OUT_DIR"/*.log
make -C "$ROOT" >/dev/null
bash "$SCRIPT_DIR/helper.sh" gen "$N" "$NINTS"
bash "$SCRIPT_DIR/helper.sh" sum "$N"
bash "$ROOT/topology/tree3/setup.sh" clean >/dev/null 2>&1 || true
bash "$ROOT/topology/tree3/setup.sh" setup

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
for rt in "${ROUTERS[@]}"; do
  docker exec -d "$rt" bash -lc "cd /app && ./build/inc $rt $CFG_PATH allreduce > tests/out/$rt.log 2>&1"
done
sleep 1
for r in $(seq 0 $((N - 1))); do
  h=${HOSTS[$r]}
  docker exec -d "$h" bash -lc "cd /app && ./build/inc $h $CFG_PATH allreduce > tests/out/$h.log 2>&1"
done

finished=0
for _ in $(seq 1 600); do
  ready=1
  for r in $(seq 0 $((N - 1))); do
    h=${HOSTS[$r]}
    if ! docker exec "$h" test -f "/app/tests/out/output-$r.data"; then
      ready=0
      break
    fi
  done
  if [ "$ready" -eq 1 ]; then
    finished=1
    break
  fi
  sleep 1
done
if [ "$finished" -ne 1 ]; then
  echo 'allreduce tree3 loss test timed out waiting for output files' >&2
fi

end_closed=1
for r in $(seq 0 $((N - 1))); do
  h=${HOSTS[$r]}
  if ! grep -q "\[host\] rank${r} allreduce done" "$OUT_DIR/$h.log"; then
    echo "worker rank${r} did not finish on $h" >&2
    end_closed=0
  fi
  summary=$(awk -v channel="ch=$r" 'index($0,"[responder-summary]") && index($0,channel) { found=1; for (i=1;i<=NF;i++) { if ($i ~ /^request_commit=/) { split($i,a,"="); req+=a[2] } if ($i ~ /^repair_commit=/) { split($i,a,"="); rep+=a[2] } } } END { if (found) print req+rep; else print "NA" }' "$OUT_DIR/$h.log")
  expected_commits=$((TOTAL_PACKETS / N))
  if [ "$r" -lt "$((TOTAL_PACKETS % N))" ]; then
    expected_commits=$((expected_commits + 1))
  fi
  if [ "$summary" = "NA" ]; then
    echo "responder ch=$r produced no summary (likely timeout or crash)" >&2
    end_closed=0
  elif [ "$summary" -ne "$expected_commits" ]; then
    echo "responder ch=$r committed ${summary}/${expected_commits} credits" >&2
    end_closed=0
  fi
done

missing=0
for r in $(seq 0 $((N - 1))); do
  if [ ! -f "$OUT_DIR/output-$r.data" ]; then
    echo "missing output-$r.data from ${HOSTS[$r]}" >&2
    missing=1
  fi
done
if [ "$finished" -ne 1 ] || [ "$end_closed" -ne 1 ] || [ "$missing" -ne 0 ]; then
  echo 'test artifacts were collected under tests/out' >&2
  exit 1
fi
bash "$SCRIPT_DIR/helper.sh" check allreduce "$N"
