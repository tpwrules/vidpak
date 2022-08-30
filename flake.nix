{
  description = "vidpak";

  inputs.flake-utils.url = "github:numtide/flake-utils";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs = { self, nixpkgs, flake-utils }:
    nixpkgs.lib.foldr nixpkgs.lib.recursiveUpdate { } [
      (flake-utils.lib.eachDefaultSystem (system: {
        packages.vidpak = nixpkgs.legacyPackages.${system}.python3Packages.callPackage ./. {};

        defaultPackage = self.packages.${system}.vidpak;
      }))
    ];
}
