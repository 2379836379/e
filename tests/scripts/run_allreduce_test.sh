#!/bin/bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TEST_ROOT="$ROOT/tests"
OUT_DIR="$TEST_ROOT/out"
DATA_DIR="$TEST_ROOT/data/current"
SCRIPT_DIR="$TEST_ROOT/scripts"
CONFIG_DIR="$TEST_ROOT/config"
BIN="$ROOT/build/inc"
CFG_PATH="topology/tree/ranks.cfg"
HOSTS=(host1 host2 host3 host4)
ROUTERS=(router-root router-a router-a0 router-a1)
N=4
NINTS=4096

cleanup() {
  set +e
  for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
    docker exec "$c" pkill -f /app/build/inc >/dev/null 2>&1 || true
    docker exec "$c" pkill -f /app/inc >/dev/null 2>&1 || true
  done
  bash "$ROOT/topology/tree/setup.sh" clean >/dev/null 2>&1 || true
}
trap cleanup EXIT

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR" "$DATA_DIR"
cd "$ROOT"
rm -f "$DATA_DIR"/input-*.data "$DATA_DIR"/expected-allreduce.data
rm -f "$OUT_DIR"/output-*.data "$OUT_DIR"/*.log
make -C "$ROOT" >/dev/null
bash "$SCRIPT_DIR/helper.sh" gen "$N" "$NINTS"
bash "$SCRIPT_DIR/helper.sh" sum "$N"
bash "$ROOT/topology/tree/setup.sh" clean >/dev/null 2>&1 || true
bash "$ROOT/topology/tree/setup.sh" setup
for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do
  docker exec "$c" mkdir -p /app/build /app/tests/out /app/tests/data/current
  docker exec "$c" rm -f /app/tests/out/output-*.data /app/tests/out/*.log
  docker cp "$BIN" "$c:/app/build/inc"
done
for r in 0 1 2 3; do
  h=${HOSTS[$r]}
  docker cp "$DATA_DIR/input-$r.data" "$h:/app/tests/data/current/input-$r.data"
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
for _ in $(seq 1 120); do
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
done
if [ $finished -ne 1 ]; then
  echo 'allreduce test timed out waiting for output files' >&2
fi
missing=0
for r in 0 1 2 3; do
  h=${HOSTS[$r]}
  if docker cp "$h:/app/tests/out/output-$r.data" "$OUT_DIR/output-$r.data"; then
    :
  else
    echo "missing output-$r.data from $h" >&2
    missing=1
  fi
  docker cp "$h:/app/tests/out/$h.log" "$OUT_DIR/$h.log" 2>/dev/null || true
done
for rt in "${ROUTERS[@]}"; do
  docker cp "$rt:/app/tests/out/$rt.log" "$OUT_DIR/$rt.log" 2>/dev/null || true
done
if [ $finished -ne 1 ] || [ $missing -ne 0 ]; then
  echo 'test artifacts were collected under tests/out' >&2
  exit 1
fi
cd "$ROOT"
bash "$SCRIPT_DIR/helper.sh" check allreduce "$N"
