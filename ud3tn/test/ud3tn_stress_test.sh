#!/usr/bin/bash
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

# This script runs a simple bundle load test. Two uD3TN instances are set up
# in two different network namespaces that are coupled using a veth pair.
# After configuring the loop contacts, bundles are injected using a third
# uD3TN instance.

# Requirements:
# - Linux system supporting network namespaces and veth
# - iproute2
# - All requirements of uD3TN, a ud3tn binary, and a Python venv in the
#   provided directory (run `make && make virtualenv` beforehand)

# To enable debug logging, set this to 1 (release builds) or 2 (debug builds).
DEBUG=0
# Important for AAP2
AAP2_ADM_SECRET=verysecretsecret
export AAP2_ADM_SECRET

# To use TCPCL instead MTCP, replace "mtcp" with "tcpclv3" in this file.

set -e

if [[ -z "$1" || -z "$2" || -z "$3" || -z "$4" ]]; then
    echo "Usage: $0 <ud3tn-dir> <runas-user> <bundle-count> <bundle-size-bytes> [static|compat|bdm]" >&2
fi

if [[ -z "$5" ]]; then
    mode=static
else
    mode=$5
fi

if [[ "$EUID" -ne 0 ]]; then
    echo "Not running as root, this might fail." >&2
fi

set -u

WORK_DIR="$1"
RUNAS_USER=$2
BUNDLE_COUNT=$3
BUNDLE_SIZE=$4
SOCK_DIR="$(mktemp -d)"

echo "Sockets and logs will be stored here: $SOCK_DIR" >&2

chown $RUNAS_USER "$SOCK_DIR"

NS1=ud3tn-stress-1
NS2=ud3tn-stress-2
VE1=ud3tn-st-veth1
VE2=ud3tn-st-veth1
IP1="10.101.101.1"
IP2="10.101.101.2"

UD3TN_1=0
UD3TN_2=0
UD3TN_3=0
BDM_1=0
BDM_2=0
BDM_3=0

UFLAG=""
PYFLAG=""

case $DEBUG in
    2)
        UFLAG="-L 4"
        PYFLAG="-vv"
        ;;
    1)
        UFLAG="-L 3"
        PYFLAG="-vv"
        ;;
    *)
        UFLAG="-L 2"
        PYFLAG=""
        ;;
esac

cleanup() {
    [ $UD3TN_1 -ne 0 ] && pkill -P $UD3TN_1
    [ $UD3TN_2 -ne 0 ] && pkill -P $UD3TN_2
    [ $UD3TN_3 -ne 0 ] && pkill -P $UD3TN_3
    [ $BDM_1 -ne 0 ] && pkill -P $BDM_1
    [ $BDM_2 -ne 0 ] && pkill -P $BDM_2
    [ $BDM_3 -ne 0 ] && pkill -P $BDM_3
    ip netns del $NS1 > /dev/null 2>&1 || true
    ip netns del $NS2 > /dev/null 2>&1 || true
}

trap cleanup EXIT
cleanup

ip netns add $NS1
ip netns add $NS2

ip link add $VE2 netns $NS2 type veth peer $VE1 netns $NS1

ip -netns $NS1 link set dev lo up
ip -netns $NS2 link set dev lo up
ip -netns $NS1 link set dev $VE1 up
ip -netns $NS2 link set dev $VE2 up

ip -netns $NS1 addr add "$IP1/24" dev $VE1
ip -netns $NS2 addr add "$IP2/24" dev $VE2

if [[ $mode == "bdm" ]]; then
    EXT_DISPATCH_FLAG="--external-dispatch"
else
    EXT_DISPATCH_FLAG=""
fi

ip netns exec $NS1 sudo -u $RUNAS_USER AAP2_ADM_SECRET=$AAP2_ADM_SECRET "$WORK_DIR/build/posix/ud3tn" $UFLAG --bdm-secret-var AAP2_ADM_SECRET -c "sqlite:$SOCK_DIR/ud3tn1.sqlite;mtcp:$IP1,4222" -e "dtn://ud3tn1.dtn/" -s "$SOCK_DIR/ud3tn1.socket" -S "$SOCK_DIR/ud3tn1.aap2.socket" $EXT_DISPATCH_FLAG > "$SOCK_DIR/ud3tn1.log" 2>&1 &
UD3TN_1=$!
ip netns exec $NS2 sudo -u $RUNAS_USER AAP2_ADM_SECRET=$AAP2_ADM_SECRET "$WORK_DIR/build/posix/ud3tn" $UFLAG --bdm-secret-var AAP2_ADM_SECRET -c "sqlite:$SOCK_DIR/ud3tn2.sqlite;mtcp:$IP2,4222" -e "dtn://ud3tn2.dtn/" -s "$SOCK_DIR/ud3tn2.socket" -S "$SOCK_DIR/ud3tn2.aap2.socket" $EXT_DISPATCH_FLAG > "$SOCK_DIR/ud3tn2.log" 2>&1 &
UD3TN_2=$!
ip netns exec $NS1 sudo -u $RUNAS_USER AAP2_ADM_SECRET=$AAP2_ADM_SECRET "$WORK_DIR/build/posix/ud3tn" $UFLAG --bdm-secret-var AAP2_ADM_SECRET -c "sqlite:$SOCK_DIR/ud3tn3.sqlite;mtcp:$IP1,4223" -e "dtn://ud3tn3.dtn/" -s "$SOCK_DIR/ud3tn3.socket" -S "$SOCK_DIR/ud3tn3.aap2.socket" $EXT_DISPATCH_FLAG > "$SOCK_DIR/ud3tn3.log" 2>&1 &
UD3TN_3=$!

sleep 1 # give ud3tn some time to start

if [[ $mode == "bdm" ]]; then

    echo "MODE: BDM CONTACT DISPATCH" >&2
    # Start BDMs and wait a bit for them to start
    ip netns exec $NS1 sudo -u $RUNAS_USER AAP2_ADM_SECRET=$AAP2_ADM_SECRET "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_bdm_ud3tn_routing.py" --socket "$SOCK_DIR/ud3tn1.aap2.socket" --secret-var AAP2_ADM_SECRET > "$SOCK_DIR/bdm_ud3tn1.log" $PYFLAG 2>&1 &
    BDM_1=$!
    ip netns exec $NS2 sudo -u $RUNAS_USER AAP2_ADM_SECRET=$AAP2_ADM_SECRET "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_bdm_ud3tn_routing.py" --socket "$SOCK_DIR/ud3tn2.aap2.socket" --secret-var AAP2_ADM_SECRET > "$SOCK_DIR/bdm_ud3tn2.log" $PYFLAG 2>&1 &
    BDM_2=$!
    ip netns exec $NS1 sudo -u $RUNAS_USER AAP2_ADM_SECRET=$AAP2_ADM_SECRET "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_bdm_ud3tn_routing.py" --socket "$SOCK_DIR/ud3tn3.aap2.socket" --secret-var AAP2_ADM_SECRET > "$SOCK_DIR/bdm_ud3tn3.log" $PYFLAG 2>&1 &
    BDM_3=$!
    sleep 1

    # Configure
    echo "Configuring contacts to send bundles in a loop." >&2
    "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_config.py" --socket "$SOCK_DIR/ud3tn1.aap2.socket" --secret-var AAP2_ADM_SECRET --schedule 3 100000000 10000000000000 --reaches ipn:1.1 dtn://ud3tn2.dtn/ "mtcp:$IP2:4222" $PYFLAG > /dev/null
    "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_config.py" --socket "$SOCK_DIR/ud3tn2.aap2.socket" --secret-var AAP2_ADM_SECRET --schedule 1 100000000 10000000000000 --reaches ipn:1.1 dtn://ud3tn1.dtn/ "mtcp:$IP1:4222" $PYFLAG > /dev/null
    "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_config.py" --socket "$SOCK_DIR/ud3tn3.aap2.socket" --secret-var AAP2_ADM_SECRET --schedule 1 100000000 10000000000000 --reaches ipn:1.1 dtn://ud3tn1.dtn/ "mtcp:$IP1:4222" $PYFLAG > /dev/null

else

    if [[ $mode == "compat" ]]; then

        echo "MODE: COMPAT ROUTER DISPATCH" >&2
        # Configure
        echo "Configuring contacts to send bundles in a loop." >&2
        "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_config.py" --socket "$SOCK_DIR/ud3tn1.aap2.socket" --secret-var AAP2_ADM_SECRET --schedule 3 100000000 10000000000000 --reaches ipn:1.1 dtn://ud3tn2.dtn/ "mtcp:$IP2:4222" $PYFLAG > /dev/null
        "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_config.py" --socket "$SOCK_DIR/ud3tn2.aap2.socket" --secret-var AAP2_ADM_SECRET --schedule 1 100000000 10000000000000 --reaches ipn:1.1 dtn://ud3tn1.dtn/ "mtcp:$IP1:4222" $PYFLAG > /dev/null
        "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_config.py" --socket "$SOCK_DIR/ud3tn3.aap2.socket" --secret-var AAP2_ADM_SECRET --schedule 1 100000000 10000000000000 --reaches ipn:1.1 dtn://ud3tn1.dtn/ "mtcp:$IP1:4222" $PYFLAG > /dev/null

    else

        echo "MODE: FIB DISPATCH" >&2
        echo "Configuring FIB links to send bundles in a loop." >&2
        "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_configure_link.py" --socket "$SOCK_DIR/ud3tn1.aap2.socket" --secret-var AAP2_ADM_SECRET ipn:1.0 "mtcp:$IP2:4222" $PYFLAG > /dev/null
        "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_configure_link.py" --socket "$SOCK_DIR/ud3tn2.aap2.socket" --secret-var AAP2_ADM_SECRET ipn:1.0 "mtcp:$IP1:4222" $PYFLAG > /dev/null
        "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_configure_link.py" --socket "$SOCK_DIR/ud3tn3.aap2.socket" --secret-var AAP2_ADM_SECRET ipn:1.0 "mtcp:$IP1:4222" $PYFLAG > /dev/null

    fi

fi

# Start it!
echo "Injecting $BUNDLE_COUNT bundles with payload size $BUNDLE_SIZE bytes into the loop." >&2
for i in $(seq 1 $BUNDLE_COUNT); do
  cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w $BUNDLE_SIZE | head -n 1 | "$WORK_DIR/.venv/bin/python" "$WORK_DIR/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_send.py" --socket "$SOCK_DIR/ud3tn3.aap2.socket" ipn:1.1
done

echo "You should see some traffic in 1-2 seconds. Inspect it using: sudo ip netns exec $NS1 iftop -i $VE1" >&2

echo -n "> Press ENTER to exit and clean up..." >&2
read
