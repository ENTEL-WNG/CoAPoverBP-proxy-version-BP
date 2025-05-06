#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
# encoding: utf-8

import argparse
import contextlib
import logging
import os
import shutil
import subprocess
import sqlite3
import sys
import threading
from time import sleep

from google.protobuf.internal import encoder

from ud3tn_utils.aap import AAPUnixClient
from ud3tn_utils.aap2 import (
    AAP2TCPClient,
    AAP2UnixClient,
    AuthType,
    BundleADU,
    BundleADUFlags,
    ResponseStatus,
)
from ud3tn_utils.aap2.bin.aap2_receive import run_aap_recv
from ud3tn_utils.config import ConfigMessage, make_contact
from ud3tn_utils.storage_agent import StorageCall, StorageOperation


def aap2_recv(verify_pl):
    with AAP2UnixClient(address="ud3tn2.aap2.socket") as aap2_client:
        secret = aap2_client.configure(
            "bundlesink",
            subscribe=True,
            secret=None,
            keepalive_seconds=0,
        )
        run_aap_recv(
            aap2_client,
            1,
            sys.stdout.buffer,
            verify_pl,
            True,
        )

def configure_mtcp_contact(aap2_client, node, start, duration):
    config_msg = bytes(ConfigMessage(
        node,
        "mtcp:localhost:4225",
        contacts=[
            make_contact(start, duration, 100000)
        ],
    ))

    aap2_client.send_adu(
        BundleADU(
            dst_eid="dtn://ud3tn1.dtn/config",
            payload_length=len(config_msg),
            adu_flags=[BundleADUFlags.BUNDLE_ADU_WITH_BDM_AUTH],
        ),
        config_msg,
    )

    assert (
        aap2_client.receive_response().response_status ==
        ResponseStatus.RESPONSE_STATUS_SUCCESS
    )

def aap2_send_msg(aap2_client, dst_node, msg):
    payload_bytes = msg.encode("utf-8")

    aap2_client.send_adu(
        BundleADU(
            dst_eid=f"{dst_node}/bundlesink",
            payload_length=len(payload_bytes),
        ),
        payload_bytes,
    )

    return aap2_client.receive_response()

if __name__ == "__main__":

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--db-file",
        help="SQLite database file",
    )
    args = parser.parse_args()

    logger = logging.getLogger(__name__)
    logger.setLevel(logging.DEBUG)

    test_dir = os.environ["TEST_DIR"]
    test_msg = "uD3TN SQLite storage test"

    # Run receiver thread
    recv_thread = threading.Thread(target=aap2_recv, args=(test_msg,))
    recv_thread.start()

    if args.db_file:
        db_ctx = contextlib.closing(sqlite3.connect(f"{test_dir}/test.sqlite"))
    else:
        db_ctx = contextlib.nullcontext()

    with AAP2UnixClient(address="ud3tn.aap2.socket") as aap2_client, db_ctx as db:
        secret = aap2_client.configure(
            None,
            subscribe=False,
            secret=None,
            auth_type=AuthType.AUTH_TYPE_BUNDLE_DISPATCH,
        )

        configure_mtcp_contact(aap2_client, "dtn://ud3tn2.dtn", 2, 1)
        configure_mtcp_contact(aap2_client, "dtn://ud3tn3.dtn", 5, 2)

        # Verify no bundle is stored in the database
        if db:
            rows = db.execute("SELECT * FROM bundles;").fetchall()
            assert len(rows) == 0, f"expected 0 bundles in DB, got {len(rows)}"

        resp = aap2_send_msg(
            aap2_client,
            "dtn://ud3tn2.dtn",
            test_msg,
        )
        assert (resp.response_status ==
                ResponseStatus.RESPONSE_STATUS_SUCCESS)
        bundle_headers = resp.bundle_headers
        aap2_send_msg(
            aap2_client,
            "dtn://ud3tn3.dtn",
            "Saved, but neither sent nor deleted",
        )
        aap2_send_msg(
            aap2_client,
            "dtn://ud3tn4.dtn",
            "Dropped immediately",
        )

        # Give uD3TN some time to forward stuff to BDM and to storage
        sleep(1)

        # Verify the expected bundles are stored in the database
        if db:
            rows = db.execute("SELECT * FROM bundles;").fetchall()
            assert len(rows) == 2, f"expected 2 bundles in DB, got {len(rows)}"

        # Wait for the start of the mtcp contact
        sleep(1.5)

        # Wait until after the end of the mtcp contact
        sleep(1)

        # Verify the bundle with `test_msg` is deleted from the database
        if db:
            rows = db.execute("SELECT * FROM bundles;").fetchall()
            assert len(rows) == 1, f"expected 1 bundle in DB, got {len(rows)}"

    recv_thread.join(timeout=1)
    if recv_thread.is_alive():
        sys.exit(1)
