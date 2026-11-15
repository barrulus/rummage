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
        if cfg.preview.thumbnails || cfg.preview.pdf then
          package.override (
            lib.optionalAttrs cfg.preview.thumbnails { withMedia = true; }
            // lib.optionalAttrs cfg.preview.pdf { withPdf = true; }
          )
        else
          package;
      description = ''
        rummage package to install and use for the file picker. Enabled preview
        options override this package's withMedia and withPdf arguments; a custom
        package must support those arguments. Features already enabled in the
        selected package are preserved.
      '';
    };

    preview = {
      thumbnails =
        lib.mkEnableOption "video thumbnails and still previews through the FFmpeg-based media engine"
        // {
          description = ''
            Enable the optional media engine for video thumbnails and still previews
            in the information pane and Quick Look. Image thumbnails and image/text
            previews are built in and do not require this option.
          '';
        };

      pdf = lib.mkEnableOption "PDF thumbnails and first-page previews through the PDFium-based PDF engine";
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
