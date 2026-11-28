#!/usr/bin/env bash
set -euo pipefail
[[ -f /.dockerenv && $EUID == 0 ]]
export XDG_CONFIG_HOME=/test/config XDG_DATA_HOME=/test/data XDG_STATE_HOME=/test/state XDG_CACHE_HOME=/test/cache
mkdir -p "$XDG_CONFIG_HOME/rummage" "$XDG_DATA_HOME/rummage/photos"
printf '[photos]\nenabled = true\n' > "$XDG_CONFIG_HOME/rummage/config.toml"
sha256sum "$XDG_CONFIG_HOME/rummage/config.toml" > /test/config.sha256
# Genuine previously built 0.6.0 packages, not relabelled current payloads.
dnf install -y /old/rummage-0*.x86_64.rpm /old/rummage-media-0*.x86_64.rpm /old/rummage-pdf-0*.x86_64.rpm /old/rummage-previews-0*.x86_64.rpm
rpm -q rummage rummage-media rummage-pdf rummage-previews
rpm --verify rummage rummage-media rummage-pdf rummage-previews
/usr/bin/rummage --version
for ((attempt=0; attempt<3600; ++attempt)); do
    [[ -f /packages/.ready ]] && break
    sleep 1
done
test -f /packages/.ready
# Passing the new base package to DNF must upgrade it and obsolete all preview add-ons.
dnf install -y /packages/rummage-[0-9]*.x86_64.rpm
rpm --verify rummage
for obsolete in rummage-media rummage-pdf rummage-previews; do
    if rpm -q "$obsolete"; then echo "Obsolete preview package remains: $obsolete" >&2; exit 1; fi
done
for exe in rummage-preview rummage-media rummage-pdf; do test -x "/usr/libexec/rummage/$exe"; done
test ! -e /usr/libexec/rummage/librummage-photos.so
/usr/bin/rummage --version
sha256sum -c /test/config.sha256
# Install the sole optional add-on and exercise real catalog writes before removing it.
dnf install -y /packages/rummage-photos-[0-9]*.x86_64.rpm python3
rpm --verify rummage rummage-photos
python3 - <<'PY'
import importlib.util,pathlib
p=pathlib.Path('/checks/check-photos-installed.py')
spec=importlib.util.spec_from_file_location('installed',p)
m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
root=pathlib.Path('/test/fixture'); root.mkdir()
m.exercise(pathlib.Path('/usr/libexec/rummage/librummage-photos.so'),root)
PY
sha256sum /test/fixture/catalog/catalog.sqlite3 > /test/catalog.sha256
rpm -ql rummage > /test/base-files.txt
rpm -ql rummage-photos > /test/photos-files.txt
dnf remove -y rummage-photos
test ! -e /usr/libexec/rummage/librummage-photos.so
rpm --verify rummage
sha256sum -c /test/catalog.sha256 /test/config.sha256
python3 /checks/check-photos-installed.py /usr/bin/rummage
for exe in rummage-preview rummage-media rummage-pdf; do test -x "/usr/libexec/rummage/$exe"; done
# Reinstallation restores the addon without losing the existing annotations.
dnf install -y /packages/rummage-photos-[0-9]*.x86_64.rpm
sha256sum -c /test/catalog.sha256
python3 - <<'PY'
import ctypes,json
m=ctypes.CDLL('/usr/libexec/rummage/librummage-photos.so')
m.rummage_photos_request.argtypes=[ctypes.c_char_p,ctypes.c_char_p]
m.rummage_photos_request.restype=ctypes.c_void_p
m.rummage_photos_free.argtypes=[ctypes.c_void_p]
p=m.rummage_photos_request(b'/test/fixture/catalog/catalog.sqlite3',json.dumps({'op':'query','text':'旅行'}).encode())
assert p
try:r=json.loads(ctypes.string_at(p))
finally:m.rummage_photos_free(p)
assert r['ok'] and len(r['photos'])==1,r
row=r['photos'][0]
assert row['description']=='Voyage 日本' and row['rating']==4,row
print('Reinstalled photo module recovered existing Unicode annotations and rating')
PY
# Full application removal also must leave user-owned catalog/config untouched.
sha256sum /test/fixture/catalog/catalog.sqlite3 > /test/catalog-after-reopen.sha256
dnf remove -y rummage rummage-photos
test ! -e /usr/bin/rummage
test ! -e /usr/libexec/rummage/librummage-photos.so
sha256sum -c /test/catalog-after-reopen.sha256 /test/config.sha256
printf 'REAL FEDORA UPGRADE, PREVIEW CONSOLIDATION, ADDON REMOVAL/REINSTALL, AND FULL REMOVAL PASSED\n'
