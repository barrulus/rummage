# Source-only bundle for native distro builds. Nothing compiled by Nix is shipped
# in a native package; the exported source archive builds without Nix or network.
{ pkgs }:
let
  inherit (pkgs) lib;
  recipe = pkgs.path + /pkgs/by-name/pd/pdfium;
  pins = builtins.fromJSON (builtins.readFile (recipe + /sources.json));
  shims = pkgs.fetchzip {
    url = "https://chromium.googlesource.com/chromium/src/+archive/refs/branch-heads/${toString pins.version.build}/tools/generate_shim_headers.tar.gz";
    inherit (pins.generateShimHeaders) hash;
    stripRoot = false;
  };
  sources = {
    inherit (pkgs)
      gn
      icu
      libyuv
      openjpeg
      ;
    abseil = pkgs.abseil-cpp;
    fast-float = pkgs.fast-float;
    highway = pkgs.libhwy;
    simdutf = pkgs.simdutf;
    googletest = pkgs.gtest;
  };
in
pkgs.runCommand "rummage-pdfium-sources-${pkgs.pdfium.version}"
  {
    nativeBuildInputs = [
      pkgs.python3
      pkgs.gnutar
      pkgs.gzip
      pkgs.xz
      pkgs.bzip2
    ];
  }
  ''
    mkdir -p "$out"
    python3 - ${pkgs.pdfium.gclientDeps} "$out" <<'PY'
    import json, pathlib, shutil, subprocess, sys
    for name, source in json.load(open(sys.argv[1])).items():
        target = pathlib.Path(sys.argv[2]) / name.replace('src', 'pdfium', 1)
        shutil.copytree(source['path'], target, dirs_exist_ok=True)
        subprocess.run(['chmod', '-R', 'u+w', str(target)], check=True)
    PY
    unpack_source() {
      mkdir -p "$out/$2"
      if test -d "$1"; then
        cp -R "$1"/. "$out/$2/"
      else
        tar -xf "$1" --strip-components=1 -C "$out/$2"
      fi
    }
    ${lib.concatStringsSep "\n" (
      lib.mapAttrsToList (name: package: "unpack_source ${package.src} ${name}") sources
    )}
    mkdir -p "$out/patches" "$out/pdfium/tools/generate_shim_headers"
    cp ${shims}/generate_shim_headers.py "$out/pdfium/tools/generate_shim_headers/"
    ${lib.concatMapStringsSep "\n" (patch: ''cp ${patch} "$out/patches/"'') pkgs.pdfium.patches}
    cp ${recipe}/system-abseil-cpp.gn ${recipe}/system-googletest.gn "$out/"
    cp ${recipe}/sources.json "$out/pdfium-pins.json"
    # Preserve source/package versions and license provenance in the source bundle.
    cat > "$out/versions.json" <<'EOF'
    ${builtins.toJSON (
      lib.mapAttrs (_: package: package.version) (sources // { pdfium = pkgs.pdfium; })
    )}
    EOF
    chmod -R u+w "$out"
  ''
