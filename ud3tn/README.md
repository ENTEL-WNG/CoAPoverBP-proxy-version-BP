## About

µD3TN (pronounced "Micro-Dee-Tee-En") is a free, lean, and space-tested DTN protocol implementation running on POSIX (plus Linux ;-)).
Though µD3TN is easily portable to further platforms, we currently only support POSIX-compliant systems (former versions also included support for STM32/FreeRTOS platforms).

µD3TN's **documentation** can be found here: [**https://d3tn.gitlab.io/ud3tn/**](https://d3tn.gitlab.io/ud3tn/)

For quickly getting up to speed, check out our [**Quick Start Guide**](https://d3tn.gitlab.io/ud3tn/posix_quick_start_guide/)!

A general introduction of µD3TN is available on its project web site at https://d3tn.com/ud3tn.html and in [our video series on YouTube](https://www.youtube.com/watch?v=ETs_BgazRJI&list=PLED8xrzySss-B2966X98dwLLb1BJQu6Ua).

### What does µD3TN provide?

µD3TN currently implements:

- Bundle Protocol version 6 ([RFC 5050](https://datatracker.ietf.org/doc/html/rfc5050)),
- Bundle Protocol version 7 ([RFC 9171](https://datatracker.ietf.org/doc/html/rfc9171)),
- several Bundle Protocol convergence layers, such as:
  - MTCP ([draft version 0](https://datatracker.ietf.org/doc/html/draft-ietf-dtn-mtcpcl-00)),
  - TCPCLv3 ([RFC 7242](https://datatracker.ietf.org/doc/html/rfc7242)),
  - CCSDS Space Packet Protocol ([SPP](https://public.ccsds.org/Pubs/133x0b2e1.pdf)),
  - BIBE ([draft version 3](https://datatracker.ietf.org/doc/html/draft-ietf-dtn-bibect-03), see [doc/Bundle-in-Bundle Encapsulation_(BIBE).md](doc/Bundle-in-Bundle&#32;Encapsulation_(BIBE).md)),
- a persistent storage backend based on SQLite (see [doc/sqlite-storage.md](doc/sqlite-storage.md)).

## Pre-compiled binaries

We provide pre-compiled Docker images in the [GitLab Docker registry](https://gitlab.com/d3tn/ud3tn-docker-images/container_registry). Refer to <https://gitlab.com/d3tn/ud3tn-docker-images/> for more information.

## Usage

A comprehensive step-by-step tutorial for Linux and POSIX systems is included [in the documentation](https://d3tn.gitlab.io/ud3tn/posix_quick_start_guide/). It covers a complete scenario in which two µD3TN instances create a small two-node DTN and external applications leverage the latter to exchange data.

The build process and runtime options of µD3TN are outlined on a dedicated [**documentation page**](https://d3tn.gitlab.io/ud3tn/usage/build-and-run/)

**Dependencies:**

- GNU `make`, `gcc`, `binutils`
- the `sqlite` development package

**Obtain the code:**

```sh
# Do not forget --recursive to initialize all Git submodules!
git clone --recursive https://gitlab.com/d3tn/ud3tn.git
```

**Build and run without Nix:**

```sh
# Build uD3TN with 8 threads in parallel (default options).
make posix -j8
# Execute the binary directly with the -h argument to get usage instructions.
build/posix/ud3tn -h
# Run uD3TN with the default options.
# Can also be used without the previous build command and will build missing parts.
make run-posix
```

**Optimized release build:**

```sh
# Remove binaries and object files built previously when changing the build configuration.
make clean
# Release build
make posix type=release optimize=yes
```

**Build with [Nix](https://nixos.wiki/wiki/Nix_package_manager):**

```sh
nix --experimental-features 'nix-command flakes' build '.?submodules=1#ud3tn'
```

**Nix development environment:**

```sh
nix --experimental-features 'nix-command flakes' develop '.?submodules=1'
```

For more details, see the [documentation page on building and running µD3TN](https://d3tn.gitlab.io/ud3tn/usage/build-and-run/).

### Test

The µD3TN development is accompanied by extensive testing. For this purpose, you should install `gdb` and a recent version of Python 3 (>= 3.8), plus the, `venv` and `pip` packages for your Python version. Our test suite covering static analysis, unit, and integration tests is documented in the [Testing Guide](https://d3tn.gitlab.io/ud3tn/testing/).

### Contribute

Contributions in any form (e.g., bug reports, feature, or merge requests) are very welcome! Please have a look at [CONTRIBUTING.md](CONTRIBUTING.md) first for a smooth experience. The project structure is organized as follows:

```
.
├── components             C source code
├── doc                    documentation
├── dockerfiles            Templates for creating Docker images
├── external               3rd party source code
├── generated              generated source code (e.g. for Protobuf)
├── include                C header files
├── mk                     make scripts
├── nix                    nix derivations, modules and tests
├── pyd3tn                 Python implementation of several DTN protocols
├── python-ud3tn-utils     Python bindings for AAP
├── test                   various test routines
└── tools                  various utility scripts
```

The entry point is implemented in [`components/daemon/main.c`](components/daemon/main.c).

## License

This work is licensed as a whole under the GNU Affero General Public License v3.0, with some parts and components being licensed under the BSD 3-Clause, Apache 2.0, MIT, zLib, and GPL v2.0 licenses.
This licensing scheme is applied since early 2024, after the release of version v0.13.0 under the Apache 2 and BSD 3-Clause licenses.

All code files, except those under the `external/` directory tree, contain an [SPDX](https://spdx.dev/) license identifier at the top, to indicate the license that applies to the specific file.
The external libraries shipped with µD3TN and contained in `external/` are subject to their own licenses, documented in [`LICENSE-3RD-PARTY.txt`](external/LICENSE-3RD-PARTY.txt).

`SPDX-License-Identifier: AGPL-3.0-or-later`

## Ecosystem

- [`ud3tn-utils`](https://pypi.org/project/ud3tn-utils/) is a Python package that provides bindings for µD3TN's [Application Agent Protocol](doc/ud3tn_aap.md) and [Application Agent Protocol v2](doc/aap20.md).
- [`aap.lua`](tools/aap.lua) is a Wireshark dissector for µD3TN's [Application Agent Protocol](doc/ud3tn_aap.md). It can be installed by copying it into one of the Lua script folders listed in the Wireshark GUI at `Help > About Wireshark > Folders`.
- [`pyD3TN`](https://pypi.org/project/pyD3TN/) is a Python package that provides implementations of several DTN related RFCs.
- [`aiodtnsim`](https://gitlab.com/d3tn/aiodtnsim) is a minimal framework for performing DTN simulations based on Python 3.7 and asyncio.
- [`dtn-tvg-util`](https://gitlab.com/d3tn/dtn-tvg-util) is a Python package simplifying the analysis and simulation of DTNs based on time-varying network graphs.

## See also

- [µD3TN's web documentation](https://d3tn.gitlab.io/ud3tn/)
- [RFC 4838](https://datatracker.ietf.org/doc/html/rfc4838) for a general introduction about DTN networks.
- [ION](https://github.com/nasa-jpl/ION-DTN): NASA's (JPL) bundle protocol implementation that has been successfully demonstrated to be interoperable with µD3TN.
- [HDTN](https://github.com/nasa/HDTN): NASA's performance optimized implementation of the DTN standard.
- [DTN7-rs](https://github.com/dtn7/dtn7-rs): Rust implementation of a disruption-tolerant networking (DTN) daemon for the Bundle Protocol version 7 - RFC9171.
