{
  lib,
  stdenv,
  stdenvNoCC,
  fetchurl,
  autoPatchelfHook,
  versionCheckHook,
}:

let
  # The DuckDB CLI, for trying a query against the engine the emulator embeds without building
  # anything. Keep it at the version of the duckdb_prebuilt_* archives in MODULE.bazel;
  # `just duckdb-version-check` fails when they differ.
  version = "2.0.1";
  fetch =
    name: hash:
    fetchurl {
      urls = [
        "https://install.duckdb.org/v${version}/${name}"
        "https://github.com/duckdb/duckdb/releases/download/v${version}/${name}"
      ];
      inherit hash;
    };
  sources = {
    aarch64-darwin = fetch "duckdb-cli-osx-universal.tar.gz" "sha256-nW6geejDtWmPVZ/GTn5eQ90hpR6KYYspsY3kcWKZykA=";
    x86_64-linux = fetch "duckdb-cli-linux-amd64.tar.gz" "sha256-tnYmi+TTnk5BqQRELJeWyZFOZFzAhcdQXwSPLJkrXso=";
    aarch64-linux = fetch "duckdb-cli-linux-arm64.tar.gz" "sha256-hON4kfzEXIh+MCIPUaSRSZXNgXQTnof479MRrIPJVqc=";
  };
in
stdenvNoCC.mkDerivation {
  pname = "duckdb-bin";
  inherit version;
  src = sources.${stdenvNoCC.hostPlatform.system};
  sourceRoot = ".";

  nativeBuildInputs = lib.optionals stdenvNoCC.hostPlatform.isLinux [ autoPatchelfHook ];

  # Sorting program headers with patchelf 0.19 makes the release CLI crash at startup.
  patchelfFlags = [ "--no-sort" ];

  buildInputs = lib.optionals stdenvNoCC.hostPlatform.isLinux [ stdenv.cc.cc.lib ];

  installPhase = ''
    runHook preInstall
    install -Dm755 duckdb $out/bin/duckdb
    runHook postInstall
  '';

  nativeInstallCheckInputs = [ versionCheckHook ];
  doInstallCheck = true;

  meta = {
    description = "DuckDB CLI binary distribution";
    homepage = "https://duckdb.org/install";
    license = lib.licenses.mit;
    changelog = "https://github.com/duckdb/duckdb/releases/tag/v${version}";
    sourceProvenance = with lib.sourceTypes; [ binaryNativeCode ];
    mainProgram = "duckdb";
    platforms = builtins.attrNames sources;
  };
}
