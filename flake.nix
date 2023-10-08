{
  description = "vidpak";

  inputs.flake-utils.url = "github:numtide/flake-utils";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs, flake-utils }:
    nixpkgs.lib.foldr nixpkgs.lib.recursiveUpdate { } [
      (flake-utils.lib.eachDefaultSystem (system: {
        packages.vidpak = nixpkgs.legacyPackages.${system}.python3Packages.callPackage ./. {};

        # the dev shell needs pip so `nix develop` and `setuptoolsShellHook` can install the package in editable mode
        devShells.vidpak = (self.packages.${system}.vidpak.overrideAttrs (o: {
          nativeBuildInputs = (o.nativeBuildInputs or []) ++ [ nixpkgs.legacyPackages.${system}.python3Packages.pip ];
        }));

        defaultPackage = self.packages.${system}.vidpak;
        devShells.default = self.devShells.${system}.vidpak;
      }))
    ];
}
