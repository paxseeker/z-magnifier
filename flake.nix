{
  description = "Wayland overlay magnifier";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      forAllPkgs = f: forAllSystems (system: f (import nixpkgs { inherit system; }));
    in {
      packages = forAllPkgs (pkgs: rec {
        default = pkgs.callPackage ./default.nix { };
        z-magnifier = default;
      });

      apps = forAllPkgs (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.system}.default}/bin/magnifier";
        };
      });

      devShells = forAllPkgs (pkgs:
        pkgs.mkShell {
          buildInputs = with pkgs; [
            wayland
            wayland-protocols
            wayland-scanner
            pkg-config
          ];
        }
      );
    };
}
