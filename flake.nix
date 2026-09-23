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
              go
              google-cloud-sdk
              just
              runn
            ];

            shellHook = pkgs.lib.optionalString pkgs.stdenv.isDarwin ''
              export CC=/usr/bin/clang
              export CXX=/usr/bin/clang++
            '';
          };
        }
      );
    };
}
