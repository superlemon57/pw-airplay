{ lib
, stdenv
, pkg-config
, pipewire
, curl
, json_c
, avahi
, systemd
, src
}:

stdenv.mkDerivation {
  pname = "pw-airplayd";
  version = "0-unstable";

  inherit src;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ pipewire curl json_c avahi systemd ];

  enableParallelBuilding = true;

  installPhase = ''
    runHook preInstall
    make install PREFIX=$out
    install -Dm644 README.md $out/share/doc/pw-airplay/README.md
    runHook postInstall
  '';

  meta = {
    description = "AirPlay 2 output for PipeWire";
    homepage = "https://github.com/superlemon57/pw-airplay";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
    mainProgram = "pw-airplayd";
  };
}
