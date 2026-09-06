#!/bin/bash
set -u -o pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)

mapfile -t TEST_SCRIPTS < <(find "$SCRIPT_DIR" -maxdepth 1 -type f -name "*.sh" ! -name "test.sh" ! -name "helper.sh" -printf "%f\n" | sort)

if [ "${#TEST_SCRIPTS[@]}" -eq 0 ]; then
  echo "no test scripts found in $SCRIPT_DIR" >&2
  exit 1
fi

failed=0
failed_scripts=()
for name in "${TEST_SCRIPTS[@]}"; do
  script="$SCRIPT_DIR/$name"
  echo "===== RUN $name ====="
  if bash "$script"; then
    echo "===== PASS $name ====="
  else
    status=$?
    echo "===== FAIL $name (exit $status) =====" >&2
    failed=$((failed + 1))
    failed_scripts+=("$name")
  fi
done

if [ "$failed" -ne 0 ]; then
  echo "$failed test script(s) failed" >&2
  printf "failed scripts:" >&2
  printf " %s" "${failed_scripts[@]}" >&2
  printf "\\n" >&2
  exit 1
fi

echo "all test scripts passed"
