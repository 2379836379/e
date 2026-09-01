#!/bin/bash
# 测试辅助脚本。
# 用法：
#   helper.sh gen   <N> <nints>
#   helper.sh sum   <N>
#   helper.sh check <mode> <N>
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TEST_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
DATA_DIR="$TEST_ROOT/data/current"
OUT_DIR="${TEST_OUT_DIR:-$TEST_ROOT/out}"

cmd="${1:-}"
shift || true

case "$cmd" in
  gen)
    N=${1:?missing N}
    nints=${2:?missing nints}
    mkdir -p "$DATA_DIR"
    for ((r=0; r<N; r++)); do
      seq 0 $((nints - 1)) | awk -v r="$r" '{printf "%d\n", ($1 * 131 + r * 1000003) % 1000000}' > "$DATA_DIR/input-$r.data"
    done
    echo "generated $N input files, $nints ints each"
    ;;

  sum)
    N=${1:?missing N}
    mkdir -p "$DATA_DIR"
    files=()
    for ((r=0; r<N; r++)); do
      files+=("$DATA_DIR/input-$r.data")
    done
    paste "${files[@]}" | awk '{s=0; for(i=1;i<=NF;i++) s+=$i; printf "%d\n", s}' > "$DATA_DIR/expected-allreduce.data"
    echo "summed $N inputs -> $DATA_DIR/expected-allreduce.data"
    ;;

  check)
    mode=${1:?missing mode}
    N=${2:?missing N}
    ok=1
    chk() {
      if cmp -s "$1" "$2" 2>/dev/null; then
        echo "$3: PASS"
      else
        echo "$3: FAIL"
        ok=0
      fi
    }
    case "$mode" in
      route|transport)
        chk "$OUT_DIR/output-$((N-1)).data" "$DATA_DIR/input-0.data" "rank $((N-1)) $mode (output-$((N-1)).data == input-0.data)"
        ;;
      shift)
        for ((j=0; j<N; j++)); do
          p=$(((j - 1 + N) % N))
          chk "$OUT_DIR/output-$j.data" "$DATA_DIR/input-$p.data" "rank $j shift (output-$j.data == input-$p.data)"
        done
        ;;
      allreduce)
        for ((j=0; j<N; j++)); do
          chk "$OUT_DIR/output-$j.data" "$DATA_DIR/expected-allreduce.data" "rank $j allreduce (output-$j.data == expected-allreduce.data)"
        done
        ;;
      *)
        echo "unknown mode: $mode"
        exit 2
        ;;
    esac
    [ $ok -eq 1 ] && echo "==> RESULT: PASS" || echo "==> RESULT: FAIL"
    [ $ok -eq 1 ]
    ;;

  *)
    echo "usage: $0 {gen N nints | sum N | check mode N}"
    exit 1
    ;;
esac
