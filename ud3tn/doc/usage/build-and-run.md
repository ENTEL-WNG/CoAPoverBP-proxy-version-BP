# Build and Run µD3TN

## Obtaining a Copy

- We provide pre-compiled Docker images in the [GitLab Docker registry](https://gitlab.com/d3tn/ud3tn-docker-images/container_registry). Refer to <https://gitlab.com/d3tn/ud3tn-docker-images/> for more information.
- For µD3TN releases, please visit [the GitLab releases page](https://gitlab.com/d3tn/ud3tn/-/releases).
- A recent development version of µD3TN can be obtained by cloning the [Git repository](https://gitlab.com/d3tn/ud3tn) (do not forget to use `--recursive` to initialize the submodules).
- The `CHANGELOG`, detailing updates and changes made to µD3TN over time, is available [as part of the Git repository](https://gitlab.com/d3tn/ud3tn/-/blob/master/CHANGELOG).

## Build µD3TN

!!! note "If you obtained the code via Git"

    The µD3TN project uses Git submodules to manage some code dependencies.
    Use the `--recursive` option if you `git clone` the project or run `git submodule init && git submodule update` at a later point in time.

### POSIX-compliant operating systems

1. Install or unpack the build toolchain

    - Install GNU `make`, `gcc` and `binutils`.
    - Install the `sqlite` development package (including `sqlite3.h` and `libsqlite3.so`).
    - For building with Clang, additionally install a recent version of `clang` and `llvm`.

2. Configure the local build toolchain in `config.mk` (**optional for most systems**)

    - Copy `config.mk.example` to `config.mk`.
    - Adjust `TOOLCHAIN` if you want to build with Clang.
    - Adjust `TOOLCHAIN_POSIX` if your toolchain installation is not included in your `$PATH`

3. Run `make run-posix` to build and execute µD3TN on your local machine.

    - You can find the µD3TN binary file in `build/posix/ud3tn`. To just build it, you can also run `make posix` or `make` (the latter building the library files as well).
    - Note that on some systems, such as BSD flavors, you may need to explicitly call GNU Make using the `gmake` command. In this case, just substitute all calls to `make` in the documentation by `gmake`.
    - Some build-time options (e.g., building with sanitizers) can be easily specified as arguments to `make`. See `config.mk.example` for the values you can specify. Example: `make sanitize=yes`
    - You can specify the `-j<threads>` option to parallelize the build, e.g., `-j8` to run eight threads in parallel.

### Release builds

To build an optimized binary with release settings, specify `type=release optimize=yes` when executing the `make` command.

```sh
make posix -j8 type=release optimize=yes
```

!!! warning "Clean the build directory"

    Do not forget to remove previously-built binaries and object files with `make clean` if there are any.

!!! warning "Release builds require an administrative secret"

    When running a release binary, you must set an *administrative secret* for protecting the AAP 2.0 endpoint. Only clients specifying this secret can perform privileged actions and change the system configuration (e.g., contacts).

    Provide the secret in an environment variable when launching µD3TN:

    ```sh
    AAP2_SECRET=my-very-secure-ud3tn-admin-secret build/posix/ud3tn --bdm-secret-var AAP2_SECRET
    ```

!!! note "Log output in release builds"

    There is no log output in release builds by default. You can increase the log level to `INFO` with `--log-level 3`. `DEBUG` log messages are removed by the compiler in release builds, thus, log level `4` is not available.

### Nix-based build and development

1. [Install the Nix package manager](https://nixos.org/download)
2. [Enable flake support](https://nixos.wiki/wiki/Flakes)

     - *Temporary:* Add `--experimental-features 'nix-command flakes'` when using any Nix commands
     - *Permanent:* Add `experimental-features = nix-command flakes` to `~/.config/nix/nix.conf` or `/etc/nix/nix.conf`

Most common Nix commands are:

- Build & run µD3TN:

    ```sh
    AAP2_SECRET=my-very-secure-ud3tn-admin-secret nix run '.?submodules=1#' -- --bdm-secret-var AAP2_SECRET
    ```

    Note that Nix by default creates optimized release builds of µD3TN. Thus, the above mentioned remarks on release builds apply and an AAP 2.0 secret has to be specified.

- Build individual packages:

    ```sh
    nix build '.?submodules=1#ud3tn'
    nix build '.?submodules=1#pyd3tn'
    nix build '.?submodules=1#python-ud3tn-utils'
    ```

- Load a development environment with all packages and dependencies:

    ```sh
    nix develop '.?submodules=1'
    ```

    After the development environment has been activated, all development dependencies are fulfilled in order to be able to execute all other described debug and build commands.

### Library

Beside the µD3TN daemon binary, two types of library can be built using `make posix-lib` or `make`:

- `build/posix/libud3tn.so`: a dynamic library (shared object) containing all but the daemon functions.
- `build/posix/libud3tn.a`: a _thin_ static library providing the same functionality. This only _references_ the `component.a` files in the `build` directory and is intended to statically link µD3TN into other projects. The preferred way to do this is to include µD3TN as part of your project's source tree (e.g. using `git subtree` or `git submodule`).

### Build the Protobuf headers

For changes to the Protobuf definitions for AAP 2.0 and the storage agent, you may want to re-generate the corresponding C headers and Python modules. This can be done via:

```sh
# For AAP 2.0
make aap2-proto-headers
# For the storage agent
make storage-agent-proto-headers
```

The preferred way to run these commands is from within a Nix shell, e.g., launched using the aforementioned command `nix develop '.?submodules=1'`.
Note that the used `protoc` and `python-protobuf` versions need to be compatible, which is hard to ensure if both are managed by different package managers (`protoc` is typically installed using the system's package manager, while `python-protobuf` is installed in the virtual environment using `pip`). Nix takes care of this for us.

## Run µD3TN

A comprehensive step-by-step tutorial for Linux and POSIX systems is included [in the documentation](../posix_quick_start_guide.md). It covers a complete scenario in which two µD3TN instances create a small two-node DTN and external applications leverage the latter to exchange data.

### Start a µD3TN node

For simple setups with just a single node, µD3TN is ready to use with its default settings. For advanced use, the CLI offers at lot of flexibility:

```
Mandatory arguments to long options are mandatory for short options, too.

  -a, --aap-host HOST         IP / hostname of the application agent service (may be insecure!)
  -A, --aap2-host HOST        IP / hostname of the AAP 2.0 service (may be insecure!)
  -b, --bp-version 6|7        bundle protocol version of bundles created via AAP
  -c, --cla CLA_OPTIONS       configure the CLA subsystem according to the
                              syntax documented in the man page
  -d, --external-dispatch     do not load the internal minimal router, allow for using an AAP 2.0 BDM
  -e, --node-id NODE_ID       local node identifier (referring to the administrative endpoint)
  -h, --help                  print this text and exit
  -l, --lifetime SECONDS      lifetime of bundles created via AAP
  -L, --log-level             higher or lower log level [1, 2, 3, 4] specifies more or less detailed output
  -m, --max-bundle-size BYTES bundle fragmentation threshold
  -p, --aap-port PORT         port number of the application agent service (may be insecure!)
  -P, --aap2-port PORT        port number of the AAP 2.0 service (may be insecure!)
  -r, --status-reports        enable status reporting
  -R, --allow-remote-config   allow configuration via bundles received from CLAs (insecure!)
  -s, --aap-socket PATH       path to the UNIX domain socket of the application agent service
  -S, --aap2-socket PATH      path to the UNIX domain socket of the experimental AAP 2.0 service
  -u, --usage                 print usage summary and exit
  -x, --bdm-secret-var VAR    restrict AAP 2.0 BDM functions to clients providing the secret in the
                              given environment variable

Default invocation: ud3tn \
  -b 7 \
  -c "sqlite:file::memory:?cache=shared;tcpclv3:*,4556;smtcp:*,4222,false;mtcp:*,4224" \
  -e dtn://ud3tn.dtn/ \
  -l 86400 \
  -L 3 \
  -m 0 \
  -s $PWD/ud3tn.socket
  -S $PWD/ud3tn.aap2.socket
```

<!-- TODO: Link Manpage in mkDocs #235 -->

The AAP and AAP 2.0 interfaces can use either a UNIX domain socket (`-s` option for AAP, `-S` option for AAP 2.0) or bind to a TCP address (`-a` and `-p` options for AAP, `-A` and `-P` options for AAP 2.0).
Examples for `CLA_OPTIONS` are documented in the [man page](../ud3tn.1),
which can be viewed in a terminal from within the project directory with `man --local-file doc/ud3tn.1`.
Default arguments and internal settings such as storage, routing, and connection parameters can be adjusted by providing a customized `config.mk` file (see the provided `config.mk.example` file).

### Configure contacts with other µD3TN / BP nodes

In the default configuration[^routing-note], µD3TN performs its bundle forwarding decisions based on _contacts_, which are associated with a specific bundle _node_. Each instance accepts bundles addressed to `dtn://<ud3tn-node-name>/config` or `ipn:<ud3tn-node-number>.9000` (by default, only via AAP or AAP 2.0) and parses them according to the [documented specification](../contacts_data_format.md). To sum it up, the interface can be used to configure:

- which next-hop bundle nodes are available,
- the CLA address through which each next-hop node can be contacted,
- start and end time of contacts with the node (optional),
- data rate during each contact (optional), and
- which other nodes the bundle node can reach in general and during a specific contact (list of EIDs, optional).

This repository includes convenient python tools that can be used after [preparing the python environment](../python-venv.md) to configure contacts (`aap2-config`).
See the [**Quick Start Guide**](../posix_quick_start_guide.md) for some hands-on examples.

[^routing-note]: µD3TN can use other DTN routing and forwarding algorithms than the default contact-based next hop selection algorithm. For this purpose, after starting the µD3TN daemon with the `--external-dispatch` option, custom *Bundle Dispatcher Modules (BDMs)* can be connected via AAP 2.0. See the [corresponding section in the Quick Start Guide](../posix_quick_start_guide.md#extended-usage-aap-20-forwarding-services-bdms).

### Leverage AAP to make applications delay and disruption tolerant

Once a µD3TN enabled DTN network has been created, applications can leverage the Application Agent Protocol (AAP) to interact with it. Applications typically use AAP to:

- register themselves at a µD3TN instance with a local identifier,
- inject bundles (hand over a payload and a destination EID to µD3TN, µD3TN then creates a corresponding bundle and tries to forward / deliver it), and
- listen for application data addressed to their identifier.

µD3TN implements two versions of AAP:

- AAP (v1): a [simple binary protocol](../ud3tn_aap.md), which supports only the basic operations listed above. There are dedicated python scripts using AAP for various tasks provided in the [`python-ud3tn-utils` module](../development/python_ud3tn_utils.md). Python bindings for AAP are also available in the [`ud3tn-utils`](https://pypi.org/project/ud3tn-utils/) Python package.
- AAP 2.0: the next generation application protocol based on Protobuf, with more control over bundle metadata plus experimental support for controlling bundle forwarding decisions and links to other nodes. Please refer to the [AAP 2.0 Overview](../aap20.md) and the [AAP 2.0 Protobuf Definition](../../references/protobuf/#componentsaap2aap2proto) for more details.
