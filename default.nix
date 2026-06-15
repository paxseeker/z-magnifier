{ lib ? pkgs.lib, pkgs ? import <nixpkgs> { }, ... }:

pkgs.stdenv.mkDerivation {
  pname = "z-magnifier";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = with pkgs; [
    pkg-config
    wayland-scanner
    wayland-protocols
  ];

  buildInputs = with pkgs; [
    wayland
  ];

  preBuild = ''
    make clean
  '';

  installPhase = ''
    mkdir -p $out/bin
    cp magnifier $out/bin/
  '';

  meta = with lib; {
    description = "Wayland overlay magnifier";
    longDescription = ''
      A pure Wayland magnifying glass overlay using wlr-layer-shell and
      zwlr-screencopy. Captures the full output once at startup, then
      re-samples from the saved frame data on every pointer move, zoom change,
      or radius change — no repeated captures.
    '';
    homepage = "https://github.com/paxseeker/z-magnifier";
    license = licenses.mit;
    platforms = platforms.linux;
    maintainers = [ ];
  };
}
