#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TEST_ROOT="$ROOT/tests"
OUT_DIR="$TEST_ROOT/out"
DATA_DIR="$TEST_ROOT/data/current"
SCRIPT_DIR="$TEST_ROOT/scripts"
CONFIG_DIR="$TEST_ROOT/config"
BIN="$ROOT/build/inc"
CFG_PATH="topology/star/ranks.cfg"
HOSTS=(host1 host2 host3 host4)
ROUTERS=(router1 router2)
N=4
NINTS=4096
LOSS_RATE="20%"

cleanup() {
  set +e
  for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
    docker exec "$c" pkill -f /app/build/inc >/dev/null 2>&1 || true
    docker exec "$c" pkill -f /app/inc >/dev/null 2>&1 || true
  done
  bash "$ROOT/topology/star/setup.sh" clean >/dev/null 2>&1 || true
}
trap cleanup EXIT

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR" "$DATA_DIR"
cd "$ROOT"
rm -f "$DATA_DIR"/input-*.data "$DATA_DIR"/expected-allreduce.data
rm -f "$OUT_DIR"/output-*.data "$OUT_DIR"/*.log
make -C "$ROOT" >/dev/null
if [ ! -x "$BIN" ] || [ ! -s "$BIN" ]; then
  echo "invalid executable after build: $BIN; rebuilding" >&2
  make -C "$ROOT" -B >/dev/null
fi
if [ ! -x "$BIN" ] || [ ! -s "$BIN" ]; then
  echo "invalid executable: $BIN" >&2
  exit 1
fi
bash "$SCRIPT_DIR/helper.sh" gen "$N" "$NINTS"
bash "$SCRIPT_DIR/helper.sh" sum "$N"
bash "$ROOT/topology/star/setup.sh" clean >/dev/null 2>&1 || true
bash "$ROOT/topology/star/setup.sh" setup
for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
  for dev in $(docker exec "$c" ip -o link show | awk -F": " '$2 != "lo" {print $2}' | cut -d@ -f1); do
    case "$dev" in eth0|lo) continue ;; esac
    docker exec "$c" tc qdisc replace dev "$dev" root netem loss "$LOSS_RATE"
  done
done
echo "configured ${LOSS_RATE} packet loss on topology interfaces"
# setup.sh bind-mounts the repository at /app in every container.  Do not use
# docker cp for paths below: copying to a bind-mounted destination can replace
# the host-side source (including build/inc) and leave a zero-byte executable.
for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
  docker exec "$c" mkdir -p /app/build /app/tests/out /app/tests/data/current
  docker exec "$c" rm -f /app/tests/out/output-*.data /app/tests/out/*.log
done
for rt in "${ROUTERS[@]}"; do
  docker exec -d "$rt" bash -lc "cd /app && ./build/inc $rt $CFG_PATH allreduce > tests/out/$rt.log 2>&1"
done
sleep 1
for r in 0 1 2 3; do
  h=${HOSTS[$r]}
  docker exec -d "$h" bash -lc "cd /app && ./build/inc $h $CFG_PATH allreduce > tests/out/$h.log 2>&1"
done
finished=0
waited=0
WAIT_TIMEOUT_SEC=600
while :; do
  ready=1
  for r in 0 1 2 3; do
    h=${HOSTS[$r]}
    if ! docker exec "$h" test -f "/app/tests/out/output-$r.data"; then
      ready=0
      break
    fi
  done
  if [ $ready -eq 1 ]; then
    finished=1
    break
  fi
  sleep 1
  waited=$((waited + 1))
  if [ $waited -ge $WAIT_TIMEOUT_SEC ]; then
    echo "timed out waiting for output files" >&2
    break
  fi
done
if [ $finished -ne 1 ]; then
  echo 'allreduce test waiting for output files' >&2
fi
end_closed=1
for r in 0 1 2 3; do
  h=${HOSTS[$r]}
  if ! docker exec "$h" grep -q "\[host\] rank${r} allreduce done" "/app/tests/out/$h.log"; then
    echo "workers did not finish on $h" >&2
    end_closed=0
  fi
  summary=$(awk -v channel="ch=$r" 'index($0,"[responder-summary]") && index($0,channel) { found=1; for (i=1;i<=NF;i++) { if ($i ~ /^request_commit=/) { split($i,a,"="); req+=a[2] } if ($i ~ /^repair_commit=/) { split($i,a,"="); rep+=a[2] } } } END { if (found) print req+rep; else print "NA" }' "$OUT_DIR/$h.log")
  if [ "$summary" = "NA" ]; then
    echo "responder ch=$r produced no summary (likely timeout or crash)" >&2
    end_closed=0
  elif [ "$summary" -ne 4 ]; then
    echo "responder ch=$r committed ${summary}/4 credits" >&2
    end_closed=0
  fi
done
missing=0
for r in 0 1 2 3; do
  if [ ! -f "$OUT_DIR/output-$r.data" ]; then
    echo "missing output-$r.data from ${HOSTS[$r]}" >&2
    missing=1
  fi
done
# The containers bind-mount OUT_DIR, so logs and outputs are already present
# on the host.  A docker cp back to the same path is both unnecessary and can
# truncate the source file.
if [ $finished -ne 1 ] || [ $end_closed -ne 1 ] || [ $missing -ne 0 ]; then
  echo 'test artifacts were collected under tests/out' >&2
  exit 1
fi
cd "$ROOT"
bash "$SCRIPT_DIR/helper.sh" check allreduce "$N"
