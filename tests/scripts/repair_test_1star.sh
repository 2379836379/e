#!/bin/bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/tests/out"; DATA="$ROOT/tests/data/current"; BIN="$ROOT/build/inc_1star"
HOSTS=(host1 host2 host3 host4); ROUTERS=(router1); CFG=topology/star1/ranks.cfg; N=4; NINTS=4096
cleanup(){ set +e; for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do docker exec "$c" pkill -f /app/build/inc_1star >/dev/null 2>&1 || true; done; bash "$ROOT/topology/star1/setup.sh" clean >/dev/null 2>&1 || true; }
trap cleanup EXIT
cd "$ROOT"; rm -rf "$OUT"; mkdir -p "$OUT" "$DATA"
rm -f "$DATA"/input-*.data "$DATA"/expected-allreduce.data "$OUT"/output-*.data "$OUT"/*.log
SRC=(protocol/main.c config/app_config.c protocol/allreduce_workload.c wire/arbor_wire.c runtime/runtime_common.c protocol/host.c protocol/requester.c protocol/responder.c protocol/router.c config/arbor_fabric.c)
gcc -Wall -Wextra -O2 -DSUBCHANNEL_COUNT=1 -DARBOR_TEST_DROP_NORMAL_REQUEST -I. -Iconfig -Iprotocol -Iwire -Iruntime -o "$BIN" "${SRC[@]}" -lpcap -lpthread
bash tests/scripts/helper.sh gen "$N" "$NINTS"; bash tests/scripts/helper.sh sum "$N"
bash topology/star1/setup.sh clean >/dev/null 2>&1 || true; bash topology/star1/setup.sh setup
for c in "${ROUTERS[@]}" "${HOSTS[@]}"; do docker exec "$c" mkdir -p /app/build /app/tests/out /app/tests/data/current; docker cp "$BIN" "$c:/app/build/inc_1star"; done
for r in 0 1 2 3; do docker cp "$DATA/input-$r.data" "host$((r+1)):/app/tests/data/current/input-$r.data"; done
docker exec -d router1 bash -lc "cd /app && ./build/inc_1star router1 $CFG allreduce > tests/out/router1.log 2>&1"
sleep 1
for r in 0 1 2 3; do h=${HOSTS[$r]}; docker exec -d "$h" bash -lc "cd /app && ./build/inc_1star $h $CFG allreduce > tests/out/$h.log 2>&1"; done
while :; do ready=1; for r in 0 1 2 3; do docker exec "${HOSTS[$r]}" test -f "/app/tests/out/output-$r.data" || { ready=0; break; }; done; [ "$ready" -eq 1 ] && break; sleep 1; done
for r in 0 1 2 3; do h=${HOSTS[$r]}; docker cp "$h:/app/tests/out/output-$r.data" "$OUT/output-$r.data"; docker cp "$h:/app/tests/out/$h.log" "$OUT/$h.log"; done
docker cp router1:/app/tests/out/router1.log "$OUT/router1.log"
bash tests/scripts/helper.sh check allreduce "$N"
