#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $EUID == 0 ]] || { echo 'Run native.sh, not this script on the host.' >&2; exit 1; }
format=${1:?}
version=${2:?}
case "$format" in
    deb)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends /packages/rummage_*.deb
        dpkg --verify rummage
        license=/usr/share/doc/rummage/copyright
        ;;
    rpm)
        dnf install -y /packages/rummage-[0-9]*.x86_64.rpm
        rpm --verify rummage
        license=/usr/share/licenses/rummage/LICENSE
        ;;
    tumbleweed)
        # The fresh install container needs the same consistent origin as the build.
        sed -i -E 's@https?://download\.opensuse\.org/@https://downloadcontent.opensuse.org/@g' /etc/zypp/repos.d/*.repo
        zypper --non-interactive refresh
        zypper --non-interactive dup --no-recommends
        zypper --non-interactive install --no-recommends --allow-unsigned-rpm /packages/rummage-[0-9]*.x86_64.rpm
        rpm --verify rummage
        license=/usr/share/licenses/rummage/LICENSE
        ;;
    arch)
        # The minimal image skips documentation; verify the complete package payload.
        sed -i 's@usr/share/doc/\*@@g' /etc/pacman.conf
        pacman -Syu --noconfirm
        pacman -U --noconfirm /packages/rummage-[0-9]*-x86_64.pkg.tar.zst
        pacman -Qkk rummage
        license=/usr/share/licenses/rummage/LICENSE
        ;;
    *) exit 1 ;;
esac
# Check shared libraries before adding tools which could mask a missing dependency.
for executable in /usr/bin/rummage /usr/libexec/rummage/rummage-preview /usr/libexec/rummage/rummage-media /usr/libexec/rummage/rummage-playback /usr/libexec/rummage/rummage-audio /usr/libexec/rummage/rummage-pdf; do
    libraries=$(ldd "$executable")
    printf '%s\n' "$libraries"
    if grep -F 'not found' <<< "$libraries"; then exit 1; fi
done
# Media/PDF belong to the standard package. Photos and font are add-ons.
test ! -e /usr/libexec/rummage/rummage-font
test ! -e /usr/libexec/rummage/librummage-photos.so
/usr/bin/rummage --about
case "$format" in
    deb) apt-get install -y --no-install-recommends /packages/rummage-photos_*.deb /packages/rummage-font_*.deb ;;
    rpm) dnf install -y /packages/rummage-photos-[0-9]*.x86_64.rpm /packages/rummage-font-[0-9]*.x86_64.rpm ;;
    tumbleweed) zypper --non-interactive install --no-recommends --allow-unsigned-rpm /packages/rummage-photos-[0-9]*.x86_64.rpm /packages/rummage-font-[0-9]*.x86_64.rpm ;;
    arch) pacman -U --noconfirm /packages/rummage-photos-[0-9]*-x86_64.pkg.tar.zst /packages/rummage-font-[0-9]*-x86_64.pkg.tar.zst ;;
esac
libraries=$(ldd /usr/libexec/rummage/librummage-photos.so)
printf '%s\n' "$libraries"
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
libraries=$(ldd /usr/libexec/rummage/rummage-pdf)
printf '%s\n' "$libraries"
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
grep -F '/usr/libexec/rummage/libpdfium.so' <<< "$libraries"
test -s /usr/share/rummage/engines/pdf.toml
test -s /usr/share/licenses/rummage-pdf/pdfium-LICENSE
case "$format" in
    deb) apt-get install -y --no-install-recommends python3 ;;
    rpm) dnf install -y python3 ;;
    tumbleweed) zypper --non-interactive install --no-recommends python3 ;;
    arch) pacman -S --noconfirm --needed python ;;
esac
python3 /source/scripts/ci/check-pdf.py /usr/libexec/rummage/rummage-pdf /source/tests/data/pdf
case "$format" in
    deb) apt-get install -y --no-install-recommends desktop-file-utils binutils ;;
    rpm) dnf install -y desktop-file-utils binutils ;;
    tumbleweed) zypper --non-interactive install --no-recommends desktop-file-utils binutils ;;
    arch) pacman -S --noconfirm --needed desktop-file-utils binutils ;;
esac
bash /source/scripts/ci/check-installed.sh /usr "$version" "$license"
python3 /source/scripts/ci/check-photos-installed.py /usr/bin/rummage --module /usr/libexec/rummage/librummage-photos.so
case "$format" in
    deb) apt-get install -y --no-install-recommends weston libgl1-mesa-dri dbus fonts-dejavu-core ;;
    rpm) dnf install -y weston mesa-dri-drivers dbus-daemon dejavu-sans-fonts ;;
    tumbleweed) zypper --non-interactive install --no-recommends weston Mesa-dri dbus-1-daemon dejavu-fonts ;;
    arch) pacman -S --noconfirm --needed weston mesa dbus ttf-dejavu ;;
esac
bash /source/scripts/ci/check-wayland.sh /usr

# Removing Photo Library leaves the launcher and previews intact.
case "$format" in
    deb) apt-get remove -y rummage-photos ;;
    rpm) dnf remove -y rummage-photos ;;
    tumbleweed) zypper --non-interactive remove rummage-photos ;;
    arch) pacman -R --noconfirm rummage-photos ;;
esac
test ! -e /usr/libexec/rummage/librummage-photos.so
python3 /source/scripts/ci/check-photos-installed.py /usr/bin/rummage
test -x /usr/libexec/rummage/rummage-media
test -x /usr/libexec/rummage/rummage-playback
test -x /usr/libexec/rummage/rummage-audio
test -x /usr/libexec/rummage/rummage-pdf

libraries=$(ldd /usr/libexec/rummage/rummage-font)
if grep -F 'not found' <<< "$libraries"; then exit 1; fi
test -s /usr/share/rummage/engines/font.toml
python3 /source/scripts/ci/check-font.py /usr/libexec/rummage/rummage-font "$(fc-match -f '%{file}' sans)"

for executable in /usr/libexec/rummage/rummage-playback /usr/libexec/rummage/rummage-audio; do
    libraries=$(ldd "$executable")
    if grep -F 'not found' <<< "$libraries"; then exit 1; fi
done
python3 /source/scripts/ci/check-playback.py /usr/libexec/rummage/rummage-playback /source/tests/data/media
