#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# encoding: utf-8

"""Minimal Bundle Dispatcher service with static routing for uD3TN-NG."""

import argparse
import json
import logging
import sys

from pyd3tn.eid import get_node_id

from ud3tn_utils.aap2 import (
    AAP2UnixClient,
    AAP2TCPClient,
    AAP2ServerDisconnected,
    AAPResponse,
    AuthType,
    DispatchReason,
    DispatchResult,
    ResponseStatus,
)
from ud3tn_utils.aap2.bin.helpers import (
    add_common_parser_arguments,
    add_keepalive_parser_argument,
    get_secret_from_args,
    initialize_logger,
)

# Which DispatchReason values we accept for bundles to be forwarded
# (in other cases they are dropped).
VALID_DISPATCH_REASONS = (
    DispatchReason.DISPATCH_REASON_NO_FIB_ENTRY,
)


logger = logging.getLogger(__name__)


def run_static_bdm(aap2_client, routing_dict):
    logger.info("Waiting for dispatch event...")

    while True:
        try:
            msg = aap2_client.receive_msg()
        except AAP2ServerDisconnected:
            logger.warning("µD3TN has closed the connection.")
            sys.exit(1)
        except KeyboardInterrupt:
            logger.info("Terminated by keyboard interrupt.")
            sys.exit(130)  # exit status for SIGINT

        if msg.WhichOneof("msg") == "keepalive":
            aap2_client.send_response_status(
                ResponseStatus.RESPONSE_STATUS_ACK,
            )
            continue
        elif msg.WhichOneof("msg") != "dispatch_event":
            logger.info("Received message with field '%s' set, discarding.",
                        msg.WhichOneof("msg"))
            aap2_client.send_response_status(
                ResponseStatus.RESPONSE_STATUS_SUCCESS,
            )
            continue

        if msg.dispatch_event.reason not in VALID_DISPATCH_REASONS:
            logger.info("Received dispatch event with reason %d, dropping.",
                        msg.dispatch_event.reason)
            aap2_client.send_response_status(
                ResponseStatus.RESPONSE_STATUS_SUCCESS,
            )
            continue

        # Main dispatch logic
        bdl = msg.dispatch_event.bundle
        dst_node_id = get_node_id(bdl.dst_eid)
        next_hop_node_id = routing_dict.get(
            dst_node_id,
            # default: assume identity (destination must be next hop)
            dst_node_id,
        )
        logger.info("Determined next hop '%s' for bundle with dst = '%s'.",
                    next_hop_node_id, bdl.dst_eid)
        disp_result = DispatchResult(
            next_hops=[DispatchResult.NextHopEntry(node_id=next_hop_node_id)]
        )
        aap2_client.send(AAPResponse(
            response_status=ResponseStatus.RESPONSE_STATUS_SUCCESS,
            dispatch_result=disp_result,
        ))


def main():
    parser = argparse.ArgumentParser(
        description="bundle dispatcher module using a static table",
    )
    add_common_parser_arguments(parser)
    add_keepalive_parser_argument(parser)
    parser.add_argument(
        "routing_table_file",
        help="the JSON file to determine the next hop for a given bundle",
    )

    args = parser.parse_args()
    global logger
    logger = initialize_logger(args.verbosity)

    with open(args.routing_table_file) as f:
        routing_dict = json.load(f)

    if args.tcp:
        aap2_client = AAP2TCPClient(address=args.tcp)
    else:
        aap2_client = AAP2UnixClient(address=args.socket)
    with aap2_client:
        secret = aap2_client.configure(
            args.agentid,
            subscribe=True,
            secret=get_secret_from_args(args),
            auth_type=AuthType.AUTH_TYPE_BUNDLE_DISPATCH,
            keepalive_seconds=args.keepalive_seconds,
        )
        logger.info("AAP 2.0 agent registered and configured as BDM!")
        logger.info("Assigned agent secret: '%s'", secret)
        run_static_bdm(aap2_client, routing_dict)


if __name__ == "__main__":
    main()
