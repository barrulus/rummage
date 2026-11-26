{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  makeWrapper,
  wayland,
  wayland-scanner,
  wayland-protocols,
  libglvnd,
  libxkbcommon,
  harfbuzz,
  freetype,
  fontconfig,
  tomlplusplus,
  nlohmann_json,
  tokei,
  libwebp,
  ffmpeg,
  sqlite,
  callPackage,
  libjxl,
  libheif,
  libde265,
  lcms2,
  librsvg,
  pango,
  zlib,
  re2,
  sdbus-cpp_2,
  shared-mime-info,
  hicolor-icon-theme,
  doctest,
  python3,
  dbus,
  dejavu_fonts,
  makeFontsConf,
  mesa,
  _7zz,
  unzip,
  writeShellScriptBin,
  withTests ? false,
  withPhotos ? false,
  rummage-pdfium ? callPackage ./pdfium.nix { },
}:

stdenv.mkDerivation {
  pname = "rummage";
  version = "0.6.1";
  outputs = [ "out" ] ++ lib.optional withPhotos "photos";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions (
      [
        ../CMakeLists.txt
        ../LICENSE
        ../cmake
        ../data
        ../docs/config.example.toml
        ../src
        # The preview helper embeds these images to warm up codecs before sandboxing.
        ../tests/data/image
        # The development SVG renderer shares its comparison helper with the tests.
        ../tests/svg_compare.hpp
        ../third_party
      ]
      ++ lib.optional withTests ../tests
    );
  };

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
    makeWrapper
    wayland-scanner
    tokei
  ]
  ++ lib.optionals withTests [
    doctest
    python3
    (writeShellScriptBin "dbus-run-session" ''
      exec ${dbus}/bin/dbus-run-session --dbus-daemon=${dbus}/bin/dbus-daemon --config-file=${dbus}/share/dbus-1/session.conf "$@"
    '')
  ];

  buildInputs = [
    wayland
    wayland-protocols
    libglvnd
    libxkbcommon
    harfbuzz
    freetype
    fontconfig
    tomlplusplus
    nlohmann_json
    libwebp
    libjxl
    # nixpkgs includes the built-in libde265 HEVC decoder in libheif.
    # Sandboxed helper tests verify that it is available at runtime.
    libheif
    libde265
    lcms2
    librsvg
    pango
    zlib
    re2
    sdbus-cpp_2
  ]
  ++ lib.optional withTests shared-mime-info
  ++ [
    ffmpeg
    rummage-pdfium
  ]
  ++ lib.optional withPhotos sqlite;

  cmakeFlags = [
    "-DRUMMAGE_WERROR=OFF"
    "-DRUMMAGE_MEDIA=ON"
    "-DRUMMAGE_PDF=ON"
    "-DRUMMAGE_PHOTOS=${if withPhotos then "ON" else "OFF"}"
    "-DRUMMAGE_BUILD_TESTS=${if withTests then "ON" else "OFF"}"
  ];

  doCheck = withTests;
  nativeCheckInputs = lib.optionals withTests [
    _7zz
    unzip
    (writeShellScriptBin "7z" ''exec ${_7zz}/bin/7zz "$@"'')
  ];
  # These two fixtures create executable scripts at runtime, after patchShebangs runs.
  postPatch = lib.optionalString withTests ''
    substituteInPlace tests/about_test.cpp tests/platform_test.cpp \
      --replace-fail '#!/bin/sh' '#!${stdenv.shell}'
  '';
  checkPhase = ''
    runHook preCheck
    export FONTCONFIG_FILE=${
      makeFontsConf {
        fontDirectories = [ dejavu_fonts ];
        includes = [ "${fontconfig.out}/share/fontconfig/conf.avail/60-latin.conf" ];
      }
    }
    export XDG_DATA_DIRS=${shared-mime-info}/share:${hicolor-icon-theme}/share
    export LIBGL_ALWAYS_SOFTWARE=1
    export EGL_PLATFORM=surfaceless
    export __EGL_VENDOR_LIBRARY_FILENAMES=${mesa}/share/glvnd/egl_vendor.d/50_mesa.json
    export LIBGL_DRIVERS_PATH=${mesa}/lib/dri
    ctest --show-only | grep -F file-picker-portal
    ctest --show-only | grep -F action-notifications
    ctest --output-on-failure --timeout 180
    runHook postCheck
  '';

  postInstall =
    lib.optionalString withPhotos ''
      mkdir -p "$photos/libexec/rummage"
      mv "$out/libexec/rummage/librummage-photos.so" "$photos/libexec/rummage/"
    ''
    + ''
      install -Dm644 "$src/LICENSE" "$out/share/licenses/rummage/LICENSE"
      wrapProgram "$out/bin/rummage" \
        ${lib.optionalString withPhotos "--set RUMMAGE_PHOTOS_LIBRARY $photos/libexec/rummage/librummage-photos.so"} \
        --set-default XDG_DATA_DIRS /usr/local/share:/usr/share \
        --suffix XDG_DATA_DIRS : "${
          lib.makeSearchPath "share" [
            shared-mime-info
            hicolor-icon-theme
          ]
        }"
    '';

  meta = {
    description = "Native file manager for Wayland";
    homepage = "https://github.com/barrulus/rummage";
    license = lib.licenses.mit;
    mainProgram = "rummage";
    platforms = [
      "x86_64-linux"
      "aarch64-linux"
    ];
  };
}
