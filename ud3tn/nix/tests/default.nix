# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

{ pkgs, nixosModules ? { }, overlays ? { } }:

let
  nixosLib = import (pkgs.path + "/nixos/lib") { };

  runTest = module: nixosLib.runTest {
    imports = [ module ];
    hostPkgs = pkgs;
    globalTimeout = 5 * 60; # = 5 minutes
    extraBaseModules = {
      imports = builtins.attrValues nixosModules;
      nixpkgs = {
        inherit pkgs;
        overlays = builtins.attrValues overlays;
      };
    };
  };
in

{
  multipleServices = runTest ./multiple-services.nix;
}
