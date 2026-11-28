#!/usr/bin/env bash
# Shared website gate for local preflight and GitHub Pages CI (Linux x86_64).
set -euo pipefail
cd "$(dirname "$0")/../.."
HUGO_VERSION='0.170.1'
HUGO_SHA256='841a5462853aefc9942e4de1e8a5d75584b6a5181941e6ad3f1d745190e03cd8'
output=$(realpath -m "${1:-website/public}")
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
curl --fail --silent --show-error --location --retry 3 \
  "https://github.com/gohugoio/hugo/releases/download/v${HUGO_VERSION}/hugo_${HUGO_VERSION}_linux-amd64.tar.gz" \
  --output "$temporary/hugo.tar.gz"
printf '%s  %s\n' "$HUGO_SHA256" "$temporary/hugo.tar.gz" | sha256sum --check -
tar -xzf "$temporary/hugo.tar.gz" -C "$temporary" hugo
"$temporary/hugo" --source website --destination "$output" --cleanDestinationDir \
  --minify --panicOnWarning --printPathWarnings
python3 website/check.py "$output"
