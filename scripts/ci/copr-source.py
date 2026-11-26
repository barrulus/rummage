#!/usr/bin/env python3
"""Prepare an offline source-RPM tree from an exported, identifiable checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(source, output, channel, tag=None):
    metadata = json.loads((source / "source.json").read_text())
    for key, pattern in (("commit", r"[0-9a-f]{40}"), ("timestamp", r"[0-9]{14}"),
                         ("version", r"[0-9]+\.[0-9]+\.[0-9]+"), ("sha256", r"[0-9a-f]{64}")):
        if not isinstance(metadata.get(key), str) or not re.fullmatch(pattern, metadata[key]):
            raise ValueError(f"invalid source identity: {key}")
    archive = source / "source.tar.gz"
    if digest(archive) != metadata["sha256"]:
        raise ValueError("source archive SHA-256 does not match source.json")
    if channel not in ("stable", "development"):
        raise ValueError("unknown update channel")
    if channel == "stable":
        if tag != "v" + metadata["version"]:
            raise ValueError("stable builds require --tag vVERSION matching the source version")
        commit = subprocess.check_output(
            ["git", "rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}"], cwd=ROOT,
            text=True).strip()
        if commit != metadata["commit"]:
            raise ValueError("release tag does not point to the exported source commit")
        release = "1%{?dist}"
    else:
        if tag is not None:
            raise ValueError("--tag is only valid for the stable channel")
        release = "0." + metadata["timestamp"] + ".git" + metadata["commit"][:12] + "%{?dist}"
    with tarfile.open(archive, "r:gz") as source_tar:
        member = source_tar.getmember("rummage-checkout/packaging/rpm/rummage.spec")
        if not member.isfile() or member.size > 1024 * 1024:
            raise ValueError("source recipe must be a regular file smaller than 1 MiB")
        spec = source_tar.extractfile(member).read().decode("utf-8")
        if not any(m.name.startswith("rummage-checkout/third_party/pdfium-sources/")
                   and m.isfile() for m in source_tar):
            raise ValueError("export-source.sh must vendor pinned PDFium sources first")
    for name, value in (("Version", metadata["version"]), ("Release", release)):
        spec, count = re.subn(r"^" + name + r":.*$", name + ":        " + value, spec, flags=re.M)
        if count != 1:
            raise ValueError("recipe must have exactly one " + name + " field")
    if "Source0:        rummage-source.tar.gz" not in spec:
        raise ValueError("unexpected source archive name in recipe")
    output.mkdir(parents=True, exist_ok=False)
    for directory in ("SOURCES", "SPECS", "SRPMS"):
        (output / directory).mkdir()
    shutil.copyfile(archive, output / "SOURCES/rummage-source.tar.gz")
    recipe = output / "SPECS/rummage.spec"
    recipe.write_text(spec)
    identity = {**metadata, "channel": channel, "tag": tag,
                "spec_sha256": digest(recipe), "release": release}
    (output / "source.json").write_text(json.dumps(identity, indent=2) + "\n")
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="export-source.sh output/input directory")
    parser.add_argument("output", type=Path, help="new local rpmbuild directory")
    parser.add_argument("--channel", choices=("stable", "development"), required=True)
    parser.add_argument("--tag", help="existing vVERSION tag for a stable build")
    args = parser.parse_args()
    try:
        identity = prepare(args.input.resolve(), args.output.resolve(), args.channel, args.tag)
    except (OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        parser.error(str(error))
    print(f"Prepared {identity['channel']} source RPM input for {identity['commit']} in {args.output}")
    print("No build or remote submission was performed.")


if __name__ == "__main__":
    main()
