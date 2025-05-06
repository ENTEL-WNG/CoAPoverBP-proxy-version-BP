#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
# encoding: utf-8

import argparse
import logging

import cbor2

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
from pyd3tn.bundle7 import (
    BibeProtocolDataUnit,
    Bundle,
    PayloadBlock,
    PrimaryBlock,
)


logger = logging.getLogger(__name__)


def build_bpdu(inner_source, inner_dest, payload):
    """Encapsulates a regular bundle with the chosen payload
    in a BIBE Administrative Record, thus forming a BIBE Bundle.

    Args:
        inner_source (str): Source EID of the encapsulated bundle
        inner_dest (str): Destination EID of the encapsulated bundle
        payload (bytes): The payload of the encapsulated bundle

    Returns:
        BPDU (bytes): The bytes making up the BPDU
    """
    inner_bundle = Bundle(
        PrimaryBlock(
            destination=inner_dest,
            source=inner_source,
        ),
        PayloadBlock(payload)
    )
    bibe_ar = BibeProtocolDataUnit(
        bundle=inner_bundle,
        transmission_id=0,
        retransmission_time=0,
        compatibility=False,
    )

    return cbor2.dumps(bibe_ar.record_data)


def main():
    import sys

    parser = argparse.ArgumentParser(
        description="send a bundle via uD3TN's AAP interface",
    )
    add_common_parser_arguments(parser)
    parser.add_argument(
        "dest_eid",
        help="the destination EID of the created bundle",
    )
    parser.add_argument(
        "PAYLOAD",
        default=None,
        nargs="?",
        help="the payload of the created bundle, (default: read from STDIN)",
    )
    parser.add_argument(
        "--bdm-auth",
        action="store_true",
        help=("request the BUNDLE_ADU_WITH_BDM_AUTH flag to be set, e.g., to "
              "manage BDMs (needs the administrative secret to be set)"))
    parser.add_argument(
        "--bibe-source",
        default=None,
        help=("if set, the payload will be encapsulated twice using BIBE, "
              "with the source of the inner bundle set to the given EID"))
    parser.add_argument(
        "--bibe-destination",
        default=None,
        help=("if set, the payload will be encapsulated twice using BIBE, "
              "with the inner bundle addressed to the given EID"))
    args = parser.parse_args()

    if args.PAYLOAD:
        payload = args.PAYLOAD.encode("utf-8")
    else:
        payload = sys.stdin.buffer.read()
        sys.stdin.buffer.close()

    global logger
    logger = initialize_logger(args.verbosity)

    is_bibe = (
        args.bibe_source is not None or
        args.bibe_destination is not None
    )
    if is_bibe and (args.bibe_source is None or args.bibe_destination is None):
        logger.fatal("--bibe-source and --bibe-destination must both be set")
        sys.exit(1)

    if args.tcp:
        aap2_client = AAP2TCPClient(address=args.tcp)
    else:
        aap2_client = AAP2UnixClient(address=args.socket)
    with aap2_client:
        secret = aap2_client.configure(
            args.agentid,
            subscribe=False,
            secret=get_secret_from_args(args),
            auth_type=(
                AuthType.AUTH_TYPE_DEFAULT if not args.bdm_auth
                else AuthType.AUTH_TYPE_BUNDLE_DISPATCH
            ),
        )
        logger.info("Assigned agent secret: '%s'", secret)
        if is_bibe:
            payload = build_bpdu(
                args.bibe_source,
                args.bibe_destination,
                payload,
            )
            flags = [BundleADUFlags.BUNDLE_ADU_BPDU]
        else:
            flags = [BundleADUFlags.BUNDLE_ADU_NORMAL]
        if args.bdm_auth:
            flags += [BundleADUFlags.BUNDLE_ADU_WITH_BDM_AUTH]
        aap2_client.send_adu(
            BundleADU(
                dst_eid=args.dest_eid,
                payload_length=len(payload),
                adu_flags=flags,
            ),
            payload,
        )
        assert (
            aap2_client.receive_response().response_status ==
            ResponseStatus.RESPONSE_STATUS_SUCCESS
        )


if __name__ == "__main__":
    main()
