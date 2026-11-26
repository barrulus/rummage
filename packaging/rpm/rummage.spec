Name:           rummage
Version:        0.7.0
Release:        1%{?dist}
Summary:        Native Wayland file manager
License:        MIT AND Apache-2.0 AND BSD-3-Clause AND BSD-2-Clause AND Unicode-3.0
URL:            https://github.com/barrulus/rummage
Source0:        rummage-source.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  python3
BuildRequires:  patch
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(libjpeg)
BuildRequires:  pkgconfig(libpng)
BuildRequires:  pkgconfig(libtiff-4)
%if 0%{?suse_version}
# openSUSE packages the GLES 3 headers separately from pkgconfig(glesv2).
BuildRequires:  Mesa-libGLESv3-devel
BuildRequires:  ninja
%else
BuildRequires:  ninja-build
%endif
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-egl)
BuildRequires:  pkgconfig(wayland-cursor)
BuildRequires:  pkgconfig(wayland-scanner)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(egl)
BuildRequires:  pkgconfig(glesv2)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(harfbuzz)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(tomlplusplus)
BuildRequires:  cmake(nlohmann_json)
BuildRequires:  pkgconfig(re2)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(libwebpdemux)
BuildRequires:  pkgconfig(libjxl)
BuildRequires:  pkgconfig(libheif) >= 1.23
BuildRequires:  pkgconfig(lcms2)
BuildRequires:  pkgconfig(librsvg-2.0) >= 2.52
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pangoft2)
BuildRequires:  pkgconfig(libavformat) >= 60.16.100
BuildRequires:  pkgconfig(libavcodec) >= 60.31.102
BuildRequires:  pkgconfig(libavutil) >= 58.29.100
BuildRequires:  pkgconfig(libswscale)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(sdbus-c++) >= 2
# The decoder uses versioned option fields, so require the same minimum at runtime.
%if 0%{?suse_version}
Requires:       libheif1 >= 1.23
%else
Requires:       libheif >= 1.23
%endif
Requires:       shared-mime-info
Requires:       hicolor-icon-theme
Obsoletes:      rummage-media <= %{version}-%{release}
Obsoletes:      rummage-pdf <= %{version}-%{release}
Obsoletes:      rummage-previews <= %{version}-%{release}
# Private renderer is not a public ABI.
%global __provides_exclude_from ^%{_libexecdir}/rummage/(libpdfium|librummage-photos)\.so$
%global __requires_exclude ^libpdfium\.so.*$

%description
A file manager with column and grid views, isolated previews, configurable
custom actions and an optional desktop file chooser portal.

%package photos
Summary:        Photo Library for rummage
Requires:       %{name} = %{version}-%{release}

%description photos
Optional local photo catalog and organisation. Originals remain in place.
Removing this package preserves the user's catalog and annotations.

%package font
Summary:        Optional font specimens for rummage
Requires:       %{name} = %{version}-%{release}

%description font
Isolated scalable TrueType/OpenType specimens using system FreeType.

%prep
%autosetup -n rummage-checkout

%build
%if 0%{?suse_version}
%define __builder ninja
%endif
%cmake -G Ninja -DRUMMAGE_FONT=ON -DRUMMAGE_PHOTOS=ON -DRUMMAGE_MEDIA=ON -DRUMMAGE_PDF=ON -DRUMMAGE_PRIVATE_PDFIUM=ON -DBUILD_SHARED_LIBS=OFF -DRUMMAGE_BUILD_TESTS=OFF -DRUMMAGE_WERROR=OFF
%if 0%{?suse_version}
%cmake_build rummage rummage-preview rummage-media rummage-pdf rummage-font rummage_photos
%else
%cmake_build --target rummage rummage-preview rummage-media rummage-pdf rummage-font rummage_photos
%endif

%install
%cmake_install
# Preserve bundled license notices under distinct names.
install -Dm644 third_party/tabler-icons/LICENSE %{buildroot}%{_datadir}/licenses/%{name}/tabler-icons-LICENSE
install -Dm644 third_party/fzy/LICENSE %{buildroot}%{_datadir}/licenses/%{name}/fzy-LICENSE
install -Dm644 third_party/wuffs/LICENSE-MIT %{buildroot}%{_datadir}/licenses/%{name}/wuffs-LICENSE-MIT
install -Dm644 third_party/wuffs/LICENSE-APACHE %{buildroot}%{_datadir}/licenses/%{name}/wuffs-LICENSE-APACHE

%files
%license LICENSE
%license %{_datadir}/licenses/%{name}/*-LICENSE*
%doc THIRD_PARTY.md
%{_bindir}/rummage
%dir %{_libexecdir}/rummage
%{_libexecdir}/rummage/rummage-preview
%{_libexecdir}/rummage/rummage-media
%{_libexecdir}/rummage/rummage-pdf
%{_libexecdir}/rummage/libpdfium.so
%license %{_datadir}/licenses/rummage-pdf/
%{_datadir}/rummage/
%exclude %{_datadir}/rummage/engines/font.toml
%{_datadir}/applications/rummage.desktop
%{_datadir}/icons/hicolor/scalable/apps/rummage.svg
%{_datadir}/xdg-desktop-portal/portals/rummage.portal
%{_datadir}/dbus-1/services/org.freedesktop.impl.portal.desktop.rummage.service

%{_datadir}/bash-completion/completions/rummage
%{_datadir}/zsh/site-functions/_rummage
%{_datadir}/fish/vendor_completions.d/rummage.fish

%files photos
%license LICENSE
%{_libexecdir}/rummage/librummage-photos.so

%files font
%license LICENSE
%{_libexecdir}/rummage/rummage-font
%{_datadir}/rummage/engines/font.toml
