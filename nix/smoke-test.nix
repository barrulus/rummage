{
  runCommand,
  package,
  bash,
  binutils,
  lib,
  desktop-file-utils,
  weston,
  dbus,
  python3,
  mesa,
  makeFontsConf,
  dejavu_fonts,
  writeShellScriptBin,
}:
runCommand "rummage-installed-smoke"
  {
    nativeBuildInputs = [
      bash
      binutils
      desktop-file-utils
      weston
      python3
      (writeShellScriptBin "dbus-run-session" ''
        exec ${dbus}/bin/dbus-run-session --dbus-daemon=${dbus}/bin/dbus-daemon --config-file=${dbus}/share/dbus-1/session.conf "$@"
      '')
    ];
    FONTCONFIG_FILE = makeFontsConf { fontDirectories = [ dejavu_fonts ]; };
    __EGL_VENDOR_LIBRARY_FILENAMES = "${mesa}/share/glvnd/egl_vendor.d/50_mesa.json";
    LIBGL_DRIVERS_PATH = "${mesa}/lib/dri";
  }
  ''
    bash ${../scripts/ci}/check-installed.sh ${package} ${package.version}
    python3 ${../scripts/ci}/check-photos-installed.py ${package}/bin/rummage \
      ${lib.optionalString (
        package ? photos
      ) "--module ${package.photos}/libexec/rummage/librummage-photos.so"}
    bash ${../scripts/ci}/check-wayland.sh ${package}
    touch "$out"
  ''
