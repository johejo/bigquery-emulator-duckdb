{
  description = "Development shell and CI apps for bigquery-emulator-duckdb";

  inputs = {
    nixpkgs.url = "https://channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst";
    nixpkgs-libc = {
      url = "github:NixOS/nixpkgs/34c7eb7545d155cc5b6f499b23a7cb1c96ab4d59";
      flake = false;
    };
  };

  outputs =
    { nixpkgs, nixpkgs-libc, ... }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      configurations = nixpkgs.lib.genAttrs systems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          releaseToolchain = import ./release-toolchain.nix {
            inherit pkgs;
            libcPkgs = import nixpkgs-libc { inherit system; };
          };
          releasePackages = [
            pkgs.bazelisk
            releaseToolchain.bintools
            releaseToolchain.clang
            pkgs.llvmPackages_23.llvm
          ];
          releaseEnvironment = {
            BAZEL_DEVSHELL_PATH = pkgs.lib.makeBinPath (releasePackages ++ pkgs.stdenvNoCC.initialPath);
            CC = "${releaseToolchain.clang}/bin/clang";
            CXX = "${releaseToolchain.clang}/bin/clang++";
            RELEASE_GLIBC = "${releaseToolchain.libc}";
            FHS_INTERPRETER = if pkgs.stdenv.hostPlatform.isAarch64
              then "/lib/ld-linux-aarch64.so.1" else "/lib64/ld-linux-x86-64.so.2";
          };
          buildPackages =
            with pkgs;
            [ bazelisk ]
            # llvm supplies the prefixed tools used to patch DuckDB archives in MODULE.bazel.
            # bintools wraps ld.lld so Nix's runtime paths are set.
            ++ lib.optionals stdenv.hostPlatform.isLinux [
              llvmPackages_23.bintools
              llvmPackages_23.clang
              llvmPackages_23.llvm
            ];
          testPackages = with pkgs; [
            (callPackage ./duckdb-bin.nix { })
            fake-gcs-server
            go
            google-cloud-sdk
            jq
            postgresql
            mysql84
            runn
          ];
          formatPackages = with pkgs; [
            buildifier
            go
            llvmPackages_23.clang-tools
            tombi
            treefmt
          ];
          devPackages = with pkgs; formatPackages ++ [
            # tools/update_bigquery_functions.sh fetches upstream documentation.
            curlMinimal
            git
            go-tools
            just
            python3
            cppcheck
          ];
          # The shell and apps use the same build tools and standard shell utilities. Test and
          # editor tools must not affect action keys, even when nested inside another shell.
          buildPath = pkgs.lib.makeBinPath (buildPackages ++ pkgs.stdenvNoCC.initialPath);
          buildEnvironment = {
            BAZEL_DEVSHELL_PATH = buildPath;
            TZDIR = "${pkgs.tzdata}/share/zoneinfo";
          } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isDarwin {
            CC = "/usr/bin/clang";
            CXX = "/usr/bin/clang++";
          } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            # Apps do not run the compiler setup hook that sets these in a devShell.
            CC = "${pkgs.llvmPackages_23.clang}/bin/clang";
            CXX = "${pkgs.llvmPackages_23.clang}/bin/clang++";
            # Bazel runs genrules with /bin/bash, which NixOS does not have.
            BAZEL_SH = "${pkgs.bash}/bin/bash";
          };
          testEnvironment = {
            # Just recipes add these only while running tests and probes.
            TEST_TOOLS_PATH = pkgs.lib.makeBinPath testPackages;
          } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            SPANNER_EMULATOR_GATEWAY = "${pkgs.callPackage ./cloud-spanner-emulator-bin.nix { }}/bin/gateway_main";
          };
          lintEnvironment = {
            # Repository rules may have a restricted PATH. Resolve clang-tidy separately
            # so its executable is also an input to lint actions.
            CLANG_TIDY = "${pkgs.llvmPackages_23.clang-tools}/bin/clang-tidy";
          };
          mkApp = name: packages: environment: command: {
            type = "app";
            meta.description = "Run ${name} for bigquery-emulator-duckdb";
            program = pkgs.lib.getExe (pkgs.writeShellApplication {
              inherit name;
              runtimeInputs = buildPackages ++ pkgs.stdenvNoCC.initialPath ++ packages;
              runtimeEnv = buildEnvironment // environment;
              text = command;
            });
          };
          recipeTools = with pkgs; {
            fmt = formatPackages ++ [ gitMinimal ];
            fmt-check = formatPackages ++ [ gitMinimal ];
            cppcheck = [ cppcheck ];
            go-vet = [ go ];
            go-staticcheck = [ go go-tools ];
            test = [ ];
            docs-check = [ ];
            e2e = [ ];
            clang-tidy = [ ];
            check = formatPackages ++ [ gitMinimal cppcheck go-tools ];
          };
        in
        {
          devShells.default = pkgs.mkShellNoCC (
            buildEnvironment // testEnvironment // lintEnvironment // {
              packages = buildPackages ++ testPackages ++ devPackages;
              # Compiler setup hooks set bare names; keep the same paths as the apps.
              shellHook = ''
                export CC=${pkgs.lib.escapeShellArg buildEnvironment.CC}
                export CXX=${pkgs.lib.escapeShellArg buildEnvironment.CXX}
              '';
            }
          );
          apps = pkgs.lib.mapAttrs (name: packages:
            mkApp name ([ pkgs.just ] ++ packages)
              (pkgs.lib.optionalAttrs (builtins.elem name [ "test" "docs-check" "e2e" "check" ]) testEnvironment
                // pkgs.lib.optionalAttrs (name == "clang-tidy" || name == "check") lintEnvironment)
              ''exec just ${if name == "clang-tidy" then "tidy" else name} "$@"''
          ) recipeTools // {
            bazel = mkApp "bazel" [ ] { } ''exec bazelisk "$@"'';
            compliance-package = mkApp "compliance-package" [ pkgs.go ] { }
              ''exec go run ./tools/complianceci package "$@"'';
            compliance-run = mkApp "compliance-run" [ ] { } ''
              bundle_dir="$1"
              shift
              exec "$bundle_dir/complianceci" run "$bundle_dir" "$@"
            '';
          } // pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            release = mkApp "release" (releasePackages ++ [ pkgs.just pkgs.patchelf pkgs.python3 pkgs.binutils ])
              releaseEnvironment ''exec just release "$@"'';
          };
        }
      );
    in
    {
      devShells = nixpkgs.lib.mapAttrs (_: configuration: configuration.devShells) configurations;
      apps = nixpkgs.lib.mapAttrs (_: configuration: configuration.apps) configurations;
    };
}
