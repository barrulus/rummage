# Use the source recipe and hash-pinned sources from our locked nixpkgs revision.
# Keep the upstream unit/embedder tests. Private static dependencies are linked
# selectively; the application already supplies the shared font/colour codecs.
{
  lib,
  callPackage,
  pdfium,
  abseil-cpp,
  libyuv,
  openjpeg,
}:

let
  icu = callPackage ./pdfium-icu.nix { };
  abseil = abseil-cpp.overrideAttrs (old: {
    cmakeFlags = lib.filter (flag: !(lib.hasPrefix "-DBUILD_SHARED_LIBS" flag)) old.cmakeFlags ++ [
      "-DBUILD_SHARED_LIBS=OFF"
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      "-DCMAKE_BUILD_TYPE=MinSizeRel"
    ];
    env = (old.env or { }) // {
      NIX_CFLAGS_COMPILE = "-ffunction-sections -fdata-sections";
    };
    postInstall = (old.postInstall or "") + ''
      # Abseil has mutually dependent archives. One indexed archive lets the
      # linker resolve them without whole-archive inclusion or ordering tricks.
      echo "CREATE $out/lib/libabsl_pdfium.a" > merge.mri
      for archive in "$out/lib/"libabsl_*.a; do
        echo "ADDLIB $archive" >> merge.mri
      done
      printf 'SAVE\nEND\n' >> merge.mri
      "$AR" -M < merge.mri
      cat > "$out/lib/pkgconfig/absl_pdfium.pc" <<EOF
      Name: absl_pdfium
      Description: Private static Abseil archive for PDFium
      Version: ${abseil-cpp.version}
      Libs: -L$out/lib -labsl_pdfium
      Cflags: -I$dev/include
      EOF
    '';
  });
  yuv = libyuv.overrideAttrs (old: {
    cmakeFlags = old.cmakeFlags ++ [
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      "-DCMAKE_BUILD_TYPE=MinSizeRel"
    ];
    env = (old.env or { }) // {
      NIX_CFLAGS_COMPILE = "-ffunction-sections -fdata-sections";
    };
    # Upstream builds both variants; expose only the archive to PDFium's linker.
    postInstall = (old.postInstall or "") + ''
      rm "$out/lib/"libyuv.so*
      install -Dm644 ../LICENSE "$out/share/licenses/libyuv/LICENSE"
    '';
  });
  jpeg2000 = openjpeg.overrideAttrs (old: {
    cmakeFlags = lib.filter (flag: !(lib.hasPrefix "-DBUILD_SHARED_LIBS" flag)) old.cmakeFlags ++ [
      "-DBUILD_SHARED_LIBS=OFF"
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      "-DCMAKE_BUILD_TYPE=MinSizeRel"
    ];
    env = (old.env or { }) // {
      NIX_CFLAGS_COMPILE = "-ffunction-sections -fdata-sections";
    };
  });
in
(pdfium.override {
  inherit icu;
  abseil-cpp = abseil;
  libyuv = yuv;
  openjpeg = jpeg2000;
}).overrideAttrs
  (old: {
    # A custom file callback may refuse a read (our byte/time budget). PDFium 8110
    # CHECKs that read instead of propagating the error. Keep it a parse failure so
    # the helper can report exhaustion rather than killing the worker. Revisit this
    # small compatibility patch when updating the locked PDFium source revision.
    patches = old.patches ++ [ ./pdfium-read-error.patch ];
    postPatch = old.postPatch + ''
        cp ${./pdfium-icu.gn} third_party/icu/BUILD.gn
      substituteInPlace third_party/abseil-cpp/BUILD.gn \
        --replace-fail '"absl_cleanup"' '"absl_pdfium"' \
        --replace-fail '"absl_flat_hash_set",' "" \
        --replace-fail '"absl_inlined_vector",' ""
    '';
    # V8 and PartitionAlloc are already disabled by nixpkgs. State the XFA choice
    # explicitly too: the helper renders ordinary PDF/AcroForm appearances only.
    gnFlags = old.gnFlags ++ [
      "pdf_enable_xfa=false"
      "pdf_use_skia=false"
      "pdf_enable_fontations=false"
      "pdf_enable_brotli=false"
      "optimize_for_size=true"
    ];
    env = (old.env or { }) // {
      NIX_LDFLAGS = "--gc-sections --exclude-libs=ALL";
    };
    postInstall = (old.postInstall or "") + ''
      install -Dm644 ../../LICENSE "$out/share/licenses/pdfium/LICENSE"
      install -Dm644 ${icu}/share/licenses/icu/LICENSE "$out/share/licenses/pdfium/ICU"
      install -Dm644 ${abseil.src}/LICENSE "$out/share/licenses/pdfium/Abseil"
      install -Dm644 ${yuv}/share/licenses/libyuv/LICENSE "$out/share/licenses/pdfium/libyuv"
      install -Dm644 ${jpeg2000.src}/LICENSE "$out/share/licenses/pdfium/OpenJPEG"
    '';
    # Keep a future recipe update from accidentally retaining private archives or
    # whole shared-library packages through an RPATH or a compiled-in store path.
    outputChecks.out.disallowedRequisites = [
      icu
      abseil
      yuv
      jpeg2000
    ];
  })
