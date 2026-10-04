{
  description = "Development shell for bigquery-emulator-duckdb";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";
  };

  outputs =
    { nixpkgs, ... }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShellNoCC (
            {
              packages =
                with pkgs;
                [
                  bazelisk
                  buildifier
                  llvmPackages.clang-tools
                  # tests/e2e/run.sh waits for the servers with curl.
                  curlMinimal
                  (callPackage ./duckdb-bin.nix { })
                  fake-gcs-server
                  git
                  go
                  google-cloud-sdk
                  just
                  # GoogleSQL's Bazel launchers need /usr/bin/env python3 before switching to
                  # the rules_python interpreter.
                  python3
                  runn
                  cppcheck
                ]
                # llvm supplies the prefixed tools used to patch DuckDB archives in MODULE.bazel.
                ++ lib.optionals stdenv.hostPlatform.isLinux [
                  llvmPackages.clang
                  llvmPackages.llvm
                ];

              TZDIR = "${pkgs.tzdata}/share/zoneinfo";
            }
            # Linux's Clang package sets CC and CXX through its setup hook.
            // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isDarwin {
              CC = "/usr/bin/clang";
              CXX = "/usr/bin/clang++";
            }
            // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
              # Bazel runs genrules with /bin/bash, which NixOS does not have.
              BAZEL_SH = "${pkgs.bash}/bin/bash";
            }
          );
        }
      );
    };
}
