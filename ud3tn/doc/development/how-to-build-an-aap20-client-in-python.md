# How to Build a µD3TN AAP 2.0 Client in Python

Using the [`ud3tn-utils`](./python_ud3tn_utils.md) Python library, it is straightforward to build [AAP 2.0](../aap20.md) client applications. This page explains how to build a basic client application that can send and receive bundles.

## Connecting to the AAP 2.0 Interface

For using the AAP 2.0 interface, four Python classes are available:

- `AAP2UnixClient`: a synchronous client connecting through a Unix domain / POSIX IPC socket. Such a socket is the **default and preferred** way to connect to the AAP 2.0 interface. Choose this if you do not want to use asynchronous Python (`asyncio`).
- `AAP2TCPClient`: a synchronous client using a plain **unencrypted** TCP connection.
- `AAP2AsyncUnixClient`: an [*asynchronous*](https://docs.python.org/3/library/asyncio.html) variant of `AAP2UnixClient`
- `AAP2AsyncTCPClient`: an *asynchronous* variant of `AAP2TCPClient`

Choose the variant that suits your use case best. When developing a tool that should support connectivity via either a Unix domain *or* TCP socket (like most tools supplied with µD3TN do), you need to build code paths for instantiating both variants. After the class instances have been created, they can be used in the same manner.

In this guide, we will work with the synchronous (non-`Async*`) variants. The asynchronous variants provide equivalent functionalities, with all methods interacting with the AAP 2.0 interface implemented as coroutines that must be awaited. At the end of this page, a full example for sending and receiving a bundle is provided for both synchronous and asynchronous client implementations.

Create a client class instance as follows:

```python
from ud3tn_utils import aap2

# Create a client instance, using the default socket in the current directory.
# Note: when building a reusable tool, the `address` parameter should be a
# variable that can be provided, e.g., through command line arguments.
# Also think about offering TCP connectivity through AAP2TCPClient.
client = aap2.AAP2UnixClient(address="ud3tn.aap2.socket")
```

If you need *bi-directional communication* (sending **and** receiving bundles, not either one), two instances must be created, as every AAP 2.0 connection is unidirectional. The (fixed) direction is set when the connection is configured in the next step.

```python
from ud3tn_utils import aap2

# Create two clients, one for sending and one for receiving bundles,
# with the default socket type and location.
send_client = aap2.AAP2UnixClient(address="ud3tn.aap2.socket")
recv_client = aap2.AAP2UnixClient(address="ud3tn.aap2.socket")
```

The connection is not established right away when creating the instance. It is recommended to use the context manager, offered by the client, to establish the connection:

```python
with client:
    # work with the client
    ...
```

This ensures that all buffers are flushed and the connection is closed when leaving the `with` block. It is equivalent to:

```python
client.connect()
try:
    # work with the client
    ...
finally:
    client.disconnect()
```

When using multiple connections at the same time (e.g., for bi-directional communication), both instances can be specified in the `with` clause:

```python
with send_client, recv_client:
    # work with the clients
    ...
```

## Configuration

Before an AAP 2.0 connection can be used, it has to be *configured*. Configuration in this regard also means asking µD3TN to register an *agent identifier* for the client, which is used to derive the DTN endpoint identifier (EID) for sending or receiving bundles.

Configuration is performed using the `configure` method of the client class:

```python
def configure(self, agent_id=None, subscribe=False, secret=None,
              auth_type=aap2_pb2.AuthType.AUTH_TYPE_DEFAULT,
              keepalive_seconds=0):
    ...
```

Below is a short explanation of each function argument:

- `agent_id`: the agent identifier to be registered. If µD3TN has a `dtn`-scheme EID (the default), this will become the *demux token*, e.g. `dtn://ud3tn.dtn/agentid` if `"agentid"` is specified here. If µD3TN has an `ipn`-scheme EID, this is a numeric string containing the intended service number.
- `subscribe`: the direction of control. If `True`, the connection will allow for receiving data from µD3TN (subscribing). If `False`, it can be used to send data to µD3TN (RPC, non-subscribing).
- `secret`: a shared secret to protect 1) jointly used endpoints and 2) administrative actions. If two clients want to register the same agent identifier (e.g., one wants to send and one wants to receive using the same endpoint), the value specified by both must match.
- `auth_type`: the requested AAP 2.0 authorization. If the client just intends to send or receive bundles that do not configure any system parameters (like contacts), the default value does not need to be changed. If anything other than `AUTH_TYPE_DEFAULT` is requested, the *administrative secret* of µD3TN (set via the `-x, --bdm-secret-var` argument of µD3TN) must be provided by the client.
- `keepalive_seconds`: the number of seconds to wait between AAP 2.0 keepalive messages, whereas `0` means to disable the keepalive feature (default). See the dedicated section below.

Note that:

- All actions must be performed with an active connection, i.e., inside the `with` block discussed in the previous section.
- The default behavior for `agent_id` and `secret` is to generate a random value. The latter will be returned by the function on success.
- If the agent identifier is already registered *and a distinct or empty secret is specified*, the configuration request is *declined*.
- When building a generic tool, it is highly recommended to allow the user to specify the agent identifier as well as the secret, e.g., through command line arguments.

For a simple script aiming to send and receive bundles using the agent identifier `"myagent"`, the connections can be configured as follows:

```python
# Register "myagent" for sending and generate (and store) a random secret.
secret = send_client.configure("myagent")
# Register "myagent" for subscribing to bundles.
recv_client.configure("myagent", True, secret)
```

## Sending Bundles

As soon as an RPC (non-subscribing) client connection has been configured successfully, sending bundle application data units (ADUs)[^1] is possible as follows:

```python
# The destination EID.
destination = "dtn://destination-node.dtn/receiving-agent"
# The bundle payload data - has to be a byte string.
payload = "This is my message.".encode("utf-8")
# Pass the metadata (destination + length) and the payload to µD3TN.
send_client.send_adu(
    aap2.BundleADU(
        dst_eid=destination,
        payload_length=len(payload),
    ),
    payload,
)
# µD3TN will respond to our request.
response = send_client.receive_response()
# Check that everything worked.
assert response.response_status == aap2.ResponseStatus.RESPONSE_STATUS_SUCCESS
```

[^1]: The core difference between an ADU and a bundle is that an ADU, as a transmission request, always contains all application data in an unencrypted form, whereas a bundle may be fragmented and/or encrypted. Fragments are not exposed by the AAP 2.0 interfaces for sending and receiving, they are handled transparently by µD3TN. Encryption ([BPSec](https://datatracker.ietf.org/doc/rfc9172/)) is currently not implemented in µD3TN.

µD3TN will create a bundle based on the provided information and payload data and may fragment, forward, or drop it at its discretion and based on its current configuration. Note that the AAP 2.0 agent will report success (as it created and injected the bundle), even if µD3TN's forwarder immediately drops the created bundle.

!!! warning "Warning"

    Do not forget to receive the response from µD3TN after sending the ADU, as shown in the code block above.

## Receiving Bundles

Using a subscribing connection, bundle ADUs can be received as follows:

```python
# Receive the next AAP 2.0 message, validate that it is an ADU,
# receive the payload, and return it.
adu_msg, payload = recv_client.receive_adu()
# Print what we received.
print(f"Received ADU from {adu_msg.src_eid}: {payload}")
# Report that we have successfully received the bundle.
recv_client.send_response_status(aap2.ResponseStatus.RESPONSE_STATUS_SUCCESS)
```

!!! warning "Warning"

    Do not forget to send a response to µD3TN after receiving the ADU, as shown in the code block above.

## Security Considerations

1. **Do not expose an insecure TCP connection.** It is generally advised to only use the POSIX IPC socket for [AAP](../ud3tn_aap.md) and AAP 2.0 in productive environments, which should additionally be protected with appropriate file permissions. AAP and AAP 2.0 support TCP clients, but do not provide any confidentiality or integrity protection by design. All data and the shared secret are transmitted without any encryption. Attackers may eavesdrop on data, control the µD3TN instance to connect to arbitrary nodes (including for performing DoS attacks) or disrupt its availability. This means that one must not allow the AAP / AAP 2.0 connection to pass untrusted nodes or links. Appropriate firewall rules and/or tunneling mechanisms should be applied.

2. **Use secure values for the shared secret.** The AAP 2.0 shared secret protects from malicious use of endpoints and extended (administrative) actions. At the moment, when checking it, µD3TN does not apply brute-force protections such as rate limiting. Thus, a long, random secret should be used in any case.

3. **Do not request more permissions than necessary and protect the administrative secret.** AAP 2.0 clients may request additional permissions when configuring a connection using the `auth_type` field. This should only be done when necessary, e.g., if the client intends to send configuration bundles to the contact configuration endpoint or if it needs to influence bundle forwarding decisions. The client should request only the minimum additional permissions necessary to perform its intended function. When passing the shared administrative secret to µD3TN (set via the `-x, --bdm-secret-var` argument of µD3TN), the client must handle this secret appropriately. For example, passing secrets via the command line is discouraged, as they will be visible in the operating system's process list. Instead, µD3TN and its Python tools use environment variables for this purpose.

4. **Test clients with release builds.** Release builds of µD3TN (i.e. when building the daemon via `make type=release optimize=yes`) enforce certain security features such as requiring to set a long administrative secret. Non-trivial AAP / AAP 2.0 client applications should always be tested to work also with release builds of µD3TN.

## Using the Keepalive Feature

Especially when using a TCP connection, it is **highly recommended** to configure AAP 2.0 to send *Keepalive* messages. This ensures that the connection does not silently drop if no data is exchanged (e.g. because the OS or a NAT/middlebox closes it).

In the case of a subscribing (receiving) client connection, µD3TN will send the keepalive messages, whereas in the case of an RPC (non-subscribing) client connection, the client itself has to care for sending the keepalive messages in the desired interval. In the latter case, *µD3TN will terminate the connection* if no keepalive messages are received.

If you have an RPC (non-subscribing) client connection, a *Keepalive* message can be sent and the response can be examined as follows:

```python
send_client.send(aap2.AAPMessage(keepalive=aap2.Keepalive()))
response = send_client.receive_response()
assert response.response_status == aap2.ResponseStatus.RESPONSE_STATUS_ACK
```

In case of a subscribing connection, if the keepalive feature has been configured, the messages received from µD3TN must be examined by the client. Even if the client only wants to receive ADUs (bundles), it must process keepalive messages. The reception logic can be adapted as follows:

```python
# Instead of recv_client.receive_adu(),
# receive the next AAP 2.0 message of any kind.
msg = recv_client.receive_msg()
if msg.WhichOneof("msg") == "adu":
    adu_msg, bundle_data = recv_client.receive_adu(msg.adu)
    # Process as if recv_client.receive_adu() had been used ...
elif msg.WhichOneof("msg") == "keepalive":
    # Acknowledge keepalive message.
    recv_client.send_response_status(aap2.ResponseStatus.RESPONSE_STATUS_ACK)
else:
    # Error - unsupported message format received!
    raise RuntimeError
```

## Controlling Forwarding Decisions (Building a BDM)

AAP 2.0 can also be used to control the forwarding decisions of µD3TN, given appropriate authorization is requested (and granted) upon configuration. For details on that, the provided [Static BDM](https://gitlab.com/d3tn/ud3tn/-/blob/master/python-ud3tn-utils/ud3tn_utils/aap2/bin/aap2_bdm_static.py?ref_type=heads) can be consulted as an example, and the [AAP 2.0 protocol description](../aap20.md), as well as the [Protobuf documentation](../../references/protobuf/#componentsaap2aap2proto) should be used as references for development.

## Common Errors

- `ConnectionRefusedError: [Errno 111] Connection refused` during connection establishment (when entering `with` block): Check that the µD3TN instance is running and listens to the exact socket address or port provided. Check that the appropriate client variant (`Unix*` or `TCP*`) is used. µD3TN will print the socket address it is listening on in the log as follows: `[INFO] AAP2Agent: Listening on <address>` (launch with `-L 3` in release builds)
- `AAP2CommunicationError: Did not receive AAP 2.0 magic number 0x2F, but: 0x17` during connection establishment (when entering `with` block): You tried to connect to the AAP (**v1**) socket using the AAP 2.0 client. Check the address/path provided to both µD3TN and the Python client. Note that µD3TN has command line arguments for both [AAP (v1)](../ud3tn_aap.md) and [AAP 2.0](../aap20.md), such as `--aap-socket` and `--aap2-socket`.
- `AAP2OperationFailed: The server returned an invalid response status: 12` on configuration: µD3TN denied authorization for the requested configuration. In most cases, this means that the wrong secret was specified. Note that, in the default configuration, some endpoints are reserved internally by µD3TN: `config`, `echo`, and `sqlite` if µD3TN uses a `dtn`-scheme EID; and `9000`, `9002`, and `9003` if µD3TN uses an `ipn`-scheme EID.
- `AAP2OperationFailed: The server returned an invalid response status: 10` on configuration: An invalid parameter was specified. Oftentimes that means that the requested agent identifier contains invalid characters, e.g., if a non-numeric agent identifier was requested but µD3TN runs with an `ipn`-scheme EID.
- `AAP2ServerDisconnected: Server disconnected on 'recv()'` when receiving data: µD3TN has closed your client connection. This may happen, e.g., if you forgot to confirm the reception of a previous bundle or when µD3TN has shut down in the meantime.

## Full Example Sending and Receiving a Bundle

The following code has been tested to work with µD3TN in its default configuration. Do not forget to run it with the `ud3tn-utils` Python dependency installed, e.g., in the [virtual environment](../python-venv.md) created for µD3TN's Python tools.

<!-- TODO: Embed as external file and test in CI! -->

```python
from ud3tn_utils import aap2

# Specify where µD3TN should be reachable (using the default value).
socket_address = "ud3tn.aap2.socket"

# Create two clients, one for sending and one for receiving bundles,
# with the default POSIX IPC socket type and the provided location.
send_client = aap2.AAP2UnixClient(address=socket_address)
recv_client = aap2.AAP2UnixClient(address=socket_address)

with send_client, recv_client:
    # Register an agent for sending, assigning a random agent ID and secret.
    # See the above sections on how to assign a custom agent ID or secret.
    secret = send_client.configure()
    # Register an agent for receiving (subscribing to bundles).
    recv_client.configure(subscribe=True)
    # Set some binary payload
    payload = b"Hello, world!"
    # Send a bundle to the receiver
    send_client.send_adu(
        aap2.BundleADU(
            # We obtain the receiver's EID here.
            dst_eid=recv_client.eid,
            payload_length=len(payload),
        ),
        payload,
    )
    # Check that sending the ADU went well.
    response = send_client.receive_response()
    assert response.response_status == aap2.ResponseStatus.RESPONSE_STATUS_SUCCESS
    # Receive the ADU.
    adu_msg, recv_payload = recv_client.receive_adu()
    # Print and check what we received.
    print(f"Received ADU from {adu_msg.src_eid}: {payload}")
    assert adu_msg.src_eid == send_client.eid
    assert recv_payload == payload
    # Tell µD3TN that receiving the ADU went well.
    recv_client.send_response_status(aap2.ResponseStatus.RESPONSE_STATUS_SUCCESS)
```

## Full Example Sending and Receiving a Bundle (`asyncio`)

To send and receive concurrently using synchronous client implementations, multiple threads have to be used. This can quickly increase complexity and introduce synchronization issues. An alternative is to use `asyncio`, for which the `aap2` module also provides client implementations.

The following asynchronous code is equivalent to the example above:

<!-- TODO: Embed as external file and test in CI! -->

```python
import asyncio
from ud3tn_utils import aap2

# Specify where µD3TN should be reachable (using the default value).
socket_address = "ud3tn.aap2.socket"

# Create two clients, one for sending and one for receiving bundles,
# with the default POSIX IPC socket type and the provided location.
send_client = aap2.AAP2AsyncUnixClient(address=socket_address)
recv_client = aap2.AAP2AsyncUnixClient(address=socket_address)


async def main(send_client, recv_client):
    async with send_client, recv_client:
        # Register an agent for sending, assigning a random agent ID and secret.
        # See the above sections on how to assign a custom agent ID or secret.
        secret = await send_client.configure()
        # Register an agent for receiving (subscribing to bundles).
        await recv_client.configure(subscribe=True)
        await send_and_receive(send_client, recv_client)


async def send_and_receive(send_client, recv_client):
    # Set some binary payload
    payload = b"Hello, world!"
    # Send a bundle to the receiver
    await send_client.send_adu(
        aap2.BundleADU(
            # We obtain the receiver's EID here.
            dst_eid=recv_client.eid,
            payload_length=len(payload),
        ),
        payload,
    )
    # Check that sending the ADU went well.
    response = await send_client.receive_response()
    assert response.response_status == aap2.ResponseStatus.RESPONSE_STATUS_SUCCESS
    # Receive the ADU.
    adu_msg, recv_payload = await recv_client.receive_adu()
    # Print and check what we received.
    print(f"Received ADU from {adu_msg.src_eid}: {payload}")
    assert adu_msg.src_eid == send_client.eid
    assert recv_payload == payload
    # Tell µD3TN that receiving the ADU went well.
    await recv_client.send_response_status(
        aap2.ResponseStatus.RESPONSE_STATUS_SUCCESS
    )


asyncio.run(main(send_client, recv_client))
```
