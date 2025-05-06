# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

{ pkgs, self, ... }:

pkgs.mkShell rec {

  hardeningDisable = [ "all" ];

  # Derive build inputs from custom packages
  inputsFrom = [
    self.packages.${pkgs.system}.pyd3tn
    self.packages.${pkgs.system}.python-ud3tn-utils
    self.packages.${pkgs.system}.ud3tn
    self.packages.${pkgs.system}.mkdocs-html
  ];

  # Prepare a virtual python enviroment for development
  venvDir = "./.venv";
  buildInputs = with pkgs.python3Packages; [ python venvShellHook ];
  postVenvCreation = ''
    unset SOURCE_DATE_EPOCH
    pip install -e ./python-ud3tn-utils
    pip install -e ./pyd3tn
  '';
  postShellHook = "unset SOURCE_DATE_EPOCH";

  packages = with pkgs; [
    clang-tools
    cppcheck
    llvmPackages.libcxxClang
    nixpkgs-fmt
    protobuf
    protoc-gen-doc
    python3Packages.flake8
    python3Packages.pytest
    python3Packages.pytest-asyncio
    sqlite
  ] ++ lib.optional (stdenv.isLinux) gdb;
}
