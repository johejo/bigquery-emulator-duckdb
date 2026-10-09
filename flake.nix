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
                  llvmPackages_23.clang-tools
                  # tools/update_bigquery_functions.sh fetches upstream documentation.
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
                # Bazel's toolchain detection links with lld when clang finds ld.lld, and with
                # gold otherwise; bintools wraps ld.lld so Nix's runtime paths are set.
                ++ lib.optionals stdenv.hostPlatform.isLinux [
                  llvmPackages_23.bintools
                  llvmPackages_23.clang
                  llvmPackages_23.llvm
                ];

              TZDIR = "${pkgs.tzdata}/share/zoneinfo";
              # Repository rules may have a restricted PATH for toolchain detection. Resolve
              # clang-tidy independently so its executable is also an input to lint actions.
              CLANG_TIDY = "${pkgs.llvmPackages_23.clang-tools}/bin/clang-tidy";
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
