{
  description = "rummage, a native file manager for Wayland";

  inputs.nixpkgs.url = "https://channels.nixos.org/nixos-unstable/nixexprs.tar.zst";

  nixConfig = {
    extra-substituters = [ "https://rummage-fm.cachix.org" ];
    extra-trusted-public-keys = [
      "rummage-fm.cachix.org-1:fhPhaCttVfPBS/i//+Z+6Ch0xdxj9g94eZdQ5m6mlwA="
    ];
  };

  outputs =
    { self, nixpkgs }:
    let
      forEachSystem = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
      ];
    in
    {
      nixosModules = {
        rummage = import ./nix/module.nix self;
        default = self.nixosModules.rummage;
      };

      packages = forEachSystem (system: {
        rummage = nixpkgs.legacyPackages.${system}.callPackage ./nix/package.nix { };
        # Expose the source-built renderer for standalone builds and dependency inspection.
        pdfium = nixpkgs.legacyPackages.${system}.callPackage ./nix/pdfium.nix { };
        pdfium-sources = import ./nix/pdfium-sources.nix { pkgs = nixpkgs.legacyPackages.${system}; };
        rummage-with-photos = self.packages.${system}.rummage.override { withPhotos = true; };
        rummage-photos = self.packages.${system}.rummage-with-photos.photos;
        rummage-with-font = self.packages.${system}.rummage.override { withFont = true; };
        rummage-font = self.packages.${system}.rummage-with-font.font;
        rummage-with-model = self.packages.${system}.rummage.override { withModel = true; };
        rummage-model = self.packages.${system}.rummage-with-model.model;
        default = self.packages.${system}.rummage;
      });

      checks = forEachSystem (system: {
        tests =
          (self.packages.${system}.rummage.override {
            withTests = true;
            withPhotos = true;
            withFont = true;
            withModel = true;
            withTerminal = true;
          }).overrideAttrs
            {
              pname = "rummage-tests";
              outputs = [ "out" ];
              # Compile and test the app/helpers, but produce no installable packages.
              installPhase = ''
                mkdir -p "$out"
                touch "$out/passed"
              '';
              postInstall = "";
              dontFixup = true;
            };
        playback =
          let
            pkgs = nixpkgs.legacyPackages.${system};
            helper = self.packages.${system}.rummage;
          in
          pkgs.runCommand "rummage-playback-installed" { nativeBuildInputs = [ pkgs.python3 ]; } ''
            test -x ${helper}/libexec/rummage/rummage-audio
            python3 ${./scripts/ci/check-playback.py} ${helper}/libexec/rummage/rummage-playback ${./tests/data/media}
            python3 ${./scripts/ci/check-audio.py} ${helper}/libexec/rummage/rummage-audio ${pkgs.pulseaudio}/bin
            touch "$out"
          '';
        font =
          let
            pkgs = nixpkgs.legacyPackages.${system};
            helper = self.packages.${system}.rummage-font;
          in
          pkgs.runCommand "rummage-font-installed" { nativeBuildInputs = [ pkgs.python3 ]; } ''
            test -s ${helper}/share/rummage/engines/font.toml
            python3 ${./scripts/ci/check-font.py} ${helper}/libexec/rummage/rummage-font \
              ${pkgs.dejavu_fonts}/share/fonts/truetype/DejaVuSans.ttf
            touch "$out"
          '';
        nixos-module = import ./nix/module-check.nix { inherit self nixpkgs system; };
        smoke = nixpkgs.legacyPackages.${system}.callPackage ./nix/smoke-test.nix {
          package = self.packages.${system}.rummage;
        };
        smoke-photos = nixpkgs.legacyPackages.${system}.callPackage ./nix/smoke-test.nix {
          package = self.packages.${system}.rummage-with-photos;
        };
      });

      devShells = forEachSystem (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          instrumentation = (pkgs.mkShell.override { stdenv = pkgs.clangStdenv; }) {
            inputsFrom = [
              (self.packages.${system}.rummage.override {
                stdenv = pkgs.clangStdenv;
                withTests = true;
                withPhotos = true;
                withFont = true;
                withModel = true;
                withTerminal = true;
              })
            ];
            packages = with pkgs; [
              python3
              doctest
              dbus
              shared-mime-info
              _7zz
              unzip
              (writeShellScriptBin "7z" ''exec ${_7zz}/bin/7zz "$@"'')
            ];
            dontUseCmakeConfigure = true;
            FONTCONFIG_FILE = pkgs.makeFontsConf { fontDirectories = [ pkgs.dejavu_fonts ]; };
            XDG_DATA_DIRS = "${pkgs.shared-mime-info}/share:${pkgs.hicolor-icon-theme}/share";
            LIBGL_ALWAYS_SOFTWARE = "1";
            EGL_PLATFORM = "surfaceless";
            __EGL_VENDOR_LIBRARY_FILENAMES = "${pkgs.mesa}/share/glvnd/egl_vendor.d/50_mesa.json";
            LIBGL_DRIVERS_PATH = "${pkgs.mesa}/lib/dri";
          };
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.rummage ];
            packages = with pkgs; [
              libvterm-neovim
              bashInteractive
              zsh
              clang-tools
              assimp
              doctest
              tokei
              gdb
              git
              ffmpeg
              sqlite
              libpulseaudio
              libjpeg_turbo
              nixfmt
              shared-mime-info
            ];
            # Keep CMake's setup hook from configuring a build on shell entry.
            dontUseCmakeConfigure = true;
            shellHook = ''
              export XDG_DATA_DIRS="''${XDG_DATA_DIRS:-/usr/local/share:/usr/share}:${pkgs.shared-mime-info}/share"
            '';
          };
        }
      );

      formatter = forEachSystem (system: nixpkgs.legacyPackages.${system}.nixfmt);
    };
}
