#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
# encoding: utf-8

import argparse
import logging
import sys

import cbor2  # type: ignore

from pyd3tn.bundle7 import Bundle
from ud3tn_utils.aap import AAPUnixClient, AAPTCPClient
from ud3tn_utils.aap.aap_message import AAPMessageType
from ud3tn_utils.aap.bin.helpers import (
    add_common_parser_arguments,
    initialize_logger,
)


logger = logging.getLogger(__name__)


def run_aap_recv(aap_client, max_count, output, verify_pl, newline):
    logger.info("Waiting for bundles...")
    counter = 0

    while True:
        try:
            msg = aap_client.receive()
        except KeyboardInterrupt:
            logger.info("Terminated by keyboard interrupt.")
            sys.exit(130)  # exit status for SIGINT
        if not msg:
            logger.warning("µD3TN has closed the connection.")
            sys.exit(1)

        enc = False
        err = False
        if msg.msg_type == AAPMessageType.RECVBUNDLE:
            payload = msg.payload
        elif msg.msg_type == AAPMessageType.RECVBIBE:
            payload = cbor2.loads(msg.payload)
            bundle = Bundle.parse(payload[2])
            payload = bundle.payload_block.data
            enc = True

        if not err:
            enc = " encapsulated" if enc else ""
            logger.info(
                "Received%s bundle from '%s', payload len = %d",
                enc,
                msg.eid,
                len(payload),
            )
            output.write(payload)
            if newline:
                output.write(b"\n")
            output.flush()
            if verify_pl is not None and verify_pl.encode("utf-8") != payload:
                logger.fatal("Unexpected payload != '%s'", verify_pl)
                sys.exit(1)
        else:
            logger.warning(
                "Received administrative record of unknown type from '%s'!",
                msg.eid
            )

        counter += 1
        if max_count and counter >= max_count:
            logger.info("Expected amount of bundles received, terminating.")
            return


def main():
    parser = argparse.ArgumentParser(
        description="register an agent with uD3TN and wait for bundles",
    )

    add_common_parser_arguments(parser)

    parser.add_argument(
        "-c", "--count",
        type=int,
        default=None,
        help="amount of bundles to be received before terminating",
    )
    parser.add_argument(
        "-o", "--output",
        type=argparse.FileType("wb"),
        default=sys.stdout.buffer,
        help="file to write the received bundle contents",
    )
    parser.add_argument(
        "--verify-pl",
        default=None,
        help="verify that the payload is equal to the provided string",
    )
    parser.add_argument(
        "--newline",
        action="store_true",
        help="print a line feed character after every received bundle payload",
    )

    args = parser.parse_args()
    global logger
    logger = initialize_logger(args.verbosity)

    try:
        if args.tcp:
            with AAPTCPClient(address=args.tcp) as aap_client:
                aap_client.register(args.agentid)
                run_aap_recv(
                    aap_client,
                    args.count,
                    args.output,
                    args.verify_pl,
                    args.newline,
                )
        else:
            with AAPUnixClient(address=args.socket) as aap_client:
                aap_client.register(args.agentid)
                run_aap_recv(
                    aap_client,
                    args.count,
                    args.output,
                    args.verify_pl,
                    args.newline,
                )
    finally:
        args.output.close()


if __name__ == "__main__":
    main()
