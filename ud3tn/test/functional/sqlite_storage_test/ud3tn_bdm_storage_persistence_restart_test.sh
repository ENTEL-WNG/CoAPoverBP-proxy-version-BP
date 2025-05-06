#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
set -o errexit

TEST_MSG="μD3TN SQLite restart-persistence test"

# Run μD3TN instances
# The environment variable $DB_FILE is set here.
source $(dirname $0)/run_ud3tn.sh persistent

exit_handler_2() {
    cd "$UD3TN_DIR"

    kill -TERM $DISPATCHER_PID || true
    sleep 0.1
    kill -TERM $UD3TN12_PID || true
    kill -TERM $UD3TN2_PID || true

    sleep 0.2
    echo
    echo ">>> UD3TN1 LOGFILE"
    cat "$TEST_DIR/ud3tn1.log" || true
    echo
    echo ">>> UD3TN2 LOGFILE"
    cat "$TEST_DIR/ud3tn2.log" || true
    echo
    echo ">>> UD3TN1 (re-launched) LOGFILE"
    cat "$TEST_DIR/ud3tn1_2.log" || true
    echo
    echo ">>> BDM1 LOGFILE"
    cat "$TEST_DIR/bdm1.log" || true
    echo
    echo ">>> BDM2 LOGFILE"
    cat "$TEST_DIR/bdm2.log" || true

    echo
    echo "Waiting for uD3TN to exit gracefully - if it doesn't, check for sanitizer warnings."
    wait $UD3TN12_PID
    wait $UD3TN2_PID
}

aap2-bdm-ud3tn-routing -vv > "$TEST_DIR/bdm1.log" 2>&1 &
DISPATCHER_PID=$!
sleep 1 # give the dispatcher some time to start

trap exit_handler_2 EXIT

aap2-config --schedule 30 100 100000 dtn://ud3tn2.dtn/ mtcp:127.0.0.1:4225
aap2-send dtn://ud3tn2.dtn/1 "$TEST_MSG"
sleep 1 # give the BDM some time to process the bundle

kill -TERM $DISPATCHER_PID
wait $DISPATCHER_PID || true
kill -TERM $UD3TN1_PID
echo "Waiting for uD3TN to exit gracefully - if it doesn't, check for sanitizer warnings."
wait $UD3TN1_PID

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 "$UD3TN_DIR/build/posix/ud3tn" -L 4 --external-dispatch -e "dtn://ud3tn1.dtn/" $CLA_CONFIG > "$TEST_DIR/ud3tn1_2.log" 2>&1 &
UD3TN12_PID=$!
sleep 1 # give uD3TN some time to start

aap2-bdm-ud3tn-routing -vv > "$TEST_DIR/bdm2.log" 2>&1 &
DISPATCHER_PID=$!
sleep 1 # give the dispatcher some time to start

aap2-config --schedule 1 100 100000 dtn://ud3tn2.dtn/ mtcp:127.0.0.1:4225
sleep 0.1 # ensure the config reaches the BDM
aap2-storage-agent --storage-agent-eid "dtn://ud3tn1.dtn/sqlite" push --dest-eid-glob "*"

timeout -v 3 aap2-receive --socket "$UD3TN_DIR/ud3tn2.aap2.socket" -a 1 -c 1 -vv --verify-pl "$TEST_MSG"
