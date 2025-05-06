# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-darwin"
        "x86_64-linux"
      ];

      forAllSystems = fn: nixpkgs.lib.genAttrs systems (
        system: fn rec {
          pkgs = import nixpkgs { inherit system; };
        }
      );
    in
    {
      apps = forAllSystems ({ pkgs, ... }: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.ud3tn}/bin/ud3tn";
        };
      });

      packages = forAllSystems ({ pkgs, ... }:
        import ./nix/packages.nix { inherit pkgs; }
      );

      checks = forAllSystems ({ pkgs, ... }: import ./nix/tests {
        pkgs = pkgs.extend self.overlays.default;
        nixosModules = self.nixosModules;
      });

      devShells = forAllSystems ({ pkgs, ... }: {
        default = import ./nix/shell.nix { inherit pkgs self; };
      });

      formatter = forAllSystems ({ pkgs, ... }: pkgs.nixpkgs-fmt);

      overlays.default = final: prev: {
        inherit (self.packages.${final.system}) ud3tn pyd3tn python-ud3tn-utils;
      };

      nixosModules.default = import ./nix/module.nix;
    };
}
