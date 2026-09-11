{
  description = "Paintboard: a local, offline drawing board in C and OpenGL";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forEach = nixpkgs.lib.genAttrs systems;
    in
    {
      overlays.default = final: _prev: {
        paintboard = final.callPackage ./package.nix { };
      };

      packages = forEach (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        rec {
          paintboard = pkgs.callPackage ./package.nix { };
          default = paintboard;
        });

      devShells = forEach (system:
        let pkgs = nixpkgs.legacyPackages.${system}; in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.paintboard ];
            packages = [ pkgs.resvg pkgs.python3 ];
          };
        });
    };
}
