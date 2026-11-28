#!/usr/bin/env bash
# Export only committed source and its identity for container package builds.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
cd "$root"
output=${1:?usage: scripts/ci/export-source.sh OUTPUT_DIRECTORY}
mkdir -p "$output"
output=$(realpath "$output")
[[ "$output" != "$root" && "$output" != "$root/"* ]]
if [[ -n $(ls -A "$output") ]]; then
    echo 'Use an empty output directory so old packages cannot be installed by mistake.' >&2
    exit 1
fi
if [[ -n $(git status --porcelain --untracked-files=no) ]]; then
    echo 'Commit the checkout first: the package must match the reported Git revision.' >&2
    exit 1
fi
mkdir -p "$output/input"
# Include hash-pinned renderer sources so distro/source-package builds need no
# Nix installation or network access. Nix is only used by this source exporter.
pdfium_sources=$(nix build .#pdfium-sources --no-link --print-out-paths)
mkdir -p "$output/source-stage"
git archive --format=tar --prefix=rummage-checkout/ HEAD | tar -xf - -C "$output/source-stage"
cp -R "$pdfium_sources" "$output/source-stage/rummage-checkout/third_party/pdfium-sources"
python3 packaging/pdfium/copyright.py "$pdfium_sources" \
    "$output/source-stage/rummage-checkout/packaging/debian/copyright"
tar --sort=name --mtime=@1 --owner=0 --group=0 --numeric-owner \
    -C "$output/source-stage" -cf - rummage-checkout | gzip -n > "$output/input/source.tar.gz"
python3 - "$output/input" <<'PY'
import hashlib, json, os, re, subprocess, sys
from pathlib import Path
out = Path(sys.argv[1])
version = re.search(r'project\(rummage VERSION ([0-9.]+)', Path('CMakeLists.txt').read_text())[1]
metadata = {
    'commit': subprocess.check_output(['git','rev-parse','HEAD'], text=True).strip(),
    'timestamp': subprocess.check_output(['git','show','-s','--format=%cd','--date=format-local:%Y%m%d%H%M%S','HEAD'], text=True, env={**os.environ, 'TZ':'UTC'}).strip(),
    'version': version,
    'sha256': hashlib.sha256((out/'source.tar.gz').read_bytes()).hexdigest(),
}
(out/'source.json').write_text(json.dumps(metadata, indent=2)+'\n')
PY
