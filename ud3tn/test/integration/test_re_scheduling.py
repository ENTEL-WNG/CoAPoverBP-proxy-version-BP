# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
import os
import time

import pytest

from ud3tn_utils.config import (
    ConfigMessage,
    make_contact,
    RouterCommand,
)

from pyd3tn.bundle7 import serialize_bundle7
from pyd3tn.helpers import CommunicationError
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
RECEIVING_CONTACT_1 = (3, 1, 1000)
RECEIVING_CONTACT_2 = (5, 1, 1000)
BUNDLE_SIZE = 200


def _setup_contacts(conn, serialize_func, c1, c2):
    outgoing_eid, outgoing_claaddr = SENDING_GS_DEF
    incoming_eid, incoming_claaddr = RECEIVING_GS_DEF
    cla_str = "smtcp"
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
                c1,
                c2,
            ],
        )),
    ))


def _drop_contact(conn, serialize_func, contact):
    outgoing_eid, outgoing_claaddr = SENDING_GS_DEF
    incoming_eid, incoming_claaddr = RECEIVING_GS_DEF
    cla_str = "smtcp"
    conn.send_bundle(serialize_func(
        outgoing_eid,
        UD3TN_CONFIG_EP,
        bytes(ConfigMessage(
            incoming_eid,
            cla_str + ":",
            contacts=[
                contact,
            ],
            type=RouterCommand.DELETE,
        )),
    ))


@pytest.mark.skipif("smtcp" not in TESTED_CLAS, reason="not selected")
def test_re_schedule_smtcp_bundle7():
    serialize_func = serialize_bundle7
    validate_func = validate_bundle7
    outgoing_eid, outgoing_claaddr = SENDING_GS_DEF
    incoming_eid, incoming_claaddr = RECEIVING_GS_DEF
    with MTCPConnection(UD3TN_HOST, SMTCP_PORT, timeout=TCP_TIMEOUT) as conn:
        # Configure contact during which we send a bundle
        c1 = make_contact(*RECEIVING_CONTACT_1)
        c2 = make_contact(*RECEIVING_CONTACT_2)
        _setup_contacts(conn, serialize_func, c1, c2)
        try:
            # Wait until first contact starts and send bundle
            time.sleep(SENDING_CONTACT[0])
            payload_data = os.urandom(BUNDLE_SIZE)
            conn.send_bundle(serialize_func(
                outgoing_eid,
                incoming_eid,
                payload_data,
            ))
            # Wait 1s (between ct), then, drop 1st contact -> re-scheduling
            time.sleep(1)
            _drop_contact(conn, serialize_func, c1)
            # Wait until 2nd contact
            time.sleep(RECEIVING_CONTACT_2[0] - SENDING_CONTACT[0] - 1)
            bdl = conn.recv_bundle()
            validate_func(bdl, payload_data)
            # Wait until end
            time.sleep(RECEIVING_CONTACT_2[1])
        finally:
            send_delete_gs(conn, serialize_func, (outgoing_eid, incoming_eid))


@pytest.mark.skipif("smtcp" not in TESTED_CLAS, reason="not selected")
def test_re_scheduling_drop_smtcp_bundle7():
    serialize_func = serialize_bundle7
    outgoing_eid, outgoing_claaddr = SENDING_GS_DEF
    incoming_eid, incoming_claaddr = RECEIVING_GS_DEF
    with MTCPConnection(UD3TN_HOST, SMTCP_PORT, timeout=TCP_TIMEOUT) as conn:
        # Configure contact during which we send a bundle
        c1 = make_contact(*RECEIVING_CONTACT_1)
        c2 = make_contact(*RECEIVING_CONTACT_2)
        _setup_contacts(conn, serialize_func, c1, c2)
        try:
            # Wait until first contact starts and send bundle
            time.sleep(SENDING_CONTACT[0])
            payload_data = os.urandom(BUNDLE_SIZE)
            conn.send_bundle(serialize_func(
                outgoing_eid,
                incoming_eid,
                payload_data,
            ))
            # Wait 1s (between ct), then, drop 2nd contact first and 1st
            # contact afterwards -> re-scheduling becomes impossible -> drop
            time.sleep(1)
            _drop_contact(conn, serialize_func, c2)
            _drop_contact(conn, serialize_func, c1)
            # Wait until 2nd contact
            time.sleep(RECEIVING_CONTACT_2[0] - SENDING_CONTACT[0] - 1)
            try:
                conn.recv_bundle()
            except CommunicationError:
                pass
            else:
                assert False, "seems we received something we did not expect"
            # Wait until end
            time.sleep(RECEIVING_CONTACT_2[1])
        finally:
            send_delete_gs(conn, serialize_func, (outgoing_eid, incoming_eid))
