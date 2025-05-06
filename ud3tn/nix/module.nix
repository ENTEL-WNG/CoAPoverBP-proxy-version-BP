# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

{ config, lib, pkgs, ... }:

let
  cfg = config.services.ud3tn;

  mkClaOptions = { defaultPort, enable ? true, addActiveOption ? false }: {
    enable = lib.mkOption {
      description = "Whether to enable the CLA.";
      default = enable;
      type = lib.types.bool;
    };

    host = lib.mkOption {
      description = "IP address / host name that the CLA should listen on.";
      default = "*";
      type = lib.types.str;
    };

    port = lib.mkOption {
      description = "Port number that the CLA should listen on.";
      default = defaultPort;
      type = lib.types.port;
    };
  } // lib.optionalAttrs addActiveOption {
    active = lib.mkOption {
      description = ''
        If true the provided host name or IP address and port number are used for initiating a TCP
        connection.
      '';
      default = false;
      type = lib.types.bool;
    };
  };

  ud3tnOptions = {
    options = {
      enable = lib.mkEnableOption "μD3TN";

      aap = {
        host = lib.mkOption {
          description = "IP / hostname of the application agent service.";
          default = null;
          example = [ "127.0.0.1" "localhost" "::" "::1" "dtn.example" ];
          type = with lib.types; nullOr str;
        };

        port = lib.mkOption {
          description = "Port number of the application agent service.";
          default = null;
          type = with lib.types; nullOr port;
        };
      };      

      aap2 = {
        host = lib.mkOption {
          description = "IP / hostname of the application agent v2 service.";
          default = null;
          example = [ "127.0.0.1" "localhost" "::" "::1" "dtn.example" ];
          type = with lib.types; nullOr str;
        };

        port = lib.mkOption {
          description = "Port number of the application agent v2 service.";
          default = null;
          type = with lib.types; nullOr port;
        };
      };

      allowRemoteConfig = lib.mkOption {
        description = ''
          Allow configuration via bundles received from CLAs or from unauthorized AAP clients.
        '';
        default = false;
        type = lib.types.bool;
      };

      bdmSecretFile = lib.mkOption {
        description = ''
          Restrict AAP 2.0 BDM functions to clients providing the secret in the give file."
        '';
        type = lib.types.path;
      };

      bpVersion = lib.mkOption {
        description = "Bundle protocol version of bundles created via AAP/AAP2.";
        default = 7;
        type = lib.types.enum [ 6 7 ];
      };

      cla = {
        mtcp = mkClaOptions { defaultPort = 4224; };
        smtcp = mkClaOptions { defaultPort = 4222; addActiveOption = true; };
        sqlite = {
          enable = lib.mkOption {
            description = "Whether to enable the SQLiteCLA.";
            default = true;
            type = lib.types.bool;
          };
        };
        tcpclv3 = mkClaOptions { defaultPort = 4556; };
        tcpspp = mkClaOptions { defaultPort = 4223; enable = false; addActiveOption = true; };
      };

      nodeId = lib.mkOption {
        description = "Local node identifier.";
        default = "dtn://ud3tn.dtn/";
        example = [ "dtn://ud3tn.dtn/" "ipn:1.0" ];
        type = lib.types.str;
      };

      externalDispatch = lib.mkOption {
        description = "Do not load the internal minimal router, allow for using an AAP 2.0 BDM.";
        default = false;
        type = lib.types.bool;
      };

      lifetime = lib.mkOption {
        description = "Lifetime in seconds of bundles created via AAP.";
        default = 86400;
        type = lib.types.ints.positive;
      };

      logLevel = lib.mkOption {
        description = "Higher or lower log level 1|2|3 specifies more or less detailed output.";
        default = 1;
        type = lib.types.enum [ 1 2 3 ];
      };

      maxBundleSize = lib.mkOption {
        description = ''
          Threshold used to determine if a bundle has to be fragmented.
          The effective threshold will be determined as the minimum of this argument and all
          maximum bundle sizes reported by the activated CLAs. A value of zero means "unlimited"
          (2^64 bytes).
        '';
        default = 0;
        type = lib.types.ints.unsigned;
      };

      openFirewall = lib.mkOption {
        description = "Open firewall ports.";
        default = false;
        type = lib.types.bool;
      };

      statusReports = lib.mkOption {
        description = "Enable status reporting.";
        default = false;
        type = lib.types.bool;
      };
    };
  };

  mkPortList = _: cfg: (
    (lib.attrsets.mapAttrsToList (_: cla: (lib.mkIf cfg.enable cla.port)) (lib.attrsets.filterAttrs (_: v: v ? "port") cfg.cla))
  );

  mkNetworkingConfig = _: cfg: {
    firewall.allowedTCPPorts = lib.mkIf cfg.openFirewall (mkPortList _ cfg);
  };

  mkServiceConfig = name: cfg:
    let
      mkClaStr = clas: (lib.concatStrings (
        lib.attrsets.mapAttrsToList
          (
            name: cla@{ enable, host, port, ... }: (
              lib.optionalString enable (
                "${name}:${host},${toString port}"
                + lib.optionalString (cla ? "active") ",${lib.boolToString cla.active}"
                + ";"
              )
            )
          )
          (lib.attrsets.filterAttrs (_: v: v ? "host" && v ? "port") clas)
        ++ lib.optional (clas.sqlite.enable) "sqlite:/var/lib/ud3tn@${name}/u3dtn.sqlite;"
      ));
    in
    {
      "ud3tn@${name}" = lib.mkIf cfg.enable {
        wantedBy = [ "multi-user.target" ];
        environment."UD3TN_BDM_SECRET" = (builtins.readFile cfg.bdmSecretFile);
        serviceConfig = {
          RuntimeDirectory = "ud3tn@${name}"; # created in /run/
          StateDirectory = "ud3tn@${name}"; # created in /var/lib/
          ExecStart = lib.concatStringsSep " " (
            [
              "${pkgs.ud3tn}/bin/ud3tn"
              "--bdm-secret-var UD3TN_BDM_SECRET"
              "--bp-version ${toString cfg.bpVersion}"
              "--cla ${mkClaStr cfg.cla}"
              "--node-id ${cfg.nodeId}"
              "--lifetime ${toString cfg.lifetime}"
              "--log-level ${toString cfg.logLevel}"
              "--max-bundle-size ${toString cfg.maxBundleSize}"
            ]
            # If a UNIX and a TCP socket parameter is provided, μD3TN favours the UNIX socket, so
            # the --aap-socket parameter is only set if neither aap.host nor aap.port is defined
            ++ lib.optional (cfg.aap.host == null && cfg.aap.port == null)
              "--aap-socket /run/ud3tn@${name}/aap.socket"
            ++ lib.optional (cfg.aap.host != null) "--aap-host ${cfg.aap.host}"
            ++ lib.optional (cfg.aap.port != null) "--aap-port ${toString cfg.aap.port}"
            ++ lib.optional (cfg.aap2.host == null && cfg.aap2.port == null)
                "--aap2-socket /run/ud3tn@${name}/aap2.socket"
            ++ lib.optional (cfg.aap2.host != null) "--aap2-host ${cfg.aap2.host}"
            ++ lib.optional (cfg.aap2.port != null) "--aap2-port ${toString cfg.aap2.port}"
            ++ lib.optional cfg.allowRemoteConfig "--allow-remote-config"
            ++ lib.optional cfg.externalDispatch "--external-dispatch"
            ++ lib.optional cfg.statusReports "--status-reports"
          );
        };
      };
    };
in

{
  options.services.ud3tn = lib.mkOption {
    type = lib.types.attrsOf (lib.types.submodule ud3tnOptions);
    default = { };
    description = "Attribute set to configure multiple μD3TN instances";
    example = lib.literalExpression ''
      {
        services.ud3tn.a = {
          enable = true;
          bdmSecretFile = /var/lib/ud3tn@a/bdm-secret;
        };

        services.ud3tn.b = {
          enable = true;
          openFirewall = true;
          aap = {
            host = "0.0.0.0";
            port = 2345;
          };
          aap2 = {
            host = "::0";
            port = 5432;
          };
          allowRemoteConfig = true;
          bdmSecretFile = /var/lib/ud3tn@b/bdm-secret;
          bpVersion = 6;
          cla = {
            mtcp = {
              host = "0.0.0.0";
              port = 3456;
            };
            smtcp = {
              host = "localhost";
              port = 4567;
              active = true;
            };
            tcpclv3 = {
              host = "::";
              port = 5678;
            };
            tcpspp = {
              host = "::1";
              port = 6789;
              active = true;
            };
          };
          nodeId = "dtn://ud3tn-b.dtn/";
          externalDispatch = true;
          lifetime = 43200;
          logLevel = 3;
          maxBundleSize = 4294967295; # u32_max
          statusReports = true;
        };
      }
    '';
  };

  config = {
    assertions = [{
      assertion = (lib.lists.allUnique (lib.lists.flatten (lib.attrsets.mapAttrsToList mkPortList cfg)));
      message = ''
        Colliding port definitions.
        Make sure that you configure different ports when operating multiple μD3TN instances.
      '';
    }];

    networking = lib.mkMerge (
      lib.attrsets.mapAttrsToList mkNetworkingConfig cfg
    );

    systemd.services = lib.mkMerge (
      lib.attrsets.mapAttrsToList mkServiceConfig cfg
    );
  };
}
