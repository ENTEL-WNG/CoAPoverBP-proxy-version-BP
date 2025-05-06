# µD3TN POSIX Quick Start Guide

This document aims to provide some pointers for getting multiple µD3TN instances up and running and quickly sending around some bundles.

## Prerequisites

It is assumed all following commands are launched on a **POSIX** or **Linux** system.
A VM or Docker container will work for this purpose, too.

### Dependencies

Please install all dependencies stated in the [`README.md` file](https://gitlab.com/d3tn/ud3tn/-/blob/master/README.md) for the version you have obtained.
At least an up-to-date **C build toolchain**, **GNU Make**, and the **GNU Binutils** are always needed.
If the SQLite-based persistent storage is not explicitly deactivated (using the `DISABLE_SQLITE_STORAGE` define, see [`config.mk.example`](https://gitlab.com/d3tn/ud3tn/-/blob/master/config.mk.example)), the **SQLite** development package is also required (including `sqlite3.h` and `libsqlite3.so`).
Apart from that, you need an up-to-date version of **Python 3** including **pip** and **venv** for the µD3TN tools and **Git** for working with the code repository.

### Getting the code

Apart from installing the dependencies, you need the µD3TN code. You can pull it from Git as follows (replace `vX.Y.Z` by the [version](https://gitlab.com/d3tn/ud3tn/-/releases) you want to use):

```sh
git clone https://gitlab.com/d3tn/ud3tn.git
cd ud3tn
git checkout vX.Y.Z
git submodule update --init --recursive
```

### Build and run µD3TN

Build µD3TN as follows:

```sh
make posix
```

Given your toolchain is working properly, the command should complete without an error code.

You can now run µD3TN as follows:

```sh
make run-posix
```

This will launch a new instance of µD3TN with the default parameters; you will see some output that µD3TN is starting up. When everything is set up, the daemon will continue running and can be terminated as usual via `SIGINT` (Ctrl+C) or `SIGTERM`.

!!! note

    For modifying µD3TN's command line parameters, you can directly launch the built binary `build/posix/ud3tn`. Some information on the parameters can be retrieved via `-h` and from the [man page](https://gitlab.com/d3tn/ud3tn/-/blob/master/doc/ud3tn.1) via `man -l doc/ud3tn.1`.

!!! note

    In release builds (via `make type=release`), it is *required* to specify an administrative *shared secret* for clients to use when configuring the system via [AAP 2.0](./aap20.md). The secret needs to be passed via an environment variable that is specified using the `-x, --bdm-secret-var` command line argument. The same secret must be provided by all clients (such as `aap2-config`), in the supplied Python tools preferably using the `--secret-var` command line argument. Configuration through AAP v1 is *disabled* in release builds and `aap-config` (note the missing character `2`) will fail, as AAP v1 cannot provide the shared secret.

### Python dependencies

For getting the Python tools working, you have two options:

1. If you have already pulled the code of a specific version it is probably easiest to use the [**development setup**](./python-venv.md). Just run `make virtualenv` in the µD3TN directory and it will create a `.venv` subdirectory containing a full [Python virtual environment](https://www.python.org/dev/peps/pep-0405/) for development. You can activate the virtual environment in your shell of choice via `source .venv/bin/activate`. (Using a custom tool to manage virtual environments in other locations is also possible - to just install all dependencies run `make update-virtualenv`.)

2. If you don't want to create a new virtual environment or have troubles with the development setup, you can use the [**PyPI packages**](https://pypi.org/user/d3tn/). You can, of course, also install them in a virtual environment or use a Python package manager of your choice. Installation via `pip` is performed as follows (replace `vX.Y.Z` by the µD3TN version you are using): `pip install pyd3tn==vX.Y.Z ud3tn-utils==vX.Y.Z`

Everytime we run a `python` command in the following section(s), these dependencies should be available (i.e., if you are using a virtual environment, it should be _activated_).

## Simple two-instance message-passing

In this scenario we want to spin up two µD3TN instances representing two nodes, connect them via the [minimal TCP convergence layer adapter](https://tools.ietf.org/html/draft-ietf-dtn-mtcpcl) (MTCP CLA), and send a [BPv7 bundle](https://datatracker.ietf.org/doc/rfc9171/) from an application connected to one instance to the second instance.

### Start µD3TN instances

We will use two µD3TN instances with two different identifiers for the node (Node IDs; note that a Node ID is also a valid DTN endpoint identifier / EID):
- Instance `A` with Node ID `dtn://a.dtn/`, with MTCP listening on TCP port `4224` and application agent 2 listening on socket `ud3tn-a.aap2.socket`
- Instance `B` with Node ID `dtn://b.dtn/`, with MTCP listening on TCP port `4225` and application agent 2 listening on socket `ud3tn-b.aap2.socket`

#### Step 1

Open a new shell in the µD3TN base directory and launch instance `A` as follows:

```sh
build/posix/ud3tn --node-id dtn://a.dtn/ \
    --aap-port 4242 --aap2-socket ud3tn-a.aap2.socket \
    --cla "mtcp:*,4224"
```

This will tell µD3TN to run with Node ID `dtn://a.dtn/`, generate bundles using version `7` (BPbis / BPv7), use TCP and port `4242` for [AAP](./ud3tn_aap.md) and listen on TCP port `4224` with the MTCP CLA.

!!! note

    You can increase output verbosity ("debug mode") by adding the `-L 4` argument to the `ud3tn` command line. This can be handy when you want to trace the transmission and reception of individual bundles.

#### Step 2

Open _another_ shell and launch instance `B` as follows:

```sh
build/posix/ud3tn --node-id dtn://b.dtn/ \
    --aap-port 4243 --aap2-socket ud3tn-b.aap2.socket \
    --cla "mtcp:*,4225"
```

You see that we have just incremented the port numbers by one to avoid a collision.

Now, both µD3TN instances are up and running. Let's continue with the next step!

### Configure µD3TN instances

µD3TN needs to be _configured_ to know which DTN nodes are reachable during which time intervals (_contacts_) and with which data rates. This configuration occurs at runtime and is not persistent at the moment. Thus, after each start you have to re-configure µD3TN.

Configuration occurs via DTN bundles containing a specific [configuration message format](./contacts_data_format.md) destined for the configuration endpoint ID `<µD3TN Node ID>/config`.
In its default configuration (that can be changed via the `--allow-remote-config` commandline argument), for security reasons, µD3TN only accepts configuration bundles with the administrative authorization flag of AAP 2.0. `aap2-config` will automatically attempt to set this flag. Note that in release builds or if you have set it when launching µD3TN, you need to specify the administrative secret on invocation of `aap2-config`.

To simplify the configuration process, we provide a Python tool for configuration.
Make sure you have activated the Python virtual environment or made available the `pyd3tn` and `ud3tn-utils` packages in another way.

#### Step 3

Open a third shell and configure a contact from `A` to `B`:

```sh
# Enable Python virtual environment first!!
aap2-config --socket ud3tn-a.aap2.socket \
    --schedule 1 3600 100000 \
    dtn://b.dtn/ mtcp:localhost:4225
```

This specifies that `aap2-config` shall connect via AAP2 to `A` on socket `ud3tn-a.aap2.socket` and issue a configuration command to the µD3TN daemon with Node ID `dtn://a.dtn/`.

The contact is defined by the `--schedule` argument: It starts `1` second from now (the time the command has been invoked), runs for `3600` seconds, and has an expected transmission rate of `100000` bytes per second.

The last two arguments specify with which DTN Node ID the specified contact(s) are expected and via which convergence layer address the node is reachable.

See `aap2-config -h` for more information on the command line arguments of the script.

You will see that instance `A` will attempt to connect to instance `B`, one second after this command has been issued. Instance `B` will accept the connection.

Note the following:

- Instance `B` will not have a possibility to send bundles to instance `A` — the connectivity is unidirectional as MTCP is a unidirectional CLA. For sending bundles the other way, you need to configure a contact in instance `B` and provide the port over which instance `A` is reachable.

- You may, of course, configure a contact at a later point in time (e.g., by setting the first parameter of `--schedule` to `60` it will start a minute after issuing the command). µD3TN will queue all bundles received for a given node for which it knows about future contacts. This means you always have to configure the contacts first, but they can start at an arbitrary time in the future.

### Attach a receiving application

#### Step 4

For sending bundles over the now-established link, first attach a receiver to instance `B`:

```sh
# Enable Python virtual environment first!!
aap2-receive --socket ud3tn-b.aap2.socket --agentid bundlesink
```

This requests µD3TN instance `B` via AAP on TCP port `4243` to register a new application with agent ID `bundlesink` (the endpoint ID will be `dtn://b.dtn/bundlesink`). If the registration is successful (which it should be), `aap2-receive` will wait for incoming bundles on that socket connection and print their payload to the console.

### Send a bundle

Now we are finally ready to send a bundle!

#### Step 5

Open another terminal and send a new bundle via AAP to instance `A`:

```sh
# Enable Python virtual environment first!!
aap2-send --socket ud3tn-a.aap2.socket \
    dtn://b.dtn/bundlesink \
    'Hello, world!'
```

This asks µD3TN instance `A` via AAP on TCP port `4242` to create and send a new bundle to `dtn://b.dtn/bundlesink` with payload data `Hello, world!`.

You should see the received bundle appear in the output of `aap2-receive`.

## Extended usage: AAP 2.0 forwarding services / BDMs

µD3TN makes it possible to attach external services that control its link establishment and forwarding logic. A service controlling the next-hop forwarding decision for incoming bundles is called a *Bundle Dispatch Module (BDM)*. Some BDMs also control link establishment, as in the first example use case below, while others rely on additional services or manual control for setting up the links.

### Enabling BDM usage when starting µD3TN

For using an external BDM with µD3TN, it needs to be told that the integrated forwarding agent should not be launched. This is accomplished via the `-d, --external-dispatch` command line switch:

```sh
build/posix/ud3tn --external-dispatch
```

### Example case 1: Compatible deterministic first contact forwarding BDM

A Python-based BDM replicating the integrated forwarding mechanism is provided in the `python-ud3tn-utils` module. It can be launched as follows:

```sh
aap2-bdm-ud3tn-routing
```

As soon as µD3TN and the BDM are both up and running, all usual commands and tests introduced above should work. The BDM will automatically control µD3TN's outgoing links based on the configured contacts.

If µD3TN is listening on a socket other than the default, the same `--socket` argument as in other AAP 2.0 tools can be used.

Enabling debug output for tracing bundle transmissions via the BDM is achieved by increasing the log output verbosity twice using `-vv`.

!!! note

    For running µD3TN's integration tests, the `--insecure-config` flag must be specified, which makes the BDM accept configuration from unknown sources, similarly to the `--allow-remote-config` flag of µD3TN itself.

### Example case 2: Manual link control and static forwarding

A second, minimal, BDM is provided, which only performs the forwarding function based on a static table (links have to be established independently of it):

```sh
aap2-bdm-static routing_table.json
```

The file `routing_table.json` should contain a JSON object mapping bundle destination node identifiers (strings) to next-hop node identifiers (also strings).

A minimal example for forwarding all bundles destined for a node `dtn://a.dtn/` to persistent storage, injecting the JSON data via process substitution, looks as follows:

```sh
aap2-bdm-static <(echo '{"dtn://a.dtn/": "dtn:storage"}')
```

As with the other BDM and the µD3TN Python tools, the static forwarder accepts the usual command line arguments for changing parameters such as the AAP 2.0 interface socket.

For controlling the links when using the static BDM, a dedicated service should be used. A simple tool using the AAP 2.0 interface to create and tear down links is provided as well. For example, to manually configure the link from node `A` to node `B` realized through the contact added in step 3 of the initial example in this document, the following command could be used.

```sh
aap2-configure-link dtn://b.dtn/ mtcp:localhost:4225
```

The link can be deleted again by adding the `-d, --delete` flag to that command.

By default, `aap2-configure-link` will set the "direct" flag in µD3TN's FIB, meaning that µD3TN will not consult an attached BDM for bundles destined directly to a next-hop node (i.e. node `dtn://b.dtn` in this example). If this is not intended, the `-i, --indirect` command line flag can be specified.

### Securing the AAP 2.0 interaction

In productive use cases, the AAP 2.0 interface should be secured against untrusted clients in two ways:

1. The AAP 2.0 socket itself should be protected. As no transport security (e.g. TLS) is implemented, AAP 2.0 is currently vulnerable against man-in-the-middle attacks, as long as they are possible. This is why a local POSIX IPC socket connection should be preferred. Moreover, the socket should be protected by assigning file system permissions that are as restrictive as possible.
2. Privileged functions of AAP 2.0 (e.g., controlling the FIB, bundle forwarding decisions, and contact configuration in BDMs) should be secured by setting an administrative shared secret via µD3TN's `-x, --bdm-secret-var` command line argument. This prevents unauthorized clients (that do not provide the shared secret) from performing privileged functions -- such clients may then only send and receive bundles. The secret value must be provided by all AAP 2.0 clients performing privileged functions. In release builds of µD3TN specifying a secret is mandatory.

The straight-forward approach is to:

- build µD3TN with `make type=release`,
- run it as an unprivileged user from a protected directory,
- use the (default) POSIX IPC socket, and
- specify a long, random secret via `--bdm-secret-var`.

### Persistent Storage

Since version v0.14.0, µD3TN supports persistently storing bundles in a SQLite database. This is realized in the implementation using a special "storage CLA" -- see the [corresponding documentation](sqlite-storage.md) for details.

While the integrated minimal routing logic only uses the system memory (also for performance reasons), the persistent storage system is automatically used by the deterministic first contact forwarding BDM (see _example case 1_ above).

By default, the storage CLA is configured such that it uses an in-memory storage adapter. For using a file on disk, launch µD3TN with a filename specified in the `sqlite` CLA configuration:

```sh
build/posix/ud3tn --node-id dtn://a.dtn/ \
    --aap-port 4242 --aap2-socket ud3tn-a.aap2.socket \
    --external-dispatch \
    --cla "sqlite:bundle_storage.sqlite;mtcp:*,4224"
# and do not forget to start the external BDM
aap2-bdm-ud3tn-routing --socket ud3tn-a.aap2.socket
```

You can now configure contacts and send bundles as usual. As long as bundles are sheduled for a future contact, they will be kept in the persistent storage.
If the services are restarted while bundles are still in storage, the bundles have to be re-scheduled _after applying all necessary configuration_ (e.g. scheduling contacts) again.

Recall bundles [as documented for the SQLite storage](sqlite-storage.md):

```sh
aap2-storage-agent --socket ud3tn-a.aap2.socket \
    --storage-agent-eid "dtn://a.dtn/sqlite" \
    push --dest-eid-glob "*"
```

## Closing remarks

Although the setup may seem complex at first, it is overall quite straight-forward to interact with µD3TN instances.
It is recommended to take a look at code of the provided Python tools and libraries, which can serve as a starting point to develop own integrations.

If you have any questions or want to [contribute](./development/contributing.md), just send us a mail via [contact@d3tn.com](mailto:contact@d3tn.com)!
