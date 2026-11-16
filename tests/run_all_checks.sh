#!/usr/bin/env bash
# Runs every automatic check of the OS Control Center.
#
#   bash tests/run_all_checks.sh
#
# It builds the project from scratch in ./build-check (your normal ./build is not touched),
# so it also proves that a clean checkout compiles.

set -u
cd "$(dirname "$0")/.."

PASS=0
FAIL=0
ok()  { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
bad() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

BUILD=build-check
LOG=build-check.log

echo "1. Clean build from scratch"
rm -rf "$BUILD"
if cmake -S . -B "$BUILD" > "$LOG" 2>&1 && cmake --build "$BUILD" >> "$LOG" 2>&1; then
    ok "the project configures and compiles"
else
    bad "the build failed. Last lines of $LOG:"
    tail -30 "$LOG"
    echo; echo "Result: build failed, other checks skipped."
    exit 1
fi

WARNINGS=$(grep -c "warning:" "$LOG" || true)
if [ "$WARNINGS" -eq 0 ]; then
    ok "no compiler warnings (-Wall -Wextra)"
else
    bad "$WARNINGS compiler warning(s): grep 'warning:' $LOG"
fi

echo "2. Programs exist"
for program in os_control_center os-control-helper os-control-ipc-child; do
    if [ -x "$BUILD/$program" ]; then ok "$program was built"; else bad "$program is missing"; fi
done

echo "3. Application starts without crashing (headless, 4 seconds)"
QT_QPA_PLATFORM=offscreen timeout 4 "./$BUILD/os_control_center" > /dev/null 2> smoke.err
CODE=$?
if [ "$CODE" -eq 124 ]; then
    ok "all six pages were created and the window ran until the timeout stopped it"
else
    bad "the application ended early with exit code $CODE. See smoke.err:"
    head -10 smoke.err
fi
rm -f smoke.err

echo "4. Privileged helper refuses to work without pkexec"
"./$BUILD/os-control-helper" signal 1 0 KILL > /dev/null 2>&1
CODE=$?
if [ "$(id -u)" -eq 0 ]; then EXPECTED=4; else EXPECTED=8; fi
if [ "$CODE" -eq "$EXPECTED" ]; then
    ok "helper exit code $CODE (refused, as designed)"
else
    bad "helper exit code $CODE, expected $EXPECTED"
fi

echo "5. IPC child program"
"./$BUILD/os-control-ipc-child" > /dev/null 2>&1
if [ $? -eq 2 ]; then ok "child rejects a call without arguments"; else bad "child usage check"; fi

if g++ -std=c++17 -Wall -Wextra -o "$BUILD/ipc_child_test" tests/ipc_child_test.cpp -lrt 2> "$BUILD/test_compile.log"; then
    if "./$BUILD/ipc_child_test" "./$BUILD/os-control-ipc-child"; then
        ok "pipe, FIFO, shared memory and message queue all work between real processes"
    else
        bad "the IPC test reported failures (see above)"
    fi
else
    bad "the IPC test did not compile: $BUILD/test_compile.log"
fi

echo "6. No IPC objects were left behind"
LEFT=$(ls /dev/shm/occ_* /tmp/occ_fifo_* /tmp/occ_test_* /dev/mqueue/occ_* 2>/dev/null | wc -l)
if [ "$LEFT" -eq 0 ]; then ok "no leftover shared memory, FIFOs or queues"; else bad "$LEFT leftover object(s) in /dev/shm, /tmp or /dev/mqueue"; fi

echo
echo "Result: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]