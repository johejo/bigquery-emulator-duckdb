{
  lib,
  unzip,
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
  version = "1.5.6";
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
    aarch64-darwin = fetch "duckdb_cli-osx-universal.zip" "sha256-gKgMaHNr196lPosCRH6XWdsMhZbqFWR2ujlsRPVMWBA=";
    x86_64-linux = fetch "duckdb_cli-linux-amd64.zip" "sha256-bonerB67w27tApHK+LVnsDDHuGrDWZj3GFTiKzxdXi8=";
    aarch64-linux = fetch "duckdb_cli-linux-arm64.zip" "sha256-xUTpLJt8MfxTwhOYAsq9ji0bKz4/kzEX8xYRI5wUAts=";
  };
in
stdenvNoCC.mkDerivation {
  pname = "duckdb-bin";
  inherit version;
  src = sources.${stdenvNoCC.hostPlatform.system};
  sourceRoot = ".";

  nativeBuildInputs = [ unzip ] ++ lib.optionals stdenvNoCC.hostPlatform.isLinux [ autoPatchelfHook ];

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
