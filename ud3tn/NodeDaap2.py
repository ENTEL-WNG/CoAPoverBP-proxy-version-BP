# NodeDaap2.py: A CoAP-over-BP proxy
# Copyright (C) 2025  Based on work by Michael Karpov
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
import random
from datetime import datetime, timedelta

from ud3tn_utils.aap2.aap2_client import AAP2AsyncUnixClient
from ud3tn_utils.aap2.generated import aap2_pb2

# Add aiocoap source to path
project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', 'aiocoap', 'src'))
sys.path.insert(0, project_root)

import aiocoap
from aiocoap.message import Message
from aiocoap.numbers.types import Type

# Socket addresses for node D
PROXY_ADDRESS = 'ud3tn-d.aap2.socket'
MAX_ID = 16777215 

class TokenMIDMapper:
    """Maps between original and proxy tokens/MIDs for CoAP messages."""
    
    def __init__(self, name):
        self.name = name
        self.next_mid = random.randint(1, 1000)  
        self.token_map = {}  # orig_token -> proxy_token
        self.reverse_token_map = {}  # proxy_token -> orig_token
        self.mid_map = {}  # orig_mid -> proxy_mid
        self.reverse_mid_map = {}  # proxy_mid -> orig_mid
        self.pending_requests = {}  # proxy_token -> (src_eid, timestamp)
        
    def generate_mid(self):
        """Generate the next message ID."""
        mid = self.next_mid
        self.next_mid = (self.next_mid % MAX_ID) + 1
        return mid
        
    def generate_token(self):
        """Generate a new unique token."""
        while True:
            token = os.urandom(2) 
            if token not in self.reverse_token_map:
                return token
    
    def map_request(self, msg, src_eid):
        """Map original token/MID to proxy token/MID for outgoing requests."""
        orig_token = msg.token
        orig_mid = msg.mid
        
        proxy_token = self.generate_token()
        proxy_mid = self.generate_mid()
        
        self.token_map[orig_token] = proxy_token
        self.reverse_token_map[proxy_token] = orig_token
        self.mid_map[orig_mid] = proxy_mid
        self.reverse_mid_map[proxy_mid] = orig_mid
        
        self.pending_requests[proxy_token] = (src_eid, datetime.now())
        
        msg.token = proxy_token
        msg.mid = proxy_mid
        
        print(f"[{self.name}] Mapped request: orig_token={orig_token.hex()} -> proxy_token={proxy_token.hex()}, "f"orig_mid={orig_mid} -> proxy_mid={proxy_mid}")
        
        return msg
    
    def map_response(self, msg):
        """Map proxy token/MID back to original token/MID for incoming responses."""
        proxy_token = msg.token
        proxy_mid = msg.mid
        
        if proxy_token in self.reverse_token_map:
            orig_token = self.reverse_token_map[proxy_token]
            src_eid, _ = self.pending_requests.get(proxy_token, (None, None))
            
            msg.token = orig_token
            
            if proxy_mid in self.reverse_mid_map:
                msg.mid = self.reverse_mid_map[proxy_mid]
                
            print(f"[{self.name}] Mapped response: proxy_token={proxy_token.hex()} -> orig_token={orig_token.hex()}, "
                        f"proxy_mid={proxy_mid} -> orig_mid={msg.mid}")
            
            return msg, src_eid
        else:
            print(f"[{self.name}] Unknown token in response: {proxy_token.hex()}")
            return None, None


class CoAPBundleProxy:
    """Proxy that forwards CoAP messages over Bundle Protocol between nodes."""
    
    def __init__(self):
        self.send_client = AAP2AsyncUnixClient(PROXY_ADDRESS)
        self.receive_client = AAP2AsyncUnixClient(PROXY_ADDRESS)

        self.a_to_b_mapper = TokenMIDMapper("A->B")
        self.b_to_a_mapper = TokenMIDMapper("B->A")
        
    async def configure_clients(self):
        """Configure all AAP2 clients."""
        await self.receive_client.configure(agent_id='rec', subscribe=True)
        await self.send_client.configure(agent_id='snd')
        
        print("AAP2 clients configured successfully")
        
    async def send_coap_message(self, msg_bytes, dst_eid):
        """Send a single CoAP message as a bundle."""
        try:
            await self.send_client.send_adu(
                aap2_pb2.BundleADU(
                    dst_eid=dst_eid,
                    payload_length=len(msg_bytes),
                ),
                msg_bytes,
            )
            print(f"Sent individual CoAP message to {dst_eid}")
                
        except Exception as e:
            print(f"Error sending CoAP message to {dst_eid}: {e}")
    
    async def process_messages(self):
        """Process all incoming CoAP messages from both Node A and Node B."""
        try:
            while True:
                adu_msg, recv_payload = await self.receive_client.receive_adu()
                print(f"Received ADU from {adu_msg.src_eid}: {len(recv_payload)} bytes")
                
                await self.receive_client.send_response_status(
                    aap2_pb2.ResponseStatus.RESPONSE_STATUS_SUCCESS
                )
                
                is_from_node_a = 'a.dtn' in adu_msg.src_eid
                is_from_node_b = 'b.dtn' in adu_msg.src_eid
                
                if not (is_from_node_a or is_from_node_b):
                    print(f"Received message from unknown source: {adu_msg.src_eid}, ignoring")
                    continue
                
                offset = 0
                while offset < len(recv_payload):
                    try:
                        partial = recv_payload[offset:]
                        msg = Message.decode(partial)
                        
                        if not msg.opt.payload_length:
                            raise ValueError("Missing Payload-Length option in incoming CoAP message")
                            
                        payload_length = msg.opt.payload_length
                        encoded_msg = msg.encode()
                        payload_marker_index = encoded_msg.find(b'\xFF')
                        
                        if payload_marker_index == -1:
                            raise ValueError("Payload Marker (0xFF) not found!")
                            
                        full_message_length = payload_marker_index + 1 + payload_length
                        coap_bytes = recv_payload[offset:offset + full_message_length]
                        offset += full_message_length
                        
                        coap_msg = Message.decode(coap_bytes)
                        
                        if is_from_node_a:
                            print(f"From A: Decoded CoAP request: MID={coap_msg.mid}, Token={coap_msg.token.hex()}, Code={coap_msg.code}")
                            
                            mapped_msg = self.a_to_b_mapper.map_request(coap_msg, adu_msg.src_eid)
                            
                            mapped_bytes = mapped_msg.encode()
                            
                            await self.send_coap_message(mapped_bytes, "dtn://b.dtn/rec")
                            
                        elif is_from_node_b:
                            print(f"From B: Decoded CoAP response: MID={coap_msg.mid}, Token={coap_msg.token.hex()}, Code={coap_msg.code}")
                            
                            mapped_msg, orig_src_eid = self.a_to_b_mapper.map_response(coap_msg)
                            
                            if mapped_msg and orig_src_eid:
                                mapped_bytes = mapped_msg.encode()
                                
                                await self.send_coap_message(mapped_bytes,  "dtn://a.dtn/rec")
                            else:
                                print("Could not map response from B, discarding")
                            
                    except Exception as e:
                        print(f"Error processing CoAP message: {e}")
                        if offset < len(recv_payload):
                            offset += 1  
                        else:
                            break
                            
        except Exception as e:
            print(f"Error in process_messages: {e}")
        finally:
            print("Message processing task ended")
    
    async def run(self):
        """Start the proxy server."""
        async with self.send_client, self.receive_client:
            await self.configure_clients()
            
            await asyncio.gather(
                self.process_messages()
            )

if __name__ == "__main__":
    proxy = CoAPBundleProxy()
    try:
        asyncio.run(proxy.run())
    except KeyboardInterrupt:
        print("Proxy shutting down...")
    except Exception as e:
        print(f"Fatal error: {e}")
        sys.exit(1)