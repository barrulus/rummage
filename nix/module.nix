self:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.programs.rummage;
in
{
  options.programs.rummage = {
    enable = lib.mkEnableOption "the rummage file manager";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "inputs.rummage.packages.\${pkgs.stdenv.hostPlatform.system}.default";
      apply =
        package:
        if cfg.photos.enable || cfg.preview.font || cfg.preview.model then
          package.override (
            lib.optionalAttrs cfg.photos.enable { withPhotos = true; }
            // lib.optionalAttrs cfg.preview.font { withFont = true; }
            // lib.optionalAttrs cfg.preview.model { withModel = true; }
          )
        else
          package;
      description = ''
        rummage package, including audio/video playback and PDF previews. Enabled
        Photo Library, font and model options override withPhotos, withFont and
        withModel; a custom package must support those arguments. Features already
        enabled in the selected package are preserved. Preview features can be
        switched off in the application's Settings.
      '';
    };

    photos.enable = lib.mkEnableOption "the optional Photo Library add-on";

    preview = {
      font = lib.mkEnableOption "font specimens through the FreeType font engine";
      model = lib.mkEnableOption "standalone model geometry previews through the Assimp engine";
    };

    filePicker = {
      enable = lib.mkEnableOption "rummage as the XDG FileChooser portal backend";

      desktops = lib.mkOption {
        type = lib.types.listOf lib.types.str;
        default = [ "umbriel" ];
        example = [
          "umbriel"
          "niri"
        ];
        description = ''
          Lowercase desktop names whose portal configuration should select rummage
          for file selection. Use "common" for the desktop-independent portals.conf.
          Keep the other portal preferences in xdg.portal.config for these desktops:
          desktop-specific configurations take precedence over common and package
          configurations.
        '';
      };
    };
  };

  config = lib.mkMerge [
    {
      assertions = [
        {
          assertion = cfg.filePicker.enable -> cfg.enable;
          message = "programs.rummage.filePicker.enable requires programs.rummage.enable.";
        }
        {
          assertion = cfg.filePicker.enable -> cfg.filePicker.desktops != [ ];
          message = "programs.rummage.filePicker.desktops must select at least one desktop (or common).";
        }
      ];
    }
    (lib.mkIf cfg.enable {
      environment.systemPackages = [ cfg.package ];
    })
    (lib.mkIf (cfg.enable && cfg.filePicker.enable) {
      xdg.portal = {
        enable = true;
        extraPortals = [ cfg.package ];
        config = lib.genAttrs cfg.filePicker.desktops (_: {
          "org.freedesktop.impl.portal.FileChooser" = [ "rummage" ];
        });
      };
    })
  ];
}
