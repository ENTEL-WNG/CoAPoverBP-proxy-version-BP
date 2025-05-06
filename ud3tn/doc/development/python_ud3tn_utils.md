# python-uD3TN-utils

The Python package uD3TN-utils is a utility library to simplify the interaction
with the uD3TN daemon within python applications.

The included `AAPClient` and `AAP2Client` classes enable user-friendly communication with the uD3TN
daemon via local or remote sockets using the [Application Agent Protocol (AAP)](../ud3tn_aap.md) and [AAP 2.0](../aap20.md).
Besides sending and receiving bundles, it is also possible to change the configuration of the uD3TN daemon's bundle forwarder via AAP or AAP 2.0 messages.
There is a [how-to guide](./how-to-build-an-aap20-client-in-python.md) explaining how to build a Python AAP 2.0 client using the Python module.

In addition to the AAP client interface, the package provides several easy-to-use utilities for interacting with a running µD3TN instance. Usage examples can be found, e.g., in the [Quick Start Guide](../posix_quick_start_guide.md).
