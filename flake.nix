{
  description = "FractalisPico - Mandelbrot explorer for the Raspberry Pi Pico 2";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            ninja
            gcc-arm-embedded # arm-none-eabi-gcc incl. newlib
            picotool         # flashing, also used by the SDK build (found via PATH)
            python3          # needed by the pico-sdk build
            tio              # serial monitor for the printf output
          ];

          shellHook = ''
            export PICO_SDK_PATH="$PWD/pico-sdk"
            echo "FractalisPico dev shell"
            echo "  first time:  git submodule update --init && git -C pico-sdk submodule update --init lib/tinyusb"
            echo "  configure:   cmake -B build -G Ninja"
            echo "  build+flash: ./flash_pico.sh"
            echo "  serial log:  tio /dev/ttyACM0"
          '';
        };
      });
    };
}
