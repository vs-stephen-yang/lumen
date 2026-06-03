#!/usr/bin/env bash
# Stop-hook: incrementally build the project and run every test executable,
# then surface a one-line pass/fail summary. Non-blocking (always exit 0) so a
# failing/hardware-dependent test never traps the session — read the summary
# and the log if something regressed.
#
# Wired from .claude/settings.json (Stop hook). Logs go to build/last_test_run.log.
set -u

# Resolve repo root from this script's location (.claude/hooks/ -> repo root).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)" || exit 0
cd "$ROOT" || exit 0

BUILD_DIR="build"
CONFIG="Debug"
LOG="$BUILD_DIR/last_test_run.log"

# Nothing to do if the project was never configured.
if [ ! -d "$BUILD_DIR" ]; then
  echo '{"systemMessage": "tests: no build/ dir — run cmake -S . -B build first"}'
  exit 0
fi

: > "$LOG"

# Incremental build (a no-op when nothing changed). Bail with a clear message
# if it fails so the summary is not misleading.
if ! cmake --build "$BUILD_DIR" --config "$CONFIG" >>"$LOG" 2>&1; then
  echo "{\"systemMessage\": \"⚠ build failed — tests skipped (see $LOG)\"}"
  exit 0
fi

TESTDIR="$BUILD_DIR/tests/$CONFIG"
if [ ! -d "$TESTDIR" ]; then
  echo '{"systemMessage": "tests: no test executables built yet"}'
  exit 0
fi

have_timeout=0
command -v timeout >/dev/null 2>&1 && have_timeout=1

pass=0
fail=0
failed_names=""
for exe in "$TESTDIR"/*.exe; do
  [ -e "$exe" ] || continue
  name="$(basename "$exe" .exe)"
  echo "=== $name ===" >>"$LOG"
  # Run from the test dir; some tests resolve paths relative to CWD.
  if [ "$have_timeout" -eq 1 ]; then
    ( cd "$TESTDIR" && timeout 180 "./$name.exe" ) >>"$LOG" 2>&1
  else
    ( cd "$TESTDIR" && "./$name.exe" ) >>"$LOG" 2>&1
  fi
  rc=$?
  # 77 is the conventional "skipped" exit (missing GPU/display/Chrome).
  if [ "$rc" -eq 0 ] || [ "$rc" -eq 77 ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed_names="$failed_names $name"
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "{\"systemMessage\": \"✓ tests: $pass passed, 0 failed\"}"
else
  echo "{\"systemMessage\": \"✗ tests: $pass passed, $fail FAILED:${failed_names} (see $LOG)\"}"
fi
exit 0
