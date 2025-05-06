#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
set -o errexit

export UD3TN_DIR="$(pwd)"
export TEST_DIR="$(mktemp -d)"

exit_handler() {
    cd "$UD3TN_DIR"

    kill -TERM $UD3TN1_PID || true
    kill -TERM $UD3TN2_PID || true

    sleep 0.2
    echo
    echo ">>> UD3TN1 LOGFILE"
    cat "$TEST_DIR/ud3tn1.log" || true
    echo
    echo ">>> UD3TN2 LOGFILE"
    cat "$TEST_DIR/ud3tn2.log" || true

    echo

    echo "Waiting for uD3TN to exit gracefully - if it doesn't, check for sanitizer warnings."
    wait $UD3TN1_PID
    wait $UD3TN2_PID
}

case "$1" in
    ""|"volatile")
        echo "Using a volatile in-memory DB"
        DB_FILE=""
        CLA_CONFIG="" # Use default CLA configuration
        ;;
    "persistent")
        echo "Using a persistent DB file"
        DB_FILE="$TEST_DIR/test.sqlite"
        CLA_CONFIG="-c sqlite:$DB_FILE;mtcp:127.0.0.1,4224"
        ;;
    *)
        echo "invalid commandline argument: 'volatile' or 'persistent'"
        exit 1
        ;;
esac

ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 "$UD3TN_DIR/build/posix/ud3tn" -L 4 --external-dispatch -e "dtn://ud3tn1.dtn/" $CLA_CONFIG > "$TEST_DIR/ud3tn1.log" 2>&1 &
UD3TN1_PID=$!
sleep 1

# Start second uD3TN instance (ud3tn2)
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 "$UD3TN_DIR/build/posix/ud3tn" -L 4 --external-dispatch -e "dtn://ud3tn2.dtn/" -c "mtcp:127.0.0.1,4225" -s "$UD3TN_DIR/ud3tn2.aap.socket" -S "$UD3TN_DIR/ud3tn2.aap2.socket" > "$TEST_DIR/ud3tn2.log" 2>&1 &
UD3TN2_PID=$!
sleep 1

trap exit_handler EXIT
