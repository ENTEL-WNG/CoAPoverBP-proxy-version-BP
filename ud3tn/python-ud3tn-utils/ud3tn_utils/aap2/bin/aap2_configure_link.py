#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
# encoding: utf-8

import argparse
import logging
import sys

from ud3tn_utils.aap2 import (
    AAP2TCPClient,
    AAP2UnixClient,
    AAPMessage,
    AuthType,
    Link,
    LinkFlags,
    LinkStatus,
    ResponseStatus,
)
from ud3tn_utils.aap2.bin.helpers import (
    add_common_parser_arguments,
    get_secret_from_args,
    initialize_logger,
)


logger = logging.getLogger(__name__)


def main():
    parser = argparse.ArgumentParser(
        description="configure a FIB entry via AAP 2.0",
    )
    add_common_parser_arguments(parser)
    parser.add_argument(
        "NODE_ID",
        help="the next-hop node ID to be configured",
    )
    parser.add_argument(
        "CLA_ADDR",
        help="the CLA address used to reach the next hop",
    )
    parser.add_argument(
        "-i", "--indirect",
        action="store_true",
        help="do not set the DIRECT flag (require BDM dispatch for the link)",
    )
    parser.add_argument(
        "-d", "--delete",
        action="store_true",
        help="if set, the entry will be deleted",
    )
    args = parser.parse_args()
    global logger
    logger = initialize_logger(args.verbosity)

    if args.tcp:
        aap2_client = AAP2TCPClient(address=args.tcp)
    else:
        aap2_client = AAP2UnixClient(address=args.socket)
    with aap2_client:
        secret = aap2_client.configure(
            args.agentid,
            subscribe=False,
            secret=get_secret_from_args(args),
            auth_type=AuthType.AUTH_TYPE_FIB_CONTROL,
        )
        logger.info("Assigned agent secret: '%s'", secret)
        if args.delete:
            target_link_status = LinkStatus.LINK_STATUS_DOWN
        else:
            target_link_status = LinkStatus.LINK_STATUS_UP
        logger.info(
            "Sending link update for '%s' via '%s': %s",
            args.NODE_ID,
            args.CLA_ADDR,
            target_link_status,
        )
        aap2_client.send(AAPMessage(link=Link(
            status=target_link_status,
            flag=(
                LinkFlags.LINK_FLAG_NONE if args.indirect
                else LinkFlags.LINK_FLAG_DIRECT
            ),
            peer_node_id=args.NODE_ID,
            peer_cla_addr=args.CLA_ADDR,
        )))
        status = aap2_client.receive_response().response_status
        if status != ResponseStatus.RESPONSE_STATUS_SUCCESS:
            logger.warning(
                "µD3TN responded with unexpected status: %s (%d)",
                aap2_client.response_status_name(status),
                status,
            )
            sys.exit(1)


if __name__ == "__main__":
    main()
