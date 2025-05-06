#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
# encoding: utf-8

import argparse
import logging

from ud3tn_utils.aap2 import (
    AAP2TCPClient,
    AAP2UnixClient,
    AuthType,
    BundleADU,
    BundleADUFlags,
    ResponseStatus,
)
from ud3tn_utils.config import ConfigMessage, make_contact
from ud3tn_utils.aap2.bin.helpers import (
    add_common_parser_arguments,
    get_config_eid,
    get_secret_from_args,
    initialize_logger,
)


logger = logging.getLogger(__name__)


def main():
    import sys

    parser = argparse.ArgumentParser(
        description="configure a contact via uD3TN's AAP 2.0 interface",
    )
    add_common_parser_arguments(parser)
    parser.add_argument(
        "--dest_eid",
        default=None,
        help="the EID of the node to which the configuration belongs",
    )
    parser.add_argument(
        "eid",
        help="the EID of the node to which the contact exists",
    )
    parser.add_argument(
        "cla_address",
        help="the CLA address of the node",
    )
    parser.add_argument(
        "-s", "--schedule",
        nargs=3,
        type=int,
        metavar=("start_offset", "duration", "bitrate"),
        action="append",
        default=[],
        help="schedule a contact relative to the current time",
    )
    parser.add_argument(
        "-r", "--reaches",
        type=str,
        action="append",
        default=[],
        help="specify an EID reachable via the node",
    )
    args = parser.parse_args()
    global logger
    logger = initialize_logger(args.verbosity)

    if not args.schedule:
        logger.fatal("At least one -s/--schedule argument must be given!")
        sys.exit(1)

    msg = bytes(ConfigMessage(
        args.eid,
        args.cla_address,
        contacts=[
            make_contact(*contact)
            for contact in args.schedule
        ],
        reachable_eids=args.reaches,
    ))

    logger.debug("> %s", msg)

    if args.tcp:
        aap2_client = AAP2TCPClient(address=args.tcp)
    else:
        aap2_client = AAP2UnixClient(address=args.socket)
    with aap2_client:
        secret = aap2_client.configure(
            args.agentid,
            subscribe=False,
            secret=get_secret_from_args(args),
            auth_type=AuthType.AUTH_TYPE_BUNDLE_DISPATCH,
        )
        logger.info("Assigned agent secret: '%s'", secret)
        dest_eid = args.dest_eid or aap2_client.node_eid
        aap2_client.send_adu(
            BundleADU(
                dst_eid=get_config_eid(dest_eid),
                payload_length=len(msg),
                adu_flags=[BundleADUFlags.BUNDLE_ADU_WITH_BDM_AUTH],
            ),
            msg,
        )
        assert (
            aap2_client.receive_response().response_status ==
            ResponseStatus.RESPONSE_STATUS_SUCCESS
        )


if __name__ == "__main__":
    main()
