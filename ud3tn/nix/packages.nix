# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

{ pkgs, ... }:

let
  version = "0.14.2";
in

rec {
  ud3tn = pkgs.stdenv.mkDerivation {
    pname = "ud3tn";
    inherit version;

    src = pkgs.lib.sourceByRegex ../. [
      "Makefile"
      "^components.*"
      "^external.*"
      "^generated.*"
      "^include.*"
      "^mk.*"
    ];

    buildInputs = [
      pkgs.sqlite
    ];

    buildPhase = ''
      make type=release optimize=yes -j $NIX_BUILD_CORES ud3tn
    '';

    installPhase = ''
      mkdir -p $out/bin
      cp build/posix/ud3tn $out/bin/
    '';
  };

  pyd3tn = with pkgs.python3Packages; buildPythonPackage {
    pname = "pyd3tn";
    inherit version;
    src = ../pyd3tn;
    format = "pyproject";
    nativeBuildInputs = [ setuptools ];
    propagatedBuildInputs = [ cbor2 ];
  };

  python-ud3tn-utils = with pkgs.python3Packages; buildPythonPackage {
    pname = "python-ud3tn-utils";
    inherit version;
    src = ../python-ud3tn-utils;
    format = "pyproject";
    nativeBuildInputs = [ setuptools ];
    propagatedBuildInputs = [ cbor2 protobuf pyd3tn ];
  };

  mkdocs-html = pkgs.stdenv.mkDerivation {
    pname = "mkdocs-html";
    inherit version;

    src = pkgs.lib.sourceByRegex ../. [
      "^doc.*$"
      "^include.*"
      "^components.*"
      "^pyd3tn.*"
      "^python-ud3tn-utils.*"
      "mkdocs.yaml"
    ];

    nativeBuildInputs = with pkgs; [
      doxygen
      protobuf
      protoc-gen-doc
    ] ++ (with python3Packages; [
      mkdocs
      mkdocs-material
      mkdocstrings
      mkdocstrings-python
    ]);

    buildPhase = ''
      mkdir -p doc/references/protobuf
      protoc \
        --doc_out=doc/references/protobuf \
        --doc_opt=doc/references/protobuf/protoc-gen-doc-markdown.tmpl,index.md \
        components/aap2/*.proto \
        components/agents/storage/storage_agent.proto

      mkdocs build --site-dir $out
    '';
  };
}
