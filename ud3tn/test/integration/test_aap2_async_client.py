# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0
import pytest

from ud3tn_utils.aap2 import (
    AAP2AsyncUnixClient,
    BundleADU,
    ResponseStatus,
)

from .helpers import (
    AAP2_AGENT_ID,
    AAP2_SOCKET,
    TEST_AAP2_ASYNC,
)


@pytest.mark.skipif(not TEST_AAP2_ASYNC,
                    reason="TEST_AAP2_ASYNC disabled via environment")
@pytest.mark.asyncio
async def test_aap2_async_adu():
    rpc_client = AAP2AsyncUnixClient(address=AAP2_SOCKET)
    sub_client = AAP2AsyncUnixClient(address=AAP2_SOCKET)
    async with rpc_client, sub_client:
        s = await rpc_client.configure(
            AAP2_AGENT_ID + "-a1",
            subscribe=False,
        )
        await sub_client.configure(
            AAP2_AGENT_ID + "-a1",
            subscribe=True,
            secret=s,
        )
        payload = b"testbundle"
        await rpc_client.send_adu(
            BundleADU(
                dst_eid=rpc_client.eid,
                payload_length=len(payload),
            ),
            payload,
        )
        response = await rpc_client.receive_response()
        assert (response.response_status ==
                ResponseStatus.RESPONSE_STATUS_SUCCESS)
        msg = await sub_client.receive_msg()
        assert msg is not None
        assert msg.WhichOneof("msg") == "adu"
        adu_msg, bundle_data = await sub_client.receive_adu(msg.adu)
        # Check that reporting processing failure is ok and valid.
        await sub_client.send_response_status(
            ResponseStatus.RESPONSE_STATUS_SUCCESS
        )
        assert bundle_data == payload
        assert adu_msg.payload_length == len(payload)
        assert adu_msg.src_eid == rpc_client.eid
