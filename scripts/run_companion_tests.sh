#!/usr/bin/env bash
# Single entry point for this repo's test loop: host unit tests always, the
# on-hardware BLE harness on request. See CLAUDE.md "The phone is not a test
# harness" -- scripts/companion_e2e_test.py is what --ble drives, not a phone.
#
# Usage:
#   scripts/run_companion_tests.sh                                    # unit tests only
#   scripts/run_companion_tests.sh --ble --port /dev/cu.usbmodem21201 # + BLE harness
#   scripts/run_companion_tests.sh --ble --port ... --only spokenfeeds --articles 5
#   scripts/run_companion_tests.sh --flash --ble --port ...           # flash, then BLE
#
# Everything after --ble is forwarded verbatim to companion_e2e_test.py, so
# any of its own flags (--only, --soak, --articles, --render-timeout, ...)
# work here unchanged. --flash is only meaningful ahead of --ble; flashing
# after the harness ran would just erase what it just exercised.
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PASS=0
FAIL=0
note_pass() { echo "PASS  $1"; PASS=$((PASS + 1)); }
note_fail() { echo "FAIL  $1"; FAIL=$((FAIL + 1)); }

# --- (a) host unit tests: test/README ------------------------------------- #
echo "== host unit tests (test/README) =="
if cmake -S test -B build/test && cmake --build build/test \
    && ctest --test-dir build/test --output-on-failure; then
  note_pass "host unit tests"
else
  note_fail "host unit tests"
fi

# --- parse this script's own flags; --ble hands off everything after it --- #
DO_FLASH=0
DO_BLE=0
BLE_ARGS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --flash)
      DO_FLASH=1
      shift
      ;;
    --ble)
      DO_BLE=1
      shift
      BLE_ARGS=("$@")
      break
      ;;
    -h | --help)
      sed -n '2,15p' "$0"
      exit 0
      ;;
    *)
      echo "run_companion_tests.sh: unrecognized argument '$1' (put harness flags after --ble)" >&2
      exit 2
      ;;
  esac
done

# --- (c) --flash: refuse if something already holds the serial port ------- #
if [ "$DO_FLASH" = "1" ]; then
  echo "== flashing (pio run -e default -t upload) =="
  shopt -s nullglob
  ports=(/dev/cu.usbmodem*)
  shopt -u nullglob
  held=0
  for p in "${ports[@]}"; do
    holder="$(lsof -t "$p" 2>/dev/null || true)"
    if [ -n "$holder" ]; then
      echo "run_companion_tests.sh: $p is held by pid(s) $holder" \
        "-- close whatever has it open (a serial monitor, a stuck harness run) and retry" >&2
      held=1
    fi
  done
  if [ "$held" = "1" ]; then
    note_fail "flash (serial port held)"
  elif pio run -e default -t upload; then
    note_pass "flash"
  else
    note_fail "flash"
  fi
fi

# --- (b) --ble: refuse under tmux, then run the harness -------------------- #
if [ "$DO_BLE" = "1" ]; then
  if [ -n "${TMUX:-}" ]; then
    cat >&2 <<'EOF'
run_companion_tests.sh: refusing to run --ble under tmux.

Host BLE via bleak fails with DENIED_BY_UNKNOWN when run inside tmux -- a
macOS TCC (Bluetooth permission) quirk, not a code bug. Run this from a plain
terminal, or drive it as a launched process outside the multiplexer; serial
over USB alone is unaffected. See CLAUDE.md, "The phone is not a test harness."
EOF
    note_fail "BLE harness (refused: running under tmux)"
  else
    echo "== companion_e2e_test.py ${BLE_ARGS[*]:-} =="
    if python3 scripts/companion_e2e_test.py "${BLE_ARGS[@]}"; then
      note_pass "BLE harness"
    else
      note_fail "BLE harness"
    fi
  fi
fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
