#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $(id -u) == 0 ]]
xbps-install -Syu xbps
xbps-install -yu
xbps-install -y -R /packages rummage
xbps-pkgdb rummage
test ! -e /usr/libexec/rummage/librummage-photos.so
# Check linked runtime dependencies before adding compositor/test dependencies.
for executable in /usr/bin/rummage /usr/libexec/rummage/rummage-preview /usr/libexec/rummage/rummage-media /usr/libexec/rummage/rummage-pdf; do
    dependencies=$(ldd "$executable")
    printf '%s\n' "$dependencies"
    if grep -F 'not found' <<< "$dependencies"; then exit 1; fi
done
xbps-install -y desktop-file-utils binutils
bash /source/scripts/ci/check-installed.sh /usr "$1"
xbps-install -y weston mesa-dri dbus dejavu-fonts-ttf python3
bash /source/scripts/ci/check-wayland.sh /usr

# Optional siblings own no launcher.
test ! -e /usr/libexec/rummage/rummage-font
python3 /source/scripts/ci/check-photos-installed.py /usr/bin/rummage
xbps-install -y -R /packages rummage-photos rummage-font
python3 /source/scripts/ci/check-photos-installed.py /usr/bin/rummage --module /usr/libexec/rummage/librummage-photos.so
libraries=$(ldd /usr/libexec/rummage/librummage-photos.so)
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
libraries=$(ldd /usr/libexec/rummage/rummage-pdf)
printf '%s\n' "$libraries"
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
grep -F '/usr/libexec/rummage/libpdfium.so' <<< "$libraries"
test -s /usr/share/rummage/engines/pdf.toml
test -s /usr/share/licenses/rummage-pdf/pdfium-LICENSE
python3 /source/scripts/ci/check-pdf.py /usr/libexec/rummage/rummage-pdf /source/tests/data/pdf

xbps-remove -y rummage-photos
test ! -e /usr/libexec/rummage/librummage-photos.so
python3 /source/scripts/ci/check-photos-installed.py /usr/bin/rummage
test -x /usr/libexec/rummage/rummage-media
test -x /usr/libexec/rummage/rummage-pdf

libraries=$(ldd /usr/libexec/rummage/rummage-font)
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
test -s /usr/share/rummage/engines/font.toml
python3 /source/scripts/ci/check-font.py /usr/libexec/rummage/rummage-font "$(fc-match -f '%{file}' sans)"
