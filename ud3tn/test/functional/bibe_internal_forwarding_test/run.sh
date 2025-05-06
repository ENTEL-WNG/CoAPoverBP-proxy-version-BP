#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

#--------------------------------------------------------------------------------------#
# This automated test checks whether µD3TN's BIBE functionality is working as expected #
# by starting 4 µD3TN instances resembling 2 BIBE nodes and forwarding a BIBE bundle   #
# using BIBE node 1 (lower1.dtn and upper1.dtn) to a bundlesink running on BIBE node 2 #
# (lower2.dtn and upper2.dtn).                                                         #
#--------------------------------------------------------------------------------------#

set -o errexit

# This assumes you are running the command from within the "ud3tn" directory.
UD3TN_DIR="$(pwd)"

exit_handler() {
    cd "$UD3TN_DIR"

    kill -TERM $UD3TN1_PID || true
    kill -TERM $UD3TN2_PID || true
    kill -TERM $UD3TN3_PID || true
    kill -TERM $UD3TN4_PID || true

    sleep 1
    echo ">>> LOWER1 LOGFILE"
    cat "/tmp/lower1.log" || true
    echo
    echo ">>> LOWER2 LOGFILE"
    cat "/tmp/lower2.log" || true
    echo
    echo ">>> UPPER1 LOGFILE"
    cat "/tmp/upper1.log" || true
    echo
    echo ">>> UPPER2 LOGFILE"
    cat "/tmp/upper2.log" || true
    echo

    echo "Waiting for uD3TN to exit gracefully - if it doesn't, check for sanitizer warnings."
    wait $UD3TN1_PID
    wait $UD3TN2_PID
    wait $UD3TN3_PID
    wait $UD3TN4_PID
}

rm -f /tmp/*.log

# Start first uD3TN instance (lower1)
"$UD3TN_DIR/build/posix/ud3tn" -a localhost -p 4242 -S "$UD3TN_DIR/ud3tn1.aap2.socket" -e "dtn://lower1.dtn/" -c "sqlite:ud3tn1.sqlite;mtcp:127.0.0.1,4224" -L 4 --allow-remote-config > /tmp/lower1.log 2>&1 &
UD3TN1_PID=$!
sleep 1

# Start second uD3TN instance (upper1)
"$UD3TN_DIR/build/posix/ud3tn" -a localhost -p 4243 -S "$UD3TN_DIR/ud3tn2.aap2.socket" -e "dtn://upper1.dtn/" -c "sqlite:ud3tn2.sqlite;bibe:," -L 4 --allow-remote-config > /tmp/upper1.log 2>&1 &
UD3TN2_PID=$!
sleep 1

# Start third uD3TN instance (lower2)
"$UD3TN_DIR/build/posix/ud3tn" -a localhost -p 4244 -S "$UD3TN_DIR/ud3tn3.aap2.socket" -e "dtn://lower2.dtn/" -c "sqlite:ud3tn3.sqlite;mtcp:127.0.0.1,4225" -L 4 --allow-remote-config > /tmp/lower2.log 2>&1 &
UD3TN3_PID=$!
sleep 1

# Start fourth uD3TN instance (upper2)
"$UD3TN_DIR/build/posix/ud3tn" -a localhost -p 4245 -S "$UD3TN_DIR/ud3tn4.aap2.socket" -e "dtn://upper2.dtn/" -c "sqlite:ud3tn4.sqlite;bibe:," -L 4 --allow-remote-config > /tmp/upper2.log 2>&1 &
UD3TN4_PID=$!

trap exit_handler EXIT

# Configure contacts
sleep 3
aap-config --tcp localhost 4243 --dest_eid dtn://upper1.dtn/ --schedule 1 3600 100000 --reaches "dtn://upper2.dtn/" dtn://lower1.dtn/ bibe:localhost:4242#dtn://lower2.dtn/
sleep 2
aap-config --tcp localhost 4242 --dest_eid dtn://lower1.dtn/ --schedule 1 3600 100000 --reaches "dtn://upper2.dtn/" dtn://lower2.dtn/ mtcp:127.0.0.1:4225
sleep 2
aap-config --tcp localhost 4245 --dest_eid dtn://upper2.dtn/ --schedule 1 3600 100000 --reaches "dtn://upper2.dtn/" dtn://lower2.dtn/ bibe:localhost:4244#dtn://upper2.dtn/
sleep 3

# Send a BIBE bundle to upper2
PAYLOAD="THISISTHEBUNDLEPAYLOAD"
timeout -v 10 stdbuf -oL aap-receive --tcp localhost 4245 -a bundlesink --count 1 --verify-pl "$PAYLOAD" --newline -vv &
sleep 1
python "$UD3TN_DIR/tools/cla/bibe_over_mtcp_test.py" -l localhost -p 4224 --payload "$PAYLOAD" -i "dtn://upper2.dtn/bundlesink" -o "dtn://lower1.dtn/" &
sleep 1
