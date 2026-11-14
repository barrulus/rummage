# PDFium uses ICU's common Unicode/bidi APIs, not locale bundles or i18n.
# Build common + ICU's own empty stubdata from the locked source archive. Static
# PIC archives let the PDFium linker keep only the functions it actually uses.
{
  lib,
  stdenv,
  icu,
}:

stdenv.mkDerivation {
  pname = "icu-pdfium";
  inherit (icu) version src;
  sourceRoot = "icu/source";
  configureFlags = [
    "--enable-static"
    "--disable-shared"
    "--disable-tests"
    "--disable-samples"
    "--disable-tools"
    "--disable-dyload"
    "--disable-plugins"
  ];
  dontDisableStatic = true;
  env = {
    CFLAGS = "-Os -fPIC -ffunction-sections -fdata-sections";
    CXXFLAGS = "-Os -fPIC -ffunction-sections -fdata-sections";
  };
  preConfigure = ''
    substituteInPlace configure --replace-fail /bin/sh ${stdenv.shell}
  '';
  buildPhase = ''
    runHook preBuild
    mkdir -p lib
    make -C stubdata -j"$NIX_BUILD_CORES"
    make -C common -j"$NIX_BUILD_CORES"
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    make -C stubdata install
    make -C common install
    mkdir -p "$out/lib/pkgconfig"
    cat > "$out/lib/pkgconfig/icu-uc.pc" <<EOF
    prefix=$out
    libdir=$out/lib
    includedir=$out/include
    Name: icu-uc
    Description: ICU common with stub data, private to PDFium
    Version: ${icu.version}
    Libs: -L$out/lib -licuuc -licudata
    Cflags: -I$out/include -DU_STATIC_IMPLEMENTATION
    EOF
    install -Dm644 ../LICENSE "$out/share/licenses/icu/LICENSE"
    runHook postInstall
  '';
  meta = {
    description = "Minimal source-built ICU common and stub data for PDFium";
    license = lib.licenses.unicode-30;
    platforms = lib.platforms.linux;
  };
}
