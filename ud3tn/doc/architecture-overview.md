# µD3TN Architecture Overview

This page provides a concise overview and explanation of important design aspects of µD3TN.

## Summary

µD3TN is a DTN Bundle Protocol (BP, [v6](https://www.rfc-editor.org/rfc/rfc5050) and [v7](https://www.rfc-editor.org/rfc/rfc9171.html)) implementation with the following core features:

- implemented in the C programming language (C99-compatible)
- single process, multi-threaded
- low resource usage, suitable for embedded systems
- loose internal coupling, interaction through queues (message passing)
- flexible application and routing interface (AAP 2.0)

The high-level architecture of µD3TN is depicted in the following figure.

![µD3TN Architecture Overview](./image-material/ud3tn-0.14-architecture.drawio.svg)

The most important aspects are:

- The core of the µD3TN Bundle Protocol Agent (*BPA Core*) is a **"Bundle Multiplexer"** that keeps **no persistent state**.
- The **Forwarding Information Base (FIB)** reflects only the *current* connectivity of the node and resolves the next BP hop to a convergence-layer (lower-layer) address.
- Applications can interact with µD3TN using **a flexible socket interface** based on the [**Application Agent Protocol 2.0 (AAP 2.0)**](./aap20.md).
- **Bundle forwarding decisions** and outgoing network links are controlled through the same application interface that is used for transferring bundles (AAP 2.0), enabling the use of *Bundle Dispatcher Modules (BDMs)*.
- **Persistent storage** is provided via a dedicated CLA that, conceptually, represents an own DTN node persistently storing bundles.

For an overview on the development of µD3TN's architecture plus further background on the design decisions made, see the [STINT 2024 paper](https://arxiv.org/abs/2407.17166).

## Core Design Goals

The overall design of µD3TN is based on three central goals:

- **Integrability**: µD3TN should be *easy to integrate* into a variety of systems, including embedded platforms such as microcontrollers.
- **Adaptability**: µD3TN should be *adaptable to different use cases and networking scenarios* that apply different lower-layer protocols, routing techniques, application setups, and governance models.
    - Fully deterministic use cases with central contact planning and optimized single-copy routing should be supported, as well as opportunistic deployments with dynamic node discovery and replication-based forwarding.
    - Both production-grade space networks with high reliability requirements and temporary research/testing deployments should be first-class citizens.
    - Adapting µD3TN should never require forks of the codebase. Extensive compile-time (re)configuration should only be required in exceptional use cases.
- **Efficiency**: µD3TN should make efficient use of the available system resources. Specifically, it should not require keeping more than one copy of a bundle's payload in memory at any time (even during parsing/serialization).

## Application Agent Protocol (AAP / AAP 2.0)

AAP provides µD3TN's application interface, which, most importantly, allows for sending and receiving bundles. In current µD3TN releases, two versions of AAP are available:

- [**AAP**](./ud3tn_aap.md) (v1)
- [**AAP 2.0**](./aap20.md)

All following conceptual discussions focus on the most recent version, **AAP 2.0**, which also supports controlling links and bundle forwarding. The older AAP v1 will stay available for some time, but is very limited in functionality and should not be used by newly-developed applications.

AAP 2.0 uses a plain POSIX IPC (default) or TCP socket and exchanges data encoded via [Protocol Buffers](https://protobuf.dev/). An extended description of AAP 2.0 is available in a [dedicated document](./aap20.md) and the documented Protobuf definition of AAP 2.0 messages and responses can also be found in the [Protobuf section of the references](../references/protobuf/#componentsaap2aap2proto).

In summary, AAP 2.0 enables:

- **Sending and receiving bundles** including various metadata
- **Control and monitoring of CLA links** to adjacent nodes, management of the FIB
- **Execution of bundle forwarding decisions** (determining the list of next hops)

Important design decisions for AAP 2.0 were:

- **Extensibility through Protobuf**: It should be easy to add new functions and fields (e.g., bundle headers or other metadata) in the future. For this reason, Protobuf was chosen as it provides such capabilities out-of-the-box.
- **Decoupling of sending and receiving ends**: The previous version of AAP (v1) only allows a single client connection per registered bundle endpoint, requiring complex logic when a client wants to asynchronously send and receive data. With AAP 2.0, every connection has a fixed control flow. This approach uses two separate connections for sending and receiving, allowing clients to manage them easily with different threads or asynchronous tasks. The same endpoint may even be served by multiple different processes.
- **Plain Protobuf for compatibility with embedded systems**: µD3TN itself should be portable and adaptable to low-resource embedded use cases. Because of that, the application interface should not require complex infrastructure or extensive libraries. The implementation in µD3TN is, thus, based on [Nanopb](https://jpa.kapsi.fi/nanopb/) and does not use mechanisms like gRPC or TLS.
- **Basic security via shared secrets**: For protecting concurrent access to endpoints as well as the modification of administrative information in µD3TN, a simple shared-secret mechanism is introduced. This prevents the need for complex, cryptographic authentication and authorization schemes.

!!! note "More Information"

    See the [AAP 2.0 documentation](./aap20.md) for further details.

## Generic Bundle Forwarding Interface (GBFI)

Through AAP 2.0, µD3TN allows the attachment of external applications that control its links to other nodes and the forwarding decision(s) for each bundle being processed. Such an application is called a *Bundle Dispatcher Module (BDM)*. Field tests of early prototypes showed that this approach is extremely valuable, because:

- It enables exchange of routing algorithms at runtime, whether opportunistic or deterministic, and facilitates adaptation to various networking use cases.
- It enables fast and easy prototyping.
- It simplifies the integration of multiple dedicated services for routing, link management, discovery, monitoring, logging, and more.

The BDM receives updates about bundle status changes (e.g., received, transmitted, transmission failed) and about the presence of links. Based on either 1) these events or 2) external triggers, such as out-of-band discovery beacons or a timer indicating the start and end of expected transmission intervals (contacts), it can take action by updating µD3TN's FIB or specifying the next-hop node(s) for a given bundle.

!!! note "More Information"

    The first iteration of this concept was documented extensively in a [research article](https://arxiv.org/abs/2209.05039).

## Persistent Storage as a CLA

For reducing complexity and keeping µD3TN itself free from persistent state, no persistent storage functionality is included as part of its core. The central idea is to perform a *delegation* of bundle storage to dedicated nodes, making "storage" a *special next hop* in terms of the forwarding decision.

For offering persistent storage, a *storage CLA* has been implemented. Conceptionally, this CLA encapsulates a minimal bundle node that can only interact with one other node (the µD3TN BPA) and stores all received bundles persistently. The current implementation is based on the SQLite database engine, which can use either an in-memory database (default, for testing purposes) or file-based storage.

The implementation as a CLA has the added benefit that it can leverage the bundle serialization/parsing infrastructure, provided by µD3TN's CLA subsystem, to efficiently load and store a serialized bundle without copying the associated blocks of memory multiple times.

The storage CLA provides an interface through a special bundle endpoint for listing, recalling ("pushing"), and deleting bundles. Access to the interface is secured using a passed-through AAP 2.0 authorization flag, i.e., all clients have to be authorized via AAP 2.0 to perform administrative actions.

!!! note "More Information"

    The usage of the SQLite storage system is documented on a [dedicated page](./sqlite-storage.md).

## Streaming I/O

The parsers and serializers implemented in µD3TN's CLA subsystem operate in a "streaming" fashion, maintaining only minimal internal buffers and aiming to write data directly to its final destination. For example, when any of the bundle parsers reads the bundle payload, it directly invokes the "receive" function for the respective convergence layer to read the binary data into the pre-allocated bundle payload buffer. This mode of operation is different from many other Bundle Protocol v7 implementations, which typically first invoke a generic CBOR parser that copies all data into an intermediary data structure, and then construct the bundle from there in a second step. In contrast, µD3TN's serializers operate in a similar but reversed manner, eliminating the need to retain a temporary, serialized copy of the bundle's wire representation in memory.

## Bundle-in-Bundle Encapsulation (BIBE)

µD3TN implements [BIBE](https://datatracker.ietf.org/doc/html/draft-ietf-dtn-bibect-04), however, with a different primary focus than the linked IETF draft: instead of providing a "Custody Transfer" capability using BIBE (that is not implemented in µD3TN), it enables "stacking" different DTN networks on top of each other. Each network layer is served by a different set of Bundle Protocol Agents, connecting to each other through well-defined interfaces: the CLA interface if they are on the same layer, and AAP if they are on different layers. This way, topological information is completely isolated between layers and does not have to be exchanged among them.

!!! note "More Information"

    More details including a usage example for BIBE are provided on the [corresponding page](https://gitlab.com/d3tn/ud3tn/-/blob/master/doc/Bundle-in-Bundle%20Encapsulation_(BIBE).md?ref_type=heads).

<!-- TODO: Thread-based architecture: how do threads interact; how do the components map to threads #234 -->
