#!/bin/sh
# Makes the archive fixtures of tests/archive_test.cpp with real tools (GNU tar 1.35, gzip, 7-Zip
# 17+, Python's zipfile). Run from anywhere: sh tests/data/archive/make-fixtures.sh
# Owners are zeroed and times fixed, so the archives depend only on what is written here (the
# encrypted zips excepted: 7-Zip salts them at random).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

mkdir -p tree/docs
printf 'hello from a\n' > tree/a.txt
printf 'second file\n' > tree/docs/b.txt
printf 'accents\n' > 'tree/città.txt'
i=0; while [ $i -lt 256 ]; do printf 'the same line again\n'; i=$((i + 1)); done > tree/docs/repeat.txt
long=$(printf 'n%.0s' $(seq 1 120))
printf 'a long name\n' > "tree/docs/$long.txt"
ln -s a.txt tree/link-to-a
find tree -exec touch -h -d '2026-11-15 12:00:00 UTC' {} +

common="--owner=0 --group=0 --numeric-owner --sort=name --mtime=2026-11-15T12:00:00Z"
# pax: no process number or access times in the extended headers, so the archives repeat.
pax="--pax-option=exthdr.name=%d/PaxHeaders/%f,delete=atime,delete=ctime"
(cd tree && tar $common --format=gnu -cf "$here/gnu.tar" .)
(cd tree && tar $common $pax --format=pax -cf "$here/pax.tar" .)
(cd tree && tar $common $pax --format=pax -cf - . | gzip -n -9 > "$here/pax.tar.gz")
printf 'just one file\n' > notes.txt
touch -d '2026-11-15 12:00:00 UTC' notes.txt
gzip -9 -c notes.txt > "$here/notes.txt.gz"

rm -f "$here"/*.zip
(cd tree && 7z a -tzip -mx=9 "$here/basic.zip" a.txt docs 'città.txt' > /dev/null)
(cd tree/docs && 7z a -tzip -mm=BZip2 "$here/bzip2.zip" repeat.txt > /dev/null)
(cd tree && 7z a -tzip -prummage -mem=AES256 "$here/aes.zip" a.txt > /dev/null)
(cd tree && 7z a -tzip -prummage -mem=ZipCrypto "$here/zipcrypto.zip" a.txt > /dev/null)
(cd "$here/../image" && 7z a -tzip "$here/pictures.zip" blocks.png shot.jpg > /dev/null)
python3 -I - "$here/symlink.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    info = zipfile.ZipInfo("link-to-a", date_time=(2026, 11, 15, 12, 0, 0))
    info.create_system = 3
    info.external_attr = (0o120777 << 16)
    z.writestr(info, "a.txt")
    plain = zipfile.ZipInfo("a.txt", date_time=(2026, 11, 15, 12, 0, 0))
    plain.create_system = 3
    plain.external_attr = (0o100644 << 16)
    z.writestr(plain, "hello from a\n", compress_type=zipfile.ZIP_DEFLATED)
PY
ls -l "$here"
