# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
import os
import select
import time

import pytest

from ud3tn_utils.config import (
    ConfigMessage,
    make_contact,
)

from pyd3tn.bundle7 import Bundle, BundleProcFlag, serialize_bundle7
from pyd3tn.mtcp import MTCPConnection

from .helpers import (
    TESTED_CLAS,
    UD3TN_CONFIG_EP,
    UD3TN_HOST,
    SMTCP_PORT,
    TCP_TIMEOUT,
    validate_bundle7,
    send_delete_gs,
)

SENDING_GS_DEF = ("dtn://sender.dtn/", "sender")
RECEIVING_GS_DEF = ("dtn://receiver.dtn/", "receiver")

SENDING_CONTACT = (1, 1, 1000)
RECEIVING_CONTACT = (3, 1, 1000)
RECEIVING_CONTACT_MNF_1 = (3, 1, 200)
RECEIVING_CONTACT_MNF_2 = (5, 1, 1000)
BUNDLE_SIZE = 200


def _ensure_nothing_received(sock, timeout):
    rl, _, _ = select.select([sock], [],  [], timeout)
    assert len(rl) == 0, "received data before timeout"


@pytest.mark.skipif("smtcp" not in TESTED_CLAS, reason="not selected")
def test_send_receive_smtcp_bundle7_fragment():
    cla_str = "smtcp"
    serialize_func = serialize_bundle7
    validate_func = validate_bundle7
    with MTCPConnection(UD3TN_HOST, SMTCP_PORT, timeout=TCP_TIMEOUT) as conn:
        outgoing_eid, outgoing_claaddr = SENDING_GS_DEF
        incoming_eid, incoming_claaddr = RECEIVING_GS_DEF
        # Configure contact during which we send a bundle
        conn.send_bundle(serialize_func(
            outgoing_eid,
            UD3TN_CONFIG_EP,
            bytes(ConfigMessage(
                outgoing_eid,
                cla_str + ":",
                contacts=[
                    make_contact(*SENDING_CONTACT),
                ],
            )),
        ))
        # Configure contact during which we want to receive the bundle
        conn.send_bundle(serialize_func(
            outgoing_eid,
            UD3TN_CONFIG_EP,
            bytes(ConfigMessage(
                incoming_eid,
                cla_str + ":",
                contacts=[
                    make_contact(*RECEIVING_CONTACT),
                ],
            )),
        ))
        try:
            # Wait until first contact starts and send bundle
            time.sleep(SENDING_CONTACT[0])
            payload_data = os.urandom(BUNDLE_SIZE)
            conn.send_bundle(serialize_func(
                outgoing_eid,
                incoming_eid,
                payload_data,
                flags=BundleProcFlag.IS_FRAGMENT,
                fragment_offset=1234,
                total_adu_length=(1234 + len(payload_data) + 1234),
            ))
            # Wait until second contact starts and try to receive bundle
            time.sleep(RECEIVING_CONTACT[0] - SENDING_CONTACT[0])
            bdl = conn.recv_bundle()
            validate_func(bdl, payload_data)
            b_obj = Bundle.parse(bdl)
            assert b_obj.is_fragmented
            assert b_obj.primary_block.fragment_offset == 1234
            assert b_obj.primary_block.total_payload_length == (
                1234 + len(payload_data) + 1234
            )
            time.sleep(RECEIVING_CONTACT[1])
        finally:
            send_delete_gs(conn, serialize_func, (outgoing_eid, incoming_eid))


@pytest.mark.skipif("smtcp" not in TESTED_CLAS, reason="not selected")
def test_send_receive_smtcp_bundle7_must_not_fragment():
    cla_str = "smtcp"
    serialize_func = serialize_bundle7
    validate_func = validate_bundle7
    with MTCPConnection(UD3TN_HOST, SMTCP_PORT, timeout=TCP_TIMEOUT) as conn:
        outgoing_eid, outgoing_claaddr = SENDING_GS_DEF
        incoming_eid, incoming_claaddr = RECEIVING_GS_DEF
        # Configure contact during which we send a bundle
        conn.send_bundle(serialize_func(
            outgoing_eid,
            UD3TN_CONFIG_EP,
            bytes(ConfigMessage(
                outgoing_eid,
                cla_str + ":",
                contacts=[
                    make_contact(*SENDING_CONTACT),
                ],
            )),
        ))
        # Configure contact during which we do not want to receive the bundle
        conn.send_bundle(serialize_func(
            outgoing_eid,
            UD3TN_CONFIG_EP,
            bytes(ConfigMessage(
                incoming_eid,
                cla_str + ":",
                contacts=[
                    make_contact(*RECEIVING_CONTACT_MNF_1),
                ],
            )),
        ))
        # Configure contact during which we want to receive the bundle
        conn.send_bundle(serialize_func(
            outgoing_eid,
            UD3TN_CONFIG_EP,
            bytes(ConfigMessage(
                incoming_eid,
                cla_str + ":",
                contacts=[
                    make_contact(*RECEIVING_CONTACT_MNF_2),
                ],
            )),
        ))
        try:
            # Wait until first contact starts and send bundle
            time.sleep(SENDING_CONTACT[0])
            payload_data = os.urandom(BUNDLE_SIZE)
            conn.send_bundle(serialize_func(
                outgoing_eid,
                incoming_eid,
                payload_data,
                flags=BundleProcFlag.MUST_NOT_BE_FRAGMENTED,
            ))
            # Wait until second contact starts and ensure nothing is received
            time.sleep(RECEIVING_CONTACT_MNF_1[0] - SENDING_CONTACT[0])
            _ensure_nothing_received(conn.sock, timeout=1)
            # Wait until third contact starts and try to receive bundle
            time.sleep(
                RECEIVING_CONTACT_MNF_2[0] - RECEIVING_CONTACT_MNF_1[0] - 1
            )
            bdl = conn.recv_bundle()
            validate_func(bdl, payload_data)
            time.sleep(RECEIVING_CONTACT[1])
        finally:
            send_delete_gs(conn, serialize_func, (outgoing_eid, incoming_eid))
