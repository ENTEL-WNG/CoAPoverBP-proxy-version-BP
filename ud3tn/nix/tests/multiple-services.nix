# SPDX-License-Identifier: BSD-3-Clause OR Apache-2.0

{ pkgs, ... }:

{
  name = "NixOS module test";

  nodes = {
    client = {
      environment.systemPackages = with pkgs; [
        (python3.withPackages (_: [ python-ud3tn-utils ]))
      ];
    };

    ud3tnMachine = { ... }: {
      environment.systemPackages = with pkgs; [
        (python3.withPackages (_: [ python-ud3tn-utils ]))
      ];

      networking.firewall.allowedTCPPorts = [
        2345 # AAP ud3tn@b
        5432 # AAP2 ud3tn@b
      ];

      services.ud3tn.a = {
        enable = true;
        bdmSecretFile = "${pkgs.writeText "ud3tn-bdm-secret" "very_secure_secret"}";
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
        bdmSecretFile = "${pkgs.writeText "ud3tn-bdm-secret" "very_secure_secret"}";
        bpVersion = 6;
        cla = {
          mtcp = {
            host = "0.0.0.0";
            port = 3456;
          };
          smtcp = {
            host = "localhost";
            port = 4567;
            active = false;
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
        lifetime = 43200;
        logLevel = 3;
        maxBundleSize = 4294967295; # u32_max
        statusReports = true;
        externalDispatch = true;
      };
    };
  };

  testScript = ''
    start_all()

    client.wait_for_unit("multi-user.target")

    ud3tnMachine.wait_for_unit("ud3tn@a.service")
    ud3tnMachine.wait_for_unit("ud3tn@b.service")
    
    with subtest("aap2-ping"):
      print("Try to ping ud3tn@a via AAP2 UNIX socket from the ud3tn node itself")
      ud3tnMachine.succeed(
        "aap2-ping "
        "--socket /run/ud3tn@a/aap2.socket "
        "-c3 "
        "dtn://ud3tn.dtn/echo"
      )

      print("Try to ping ud3tn@b via AAP2 TCP socket from the client node")
      client.succeed(
        "aap2-ping "
        "--tcp ud3tnMachine 5432 "
        "-c3 "
        "dtn://ud3tn-b.dtn/echo"
      )
    
    with subtest("aap-ping"):
      print("Try to ping ud3tn@a via AAP UNIX socket from the ud3tn node itself")
      ud3tnMachine.succeed(
        "aap-ping "
        "--socket /run/ud3tn@a/aap.socket "
        "-c3 "
        "dtn://ud3tn.dtn/echo"
      )

      print("Try to ping ud3tn@b via AAP TCP socket from the client node")
      client.succeed(
        "aap-ping "
        "--tcp ud3tnMachine 2345 "
        "-c3 "
        "dtn://ud3tn-b.dtn/echo"
      )
  '';
}
