{
  description = "Development shell for bigquery-emulator-duckdb";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
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

          # Tools that both the shell and Bazel's build actions use.
          buildTools = [
            pkgs.llvmPackages.clang-tools
          ]
          # llvm provides llvm-nm and llvm-objcopy, which MODULE.bazel uses to patch DuckDB's
          # archives. Its tools are all prefixed, so it does not change which ar, nm or ld the
          # C++ toolchain finds.
          ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isLinux [
            pkgs.llvmPackages.clang
            pkgs.llvmPackages.llvm
          ];

          # Bazel settings that pin the environment of build actions to store paths, so their
          # cache keys match on every machine with the same flake.lock and system. The shell hook
          # copies this to .bazelrc.devshell, which .bazelrc imports.
          actionPath = pkgs.lib.concatStringsSep ":" (
            [
              (pkgs.lib.makeBinPath (
                pkgs.stdenvNoCC.initialPath
                ++ [
                  # --action_env also sets the environment of repository rules, which fetch
                  # git_repository dependencies.
                  pkgs.git
                  # GoogleSQL's code generators start with #!/usr/bin/env python3.
                  pkgs.python3
                ]
                ++ buildTools
              ))
            ]
            # Apple's toolchain runs xcrun and friends from the system directories.
            ++ pkgs.lib.optionals pkgs.stdenv.hostPlatform.isDarwin [ "/usr/bin:/bin:/usr/sbin:/sbin" ]
          );
          tzdir = "${pkgs.tzdata}/share/zoneinfo";
          bazelrc = pkgs.writeText "bazelrc-nix" ''
            build --action_env=PATH=${actionPath}
            build --host_action_env=PATH=${actionPath}
            build --action_env=TZDIR=${tzdir}
            build --test_env=TZDIR=${tzdir}
          '';
        in
        {
          default = pkgs.mkShellNoCC {
            packages = with pkgs; [
              bazelisk
              buildifier
              # For trying a query against the engine the emulator embeds without building
              # anything. Worth keeping at the version MODULE.bazel pins, which nixpkgs
              # happens to carry today; nothing enforces that they stay in step.
              duckdb
              fake-gcs-server
              go
              google-cloud-sdk
              just
              runn
            ]
            ++ buildTools;

            shellHook = ''
              install -m 644 ${bazelrc} "$(git rev-parse --show-toplevel)/.bazelrc.devshell"
            ''
            + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
                export CC=/usr/bin/clang
                export CXX=/usr/bin/clang++
              ''
              # Bazel runs genrules with /bin/bash, which NixOS does not have.
              + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isLinux ''
                export CC=${pkgs.llvmPackages.clang}/bin/clang
                export CXX=${pkgs.llvmPackages.clang}/bin/clang++
                export BAZEL_SH=${pkgs.bash}/bin/bash
              '';
          };
        }
      );
    };
}
