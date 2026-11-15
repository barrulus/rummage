#!/usr/bin/env bash
# Build the private native renderer entirely from the exported, pinned sources.
# Usage: build.sh SOURCE_BUNDLE BUILD_DIRECTORY PREFIX [JOBS]
set -euo pipefail
sources=$(realpath "${1:?}")
mkdir -p "${2:?}" "${3:?}"
work=$(realpath "$2")
prefix=$(realpath "$3")
jobs=${4:-2}
root=$(cd "$(dirname "$0")/../.." && pwd)
export CC=${CC:-gcc} CXX=${CXX:-g++} AR=${AR:-ar} NM=${NM:-nm}
export READELF=${READELF:-readelf}
export CFLAGS="${CFLAGS:-} -fPIC -ffunction-sections -fdata-sections"
export CXXFLAGS="${CXXFLAGS:-} -fPIC -ffunction-sections -fdata-sections"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

cmake_library() {
    local name=$1
    shift
    cmake -S "$sources/$name" -B "$work/$name" -G Ninja \
        -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF "$@"
    cmake --build "$work/$name" --parallel "$jobs"
    cmake --install "$work/$name"
}

cmake_library abseil -DABSL_BUILD_TESTING=OFF -DABSL_ENABLE_INSTALL=ON -DCMAKE_CXX_STANDARD=20
# Resolve Abseil's mutually dependent archives without retaining unused objects.
{
    echo "CREATE $prefix/lib/libabsl_pdfium.a"
    for archive in "$prefix/lib/"libabsl_*.a; do
        [[ "$archive" == "$prefix/lib/libabsl_pdfium.a" ]] || echo "ADDLIB $archive"
    done
    printf 'SAVE\nEND\n'
} > "$work/abseil.mri"
"$AR" -M < "$work/abseil.mri"
cat > "$prefix/lib/pkgconfig/absl_pdfium.pc" <<EOF
Name: absl_pdfium
Description: Private static Abseil for PDFium
Version: 1
Libs: -L$prefix/lib -labsl_pdfium
Cflags: -I$prefix/include
EOF

mkdir -p "$work/icu"
(
    cd "$work/icu"
    "$sources/icu/source/configure" --prefix="$prefix" --libdir="$prefix/lib" \
        --enable-static --disable-shared --disable-tests --disable-samples \
        --disable-tools --disable-dyload --disable-plugins
    mkdir -p lib
    make -C stubdata -j"$jobs"
    make -C common -j"$jobs"
    make -C stubdata install
    make -C common install
)
cat > "$prefix/lib/pkgconfig/icu-uc.pc" <<EOF
Name: icu-uc
Description: Private ICU common and empty stubdata
Version: 78
Libs: -L$prefix/lib -licuuc -licudata
Cflags: -I$prefix/include -DU_STATIC_IMPLEMENTATION
EOF

cmake_library libyuv -DUNIT_TEST=OFF
# libyuv builds both variants. Only the archive is used by the private renderer.
rm -f "$prefix/lib/"libyuv.so*
cat > "$prefix/lib/pkgconfig/libyuv.pc" <<EOF
Name: libyuv
Description: Private static libyuv for PDFium
Version: 1908
Libs: -L$prefix/lib -lyuv -ljpeg
Cflags: -I$prefix/include
EOF
cmake_library openjpeg -DBUILD_CODEC=OFF -DBUILD_TESTING=OFF
cmake_library highway -DHWY_ENABLE_TESTS=OFF -DHWY_ENABLE_EXAMPLES=OFF -DHWY_ENABLE_CONTRIB=OFF
cmake_library simdutf -DSIMDUTF_TESTS=OFF -DSIMDUTF_TOOLS=OFF -DSIMDUTF_BENCHMARKS=OFF
cmake_library googletest -DINSTALL_GTEST=ON

mkdir -p "$work/gn-source"
cp -a "$sources/gn/." "$work/gn-source/"
chmod -R u+w "$work/gn-source"
(
    cd "$work/gn-source"
    # GN's Windows-oriented format suppression conflicts with distro
    # -Werror=format-security. Keep format checking and tolerate upstream warnings.
    python3 - <<'PY'
from pathlib import Path
path = Path('build/gen.py')
text = path.read_text()
if "'-Wno-format'" not in text:
    raise ValueError('Revisit GN format-warning compatibility for this source version')
path.write_text(text.replace("'-Wno-format'", "'-Wformat'"))
PY
    python3 build/gen.py --no-last-commit-position --no-strip --allow-warnings --no-static-libstdc++
    printf '#define LAST_COMMIT_POSITION_NUM %s\n#define LAST_COMMIT_POSITION "%s"\n' \
        "$(cat .nix-files/REV_NUM)" "$(cat .nix-files/REV_SHORT)" > out/last_commit_position.h
    ninja -C out -j"$jobs" gn
)
mkdir -p "$work/pdfium"
cp -a "$sources/pdfium/." "$work/pdfium/"
chmod -R u+w "$work/pdfium"
cd "$work/pdfium"
for patch in "$sources/patches/"*.patch "$root/nix/pdfium-read-error.patch"; do
    patch -p1 < "$patch"
done
python3 "$root/packaging/pdfium/configure.py" "$sources" "$prefix"
export LDFLAGS="${LDFLAGS:-} -Wl,--gc-sections -Wl,--exclude-libs=ALL"
"$work/gn-source/out/gn" gen out/Release --args='
    is_debug=false is_clang=false use_lld=false
    custom_toolchain="//build/toolchain/linux/unbundle:default"
    host_toolchain="//build/toolchain/linux/unbundle:default"
    use_sysroot=false use_custom_libcxx=false use_custom_libcxx_for_host=false
    treat_warnings_as_errors=false optimize_for_size=true
    pdf_enable_v8=false pdf_enable_xfa=false pdf_use_partition_alloc=false
    pdf_use_skia=false pdf_enable_fontations=false pdf_enable_brotli=false
    pdf_bundle_freetype=false use_system_freetype=true
    use_system_libjpeg=true use_system_lcms2=true use_system_libopenjpeg2=true
    use_system_libpng=true use_system_libtiff=true use_system_zlib=true
    use_system_harfbuzz=true'
ninja -C out/Release -j"$jobs" pdfium
install -Dm755 out/Release/libpdfium.so "$prefix/lib/libpdfium.so"
mkdir -p "$prefix/include/pdfium" "$prefix/share/licenses/rummage-pdf"
cp public/*.h "$prefix/include/pdfium/"
for name in pdfium abseil libyuv openjpeg highway simdutf fast-float; do
    for license in "$sources/$name/"LICENSE*; do
        install -m644 "$license" "$prefix/share/licenses/rummage-pdf/$name-$(basename "$license")"
    done
done
install -m644 "$sources/icu/LICENSE" "$prefix/share/licenses/rummage-pdf/icu-LICENSE"
while IFS= read -r -d '' license; do
    relative=${license#"$sources/pdfium/"}
    install -Dm644 "$license" "$prefix/share/licenses/rummage-pdf/$relative"
done < <(find "$sources/pdfium/third_party" -type f \( -iname '*license*' -o -iname 'copying*' \) -print0)
cp "$sources/versions.json" "$prefix/share/licenses/rummage-pdf/source-versions.json"
# Runtime dependencies must be system libraries, never build-directory artifacts.
if readelf -d "$prefix/lib/libpdfium.so" | grep -E 'RPATH|RUNPATH'; then
    echo 'Unexpected build path in PDFium runtime search path' >&2
    exit 1
fi
