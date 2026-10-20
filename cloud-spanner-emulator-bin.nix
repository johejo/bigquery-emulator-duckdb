{
  lib,
  stdenv,
  stdenvNoCC,
  fetchurl,
  autoPatchelfHook,
}:

let
  version = "1.5.59";
  fetch =
    arch: hash:
    fetchurl {
      url = "https://storage.googleapis.com/cloud-spanner-emulator/releases/${version}/cloud-spanner-emulator_linux_${arch}-${version}.tar.gz";
      inherit hash;
    };
  sources = {
    x86_64-linux = fetch "amd64" "sha256-OU/8B1g51+kwNozm6XiJ1xOFanz6qbTJ+nCPiCy8TF8=";
    aarch64-linux = fetch "arm64" "sha256-DpoVtHq5mI8x91jnalsNDTCZaXk26D7l41tGtVaZIzQ=";
  };
in
stdenvNoCC.mkDerivation {
  pname = "cloud-spanner-emulator-bin";
  inherit version;
  src = sources.${stdenvNoCC.hostPlatform.system};
  sourceRoot = ".";

  nativeBuildInputs = [ autoPatchelfHook ];
  buildInputs = [ stdenv.cc.cc.lib ];

  installPhase = ''
    runHook preInstall
    # The gateway locates emulator_main and third-party notices next to itself.
    install -Dm755 gateway_main $out/bin/gateway_main
    install -Dm755 emulator_main $out/bin/emulator_main
    if [[ -f THIRD_PARTY_NOTICES.txt ]]; then
      install -Dm644 THIRD_PARTY_NOTICES.txt $out/bin/THIRD_PARTY_NOTICES.txt
    else
      # The arm64 archive supplies compressed notices under a different name.
      gzip -dc licenses.txt.gz > $out/bin/THIRD_PARTY_NOTICES.txt
    fi
    runHook postInstall
  '';

  meta = {
    description = "Cloud Spanner Emulator binary distribution";
    homepage = "https://github.com/GoogleCloudPlatform/cloud-spanner-emulator";
    license = lib.licenses.asl20;
    changelog = "https://github.com/GoogleCloudPlatform/cloud-spanner-emulator/releases/tag/v${version}";
    sourceProvenance = with lib.sourceTypes; [ binaryNativeCode ];
    mainProgram = "gateway_main";
    platforms = builtins.attrNames sources;
  };
}
