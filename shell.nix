{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
    buildInputs = with pkgs; [
        wayland
        wayland-protocols
        wayland-scanner
        pkg-config
    ];
}
