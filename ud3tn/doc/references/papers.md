# Papers

- ["The Architectural Refinement of μD3TN: Toward a Software-Defined DTN Protocol Stack"](https://arxiv.org/abs/2407.17166)

!!! Abstract "Abstract - The Architectural Refinement of μD3TN: Toward a Software-Defined DTN Protocol Stack"

    This paper provides a comprehensive overview of the uD3TN project's development, detailing its transformation into a flexible and modular software implementation of the Delay-/Disruption-Tolerant Networking (DTN) Bundle Protocol. Originating from uPCN, designed for microcontrollers, uD3TN has undergone significant architectural refinement to increase flexibility, compatibility, and performance across various DTN applications. Key developments include achieving platform independence, supporting multiple Bundle Protocol versions concurrently, introducing abstract Convergence Layer Adapter (CLA) interfaces, and developing the so called Application Agent Protocol (AAP) for interaction with the application layer. Additional enhancements, informed by field tests, include Bundle-in-Bundle Encapsulation and exploring a port to the Rust programming language, indicating the project's ongoing adaptation to practical needs. The paper also introduces the Generic Bundle Forwarding Interface and AAPv2, showcasing the latest innovations in the project. Moreover, it provides a comparison of uD3TN's architecture with the Interplanetary Overlay Network (ION) protocol stack, highlighting some general architectural principles at the foundation of DTN protocol implementations.

- ["A Generic Bundle Forwarding Interface"](https://arxiv.org/abs/2209.05039)

!!! Abstract "Abstract - A Generic Bundle Forwarding Interface"

    A generic interface for determining the next hop(s) for a DTN bundle is a valuable contribution to DTN research and development as it decouples the topology-independent elements of bundle processing from the topology-dependent forwarding decision. We introduce a concept that greatly increases flexibility regarding the evaluation and deployment of DTN forwarding and routing techniques and facilitates the development of software stacks applicable to heterogeneous topologies.
