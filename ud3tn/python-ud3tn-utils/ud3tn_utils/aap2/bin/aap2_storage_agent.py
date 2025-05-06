#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
# encoding: utf-8

import argparse
import logging
import sys

from google.protobuf.internal import encoder

from ud3tn_utils.aap2 import (
    AAP2TCPClient,
    AAP2UnixClient,
    AuthType,
    BundleADU,
    BundleADUFlags,
    ResponseStatus,
)
from ud3tn_utils.aap2.bin.helpers import (
    add_common_parser_arguments,
    get_secret_from_args,
    initialize_logger,
)
from ud3tn_utils.storage_agent import StorageCall, StorageOperation


logger = logging.getLogger(__name__)


def _init_parser():
    parser = argparse.ArgumentParser(
        description="send StorageAgent commands via uD3TN's AAP2 interface",
    )

    add_common_parser_arguments(parser)

    parser.add_argument(
        "--storage-agent-eid",
        type=str,
        default="dtn://ud3tn.dtn/sqlite",
        help="destination EID (dtn or ipn URI) of the StorageAgent",
    )

    subparser = parser.add_subparsers(dest="cmd")

    delete_parser = subparser.add_parser("delete")
    delete_parser.add_argument(
        "--dest-eid-glob",
        type=str,
        help="delete bundles that match the specified destination EID glob"
    )
    delete_parser.add_argument(
        "--compound-bundle-id",
        type=str,
        nargs=6,
        metavar=(
            'SOURCE',
            'DESTINATION',
            'CREATION_TIMESTAMP',
            'SEQUENCE_NUMBER',
            'FRAGMENT_OFFSET',
            'PAYLOAD_LENGTH'
        ),
        help="delete the bundle that matches the compound bundle id"
    )

    push_parser = subparser.add_parser("push")
    push_parser.add_argument(
        "--dest-eid-glob",
        type=str,
        help="push bundles that match the specified destination EID glob"
    )
    push_parser.add_argument(
        "--compound-bundle-id",
        type=str,
        nargs=6,
        metavar=(
            'SOURCE',
            'DESTINATION',
            'CREATION_TIMESTAMP',
            'SEQUENCE_NUMBER',
            'FRAGMENT_OFFSET',
            'PAYLOAD_LENGTH'
        ),
        help="push the bundle that match the compound bundle id"
    )

    return parser


def main():
    parser = _init_parser()
    args = parser.parse_args()
    global logger
    logger = initialize_logger(args.verbosity)

    cmd = StorageCall()
    if args.cmd == "delete":
        cmd.operation = StorageOperation.STORAGE_OPERATION_DELETE_BUNDLES
    elif args.cmd == "push":
        cmd.operation = StorageOperation.STORAGE_OPERATION_PUSH_BUNDLES
    else:
        logger.error(f"Invalid `cmd` argument: {args.cmd}")
        parser.print_help()
        sys.exit(1)

    if args.compound_bundle_id:
        cmd.id.source_eid = args.compound_bundle_id[0]
        cmd.id.destination_eid = args.compound_bundle_id[1]
        cmd.id.creation_timestamp = int(args.compound_bundle_id[2])
        cmd.id.sequence_number = int(args.compound_bundle_id[3])
        cmd.id.fragment_offset = int(args.compound_bundle_id[4])
        cmd.id.payload_length = int(args.compound_bundle_id[5])
    elif args.dest_eid_glob:
        cmd.metadata.eid_glob = args.dest_eid_glob
    else:
        logger.error("Missing filter")
        parser.print_help()
        sys.exit(1)

    cmd_bytes = cmd.SerializeToString()
    proto_msg = encoder._VarintBytes(len(cmd_bytes)) + cmd_bytes

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

        aap2_client.send_adu(
            BundleADU(
                dst_eid=args.storage_agent_eid,
                payload_length=len(proto_msg),
                adu_flags=[BundleADUFlags.BUNDLE_ADU_WITH_BDM_AUTH],
            ),
            proto_msg,
        )

        assert (
            aap2_client.receive_response().response_status ==
            ResponseStatus.RESPONSE_STATUS_SUCCESS
        )


if __name__ == "__main__":
    main()
