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
        in
        {
          default = pkgs.mkShellNoCC {
            packages = with pkgs; [
              bazelisk
              buildifier
              clang-tools
              # For trying a query against the engine the emulator embeds without building
              # anything. Worth keeping at the version MODULE.bazel pins, which nixpkgs
              # happens to carry today; nothing enforces that they stay in step.
              duckdb
              fake-gcs-server
              go
              google-cloud-sdk
              just
              runn
            ] ++ lib.optionals stdenv.hostPlatform.isLinux [ gcc14 ];

            shellHook =
              pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isDarwin ''
                export CC=/usr/bin/clang
                export CXX=/usr/bin/clang++
              ''
              # Bazel runs genrules with /bin/bash, which NixOS does not have.
              + pkgs.lib.optionalString pkgs.stdenv.hostPlatform.isLinux ''
                export CC=${pkgs.gcc14}/bin/gcc
                export CXX=${pkgs.gcc14}/bin/g++
                export BAZEL_SH=${pkgs.bash}/bin/bash
              '';
          };
        }
      );
    };
}
