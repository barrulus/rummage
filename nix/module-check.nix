{
  self,
  nixpkgs,
  system,
}:
let
  pkgs = nixpkgs.legacyPackages.${system};
  lib = nixpkgs.lib;
  chooser = "org.freedesktop.impl.portal.FileChooser";
  evaluate =
    settings:
    (lib.nixosSystem {
      inherit system;
      modules = [
        self.nixosModules.default
        { system.stateVersion = "26.11"; }
        settings
      ];
    }).config;
  disabled = evaluate { };
  app = evaluate { programs.rummage.enable = true; };
  photos = evaluate {
    programs.rummage = {
      enable = true;
      photos.enable = true;
      filePicker.enable = true;
    };
  };
  picker = evaluate {
    programs.rummage = {
      enable = true;
      filePicker.enable = true;
    };
    xdg.portal = {
      extraPortals = [ pkgs.xdg-desktop-portal-gtk ];
      config.umbriel = {
        default = [
          "umbriel"
          "gtk"
        ];
        "org.freedesktop.impl.portal.ScreenCast" = [ "umbriel" ];
      };
    };
  };
  replacement = pkgs.runCommand "rummage-module-test-package" { } "mkdir -p $out";
  custom = evaluate {
    programs.rummage = {
      enable = true;
      package = replacement;
      filePicker = {
        enable = true;
        desktops = [
          "niri"
          "common"
        ];
      };
    };
  };
  invalid = evaluate { programs.rummage.filePicker.enable = true; };
  emptyDesktops = evaluate {
    programs.rummage = {
      enable = true;
      filePicker.enable = true;
      filePicker.desktops = [ ];
    };
  };
  rejected =
    config: lib.any (a: !a.assertion && lib.hasPrefix "programs.rummage." a.message) config.assertions;
in
assert !disabled.xdg.portal.enable;
assert !(lib.elem disabled.programs.rummage.package disabled.environment.systemPackages);
assert lib.elem app.programs.rummage.package app.environment.systemPackages;
assert !app.xdg.portal.enable;
assert app.xdg.portal.config == { };
assert !app.programs.rummage.photos.enable;
assert app.programs.rummage.package == self.packages.${system}.default;
assert photos.programs.rummage.package == self.packages.${system}.rummage-with-photos;
assert lib.elem "photos" photos.programs.rummage.package.outputs;
assert !(lib.elem "photos" app.programs.rummage.package.outputs);
assert lib.elem photos.programs.rummage.package photos.environment.systemPackages;
assert lib.elem photos.programs.rummage.package photos.xdg.portal.extraPortals;
assert !rejected photos;
assert picker.xdg.portal.enable;
assert lib.elem picker.programs.rummage.package picker.xdg.portal.extraPortals;
assert lib.elem pkgs.xdg-desktop-portal-gtk picker.xdg.portal.extraPortals;
assert lib.elem picker.programs.rummage.package picker.services.dbus.packages;
assert picker.xdg.portal.config.umbriel.${chooser} == "rummage";
assert picker.xdg.portal.config.umbriel.default == "umbriel;gtk";
assert picker.xdg.portal.config.umbriel."org.freedesktop.impl.portal.ScreenCast" == "umbriel";
assert lib.hasInfix "${chooser}=rummage"
  picker.environment.etc."xdg/xdg-desktop-portal/umbriel-portals.conf".text;
assert lib.elem replacement custom.environment.systemPackages;
assert lib.elem replacement custom.xdg.portal.extraPortals;
assert lib.elem replacement custom.services.dbus.packages;
assert custom.xdg.portal.config.niri.${chooser} == "rummage";
assert custom.xdg.portal.config.common.${chooser} == "rummage";
assert !(custom.xdg.portal.config ? umbriel);
assert !rejected picker;
assert rejected invalid;
assert rejected emptyDesktops;
pkgs.runCommand "rummage-module-check" { } ''
  touch "$out"
''
