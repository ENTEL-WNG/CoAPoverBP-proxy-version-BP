# NodeBaap2.py: A basic server that receives CoAP messages over DTN using AAP2 and manages simple dynamic resources.
# Copyright (C) 2025  Michael Karpov
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License as published by
# the Free Software Foundation, either version 3 of the License, or any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

import asyncio
import sys
import os
import json

from ud3tn_utils.aap2.aap2_client import AAP2AsyncUnixClient
from ud3tn_utils.aap2.generated import aap2_pb2

# Add aiocoap source to path
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'aiocoap', 'src'))
sys.path.insert(0, project_root)

import aiocoap
from aiocoap.message import Message
from aiocoap.numbers.types import Type
import aiocoap.resource as resource

SERVER_ADDRESS = 'ud3tn-b.aap2.socket'
DATABASE_FILE = 'resources.json'
resources = {}

def save_resources_to_file():
    """Persist resource values to disk."""
    data_to_save = {name: resource.data for name, resource in resources.items()}
    with open(DATABASE_FILE, 'w') as f:
        json.dump(data_to_save, f)
    print("Database saved to", DATABASE_FILE)

def load_resources_from_file():
    """Load persisted resource values if available."""
    if not os.path.exists(DATABASE_FILE):
        print("No database file found, starting fresh.")
        return

    with open(DATABASE_FILE, 'r') as f:
        loaded_data = json.load(f)
        for name, data in loaded_data.items():
            resources[name] = DynamicResource(initial_data=data)
    print("Database loaded from", DATABASE_FILE)

class DynamicResource(resource.Resource):
    """A resource supporting GET, PUT, and DELETE methods."""
    def __init__(self, initial_data=None):
        super().__init__()
        if initial_data is None:
            initial_data = {'values': []} 
        self.data = initial_data

    async def render_get(self, request):
        payload = str(self.data['values']).encode('utf-8')
        return aiocoap.Message(code=aiocoap.CONTENT, payload=payload)

    async def render_put(self, request):
        try:
            value = request.payload.decode('utf-8')
            self.data['values'].append(value)
            save_resources_to_file()
            return aiocoap.Message(code=aiocoap.CHANGED, payload=b"Value appended.")
        except Exception as e:
            print("PUT error:", e)
            return aiocoap.Message(code=aiocoap.BAD_REQUEST, payload=b"Invalid PUT payload.")

    async def render_delete(self, request):
        self.data['values'].clear()
        save_resources_to_file()
        return aiocoap.Message(code=aiocoap.DELETED, payload=b"Resource values cleared.")

async def handle_post(request):
    """Handle POST requests to create new resources."""
    try:
        name = request.payload.decode('utf-8').strip()
        if not name:
            return aiocoap.Message(code=aiocoap.BAD_REQUEST, payload=b"Missing resource name.")

        if name in resources:
            return aiocoap.Message(code=aiocoap.BAD_REQUEST, payload=b"Resource already exists.")

        resources[name] = DynamicResource()
        save_resources_to_file()

        print(f"Created new resource: /{name}")
        return aiocoap.Message(code=aiocoap.CREATED, payload=f"Created {name}".encode('utf-8'))
    except Exception as e:
        print("POST error:", e)
        return aiocoap.Message(code=aiocoap.BAD_REQUEST, payload=b"Invalid POST payload.")

send_client = AAP2AsyncUnixClient(SERVER_ADDRESS)
receive_client = AAP2AsyncUnixClient(SERVER_ADDRESS)

async def main():
    """Load resources and start CoAP-over-BP handler."""
    load_resources_from_file()

    async with send_client, receive_client:
        await send_client.configure(agent_id='snd')
        await receive_client.configure(agent_id='rec', subscribe=True)
        
        await bundle_coap_server(receive_client, send_client)

async def bundle_coap_server(receive_client, send_client):
    """Handle CoAP messages received via bundle protocol."""
    try:
        while True:
            adu_msg, recv_payload = await receive_client.receive_adu()
            print(f"Received ADU from {adu_msg.src_eid}: {len(recv_payload)} bytes")

            await receive_client.send_response_status(
                aap2_pb2.ResponseStatus.RESPONSE_STATUS_SUCCESS
            )

            offset = 0
            while offset < len(recv_payload):
                partial = recv_payload[offset:]
                try:
                    msg = Message.decode(partial)

                    if not msg.opt.payload_length:
                        raise ValueError("Missing Payload-Length option in incoming CoAP message")

                    payload_length = msg.opt.payload_length
                    encoded = msg.encode()
                    payload_marker_index = encoded.find(b'\xFF')
                    if payload_marker_index == -1:
                        raise ValueError("Payload Marker (0xFF) not found!")

                    full_message_length = payload_marker_index + 1 + payload_length
                    coap_bytes = recv_payload[offset:offset + full_message_length]
                    offset += full_message_length

                    request = Message.decode(coap_bytes)

                    print(f"Parsed CoAP request: MID={request.mid} Token={request.token.hex()} Code={request.code} Payload Length={request.opt.payload_length}")

                    path = request.opt.uri_path
                    resource_name = path[0] if path else ''

                    if resource_name in resources:
                        resource = resources[resource_name]
                        if request.code == aiocoap.PUT:
                            handler = getattr(resource, 'render_put', None)
                        elif request.code == aiocoap.GET:
                            handler = getattr(resource, 'render_get', None)
                        elif request.code == aiocoap.DELETE:
                            handler = getattr(resource, 'render_delete', None)
                        else:
                            handler = None

                        if handler:
                            try:
                                response = await handler(request)
                            except Exception as e:
                                print("Error during resource handling:", e)
                                response = aiocoap.Message(code=aiocoap.INTERNAL_SERVER_ERROR, payload=b"Internal server error")
                        else:
                            response = aiocoap.Message(code=aiocoap.METHOD_NOT_ALLOWED, payload=b"Method not allowed")
                    else:
                        if request.code == aiocoap.POST:
                            response = await handle_post(request)
                        else:
                            response = aiocoap.Message(code=aiocoap.NOT_FOUND, payload=b"Resource not found")

                    response.token = request.token
                    response.mid = request.mid
                    response.mtype = Type.ACK if request.mtype != Type.NON else Type.NON
                    response.opt.payload_length = len(response.payload)

                    payload = response.encode()
                    await send_client.send_adu(
                        aap2_pb2.BundleADU(
                            dst_eid="dtn://d.dtn/rec",
                            payload_length=len(payload)
                        ),
                        payload,
                    )
                    await send_client.receive_response()

                except Exception as e:
                    print("Failed to parse CoAP message:", e)
                    break

    finally:
        await receive_client.disconnect()
        await send_client.disconnect()

if __name__ == "__main__":
    asyncio.run(main())
