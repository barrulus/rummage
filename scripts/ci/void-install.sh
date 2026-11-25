#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $(id -u) == 0 ]]
xbps-install -Syu xbps
xbps-install -yu
xbps-install -y -R /packages rummage
xbps-pkgdb rummage
test ! -e /usr/libexec/rummage/rummage-media
test ! -e /usr/libexec/rummage/rummage-pdf
test ! -e /usr/libexec/rummage/libpdfium.so
# Check linked runtime dependencies before adding compositor/test dependencies.
for executable in /usr/bin/rummage /usr/libexec/rummage/rummage-preview; do
    dependencies=$(ldd "$executable")
    printf '%s\n' "$dependencies"
    if grep -F 'not found' <<< "$dependencies"; then exit 1; fi
done
xbps-install -y desktop-file-utils
bash /source/scripts/ci/check-installed.sh /usr "$1"
xbps-install -y weston mesa-dri dbus dejavu-fonts-ttf python3
bash /source/scripts/ci/check-wayland.sh /usr

# Install the optional sibling after checking the base package independently.
xbps-install -y -R /packages rummage-media
ldd /usr/libexec/rummage/rummage-media > /tmp/rummage-media-libraries
if grep -F 'not found' /tmp/rummage-media-libraries; then exit 1; fi

test ! -e /usr/libexec/rummage/rummage-pdf
xbps-install -y -R /packages rummage-previews
libraries=$(ldd /usr/libexec/rummage/rummage-pdf)
printf '%s\n' "$libraries"
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
grep -F '/usr/libexec/rummage/libpdfium.so' <<< "$libraries"
test -s /usr/share/rummage/engines/pdf.toml
test -s /usr/share/licenses/rummage-pdf/pdfium-LICENSE
python3 /source/scripts/ci/check-pdf.py /usr/libexec/rummage/rummage-pdf /source/tests/data/pdf
