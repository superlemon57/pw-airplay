{
  description = "pw-airplay: AirPlay 2 output for PipeWire";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in rec {
          # OwnTone 29.3, patched for live-pipe input and locked down
          # network exposure, plus airptpd (its PTP clock daemon).
          owntone-airplay = pkgs.callPackage ./nix/owntone-airplay.nix { };

          # pw-airplayd itself.
          pw-airplayd = pkgs.callPackage ./nix/pw-airplayd.nix { src = self; };

          default = pw-airplayd;
        });

      # System-wide part: airptpd + firewall ports. Equivalent of
      # install-system.sh. Import into configuration.nix.
      nixosModules.default = import ./nix/module-nixos.nix self;
      nixosModules.pw-airplay = self.nixosModules.default;

      # Per-user part: systemd user services, configuration, PipeWire
      # drop-in. Equivalent of install.sh. Import into home.nix.
      homeModules.default = import ./nix/module-home.nix self;
      homeModules.pw-airplay = self.homeModules.default;
    };
}
