#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
set -o errexit

# The environment variable $DB_FILE is set in ./run_ud3tn.sh based on the command line argument
# specified by the user. If it is not set, an in-memory DB is used, which means that an external
# check of the database status is not possible.

# Run μD3TN instances
source $(dirname $0)/run_ud3tn.sh

aap2-bdm-ud3tn-routing -vv & DISPATCHER_PID=$!
sleep 1 # give the dispatcher some time to start

if [ -n "$DB_FILE" ]; then
    timeout -v 30 python $(dirname $0)/storage_agent_bundle_id_test.py --db-file $DB_FILE
else
    timeout -v 30 python $(dirname $0)/storage_agent_bundle_id_test.py
fi

kill -TERM $DISPATCHER_PID
