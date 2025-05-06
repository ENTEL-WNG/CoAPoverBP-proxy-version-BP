#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
set -o errexit

# The environment variable $DB_FILE is set in ./run_ud3tn.sh based on the command line argument
# specified by the user. If it is not set, an in-memory DB is used, which means that an external
# check of the database status is not possible.

# This assumes you are running the command from within the "ud3tn" directory.
TEST_MSG="μD3TN SQLite storage + static dispatcher test"

SQL_SELECT="SELECT source, destination, creation_timestamp, sequence_number, fragment_offset, payload_length, hex(bundle) FROM bundles;"

# Run μD3TN instances
source $(dirname $0)/run_ud3tn.sh

# Start a static BDM forwarding bundles to the storage "node"
aap2-bdm-static "$(dirname $0)/static_rdict_1_storage.json" & DISP1_PID=$!

# Wait a moment until the BDM has started
sleep 0.5

# Send a bundle to the destination node
aap2-send dtn://ud3tn2.dtn/bundlesink "$TEST_MSG"

# Wait a moment until μD3TN has stored the bundle in the database
sleep 0.5

# Query DB to verify the bundle is stored
if [ -n "$DB_FILE" ]; then
    sqlite3 "$TEST_DIR/test.sqlite" "$SQL_SELECT"
    ROWS=$(sqlite3 "$TEST_DIR/test.sqlite" "SELECT COUNT(*) FROM bundles;")

    if [ $ROWS != 1 ]; then
        echo "Bundle is not stored in the database"
        exit -1
    fi
fi

# Configure the link to uD3TN 2
aap2-configure-link dtn://ud3tn2.dtn/ mtcp:localhost:4225

# Terminate the dispatcher which forwards to the DB
kill -TERM $DISP1_PID

# Wait until it has terminated and uD3TN 1 has connected to uD3TN 2
sleep 0.5

# Start a static BDM forwarding bundles to the (now-connected) destination node
aap2-bdm-static "$(dirname $0)/static_rdict_2_dest.json" & DISP2_PID=$!

# Send a PUSH command asynchronously to the SQLiteAgent to inject the bundles
# to μD3TN after the receiver and BDM are active
( sleep 1 &&
aap2-storage-agent \
    --storage-agent-eid "dtn://ud3tn1.dtn/sqlite" \
    push \
    --dest-eid-glob "dtn://ud3tn2.dtn*"
) &

# Add a receiver to and verify the incomming bundle payload
timeout -v 3 aap2-receive --socket "$UD3TN_DIR/ud3tn2.aap2.socket" --agentid bundlesink -c 1 --verify-pl "$TEST_MSG" -vv

# Terminate the second BDM gracefully
kill -TERM $DISP2_PID

# Query DB to verify the bundle is still stored
if [ -n "$DB_FILE" ]; then
    sqlite3 "$TEST_DIR/test.sqlite" "$SQL_SELECT"
    ROWS=$(sqlite3 "$TEST_DIR/test.sqlite" "SELECT COUNT(*) FROM bundles;")

    if [ $ROWS != 1 ]; then
        echo "Bundle is not stored in the database"
        exit -1
    fi
fi

# Send a DELETE command to the SQLiteAgent to delete the bundle from the DB
aap2-storage-agent \
    --storage-agent-eid "dtn://ud3tn1.dtn/sqlite" \
    delete \
    --dest-eid-glob "dtn://ud3tn2.dtn*"

# Wait a moment until μD3TN has deleted the bundle from the database
sleep 1

# Query DB to verify the bundle is deleted
if [ -n "$DB_FILE" ]; then
    sqlite3 "$TEST_DIR/test.sqlite" "$SQL_SELECT"

    ROWS=$(sqlite3 "$TEST_DIR/test.sqlite" "SELECT COUNT(*) FROM bundles;")

    if [ $ROWS != 0 ]; then
        echo "Bundle is not deleted from the database"
        exit -1
    fi
fi
