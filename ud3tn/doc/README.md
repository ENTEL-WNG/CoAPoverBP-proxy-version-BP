# Documentation

## MkDocs

An HTML version of the μD3TN documentation is created via [MkDocs](https://www.mkdocs.org/). All files stored in the MkDocs directory are taken into account. The MkDocs configuration can be changed in the `mkdocs.yaml` file. To add further pages to the navigation, they must be added to the `nav` section.

### Prepare

- w/o nix

  - Install [protoc](https://grpc.io/docs/protoc-installation/)
  - Install [protoc-gen-doc](https://github.com/pseudomuto/protoc-gen-doc) via your package manager, `go install` or another preferred method

    To install it as an unprivileged user in a subdirectory of the work tree:

    ```sh
    GOBIN=$(pwd)/.gobin/ go install github.com/pseudomuto/protoc-gen-doc/cmd/protoc-gen-doc@latest
    # Then, run all protoc commands as follows:
    PATH=$(pwd)/.gobin/:$PATH protoc ...
    ```
  - Install mkdocs and relevant Python dependencies (preferably use a [virtual environment](https://d3tn.gitlab.io/ud3tn/python-venv/))

    ```sh
    pip install -U mkdocs
    pip install $(mkdocs get-deps)
    ```

- w/ nix (see also [the µD3TN documentation on it](https://d3tn.gitlab.io/ud3tn/usage/build-and-run/#nix-based-build-and-development))

  ```sh
  nix develop '.?submodules=1'
  ```

### Develop

```sh
# Create markdown docs from protobuf
protoc \
  --doc_out=doc/references/protobuf \
  --doc_opt=markdown,index.md \
  components/aap2/aap2.proto \
  components/agents/storage/storage_agent.proto
# Run dev-server for live preview of HTML documents (without protobuf)
mkdocs serve
```

### Build

```sh
# w/o nix
make doc

# w/ nix
nix build .#mkdocs-html
```

### Deploy

When changes are made to the master branch, the latest version of the documentation is automatically built and published at https://d3tn.gitlab.io/ud3tn.

## Man Page

There exists also a man page for μD3TN, which can be viewed with

```sh
man --local-file ud3tn.1
```
