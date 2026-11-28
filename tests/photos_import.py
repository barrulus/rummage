#!/usr/bin/env python3
"""Measure real isolated decoding of copied public test images; no personal photographs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--build-mode", default="unspecified")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.count <= 100000:
        parser.error("count must be between 1 and 100000")
    images = Path(__file__).parent / "data/image"
    names = ["blocks.jpg", "blocks.png", "blocks.webp", "blocks.gif", "blocks.jxl",
             "blocks.heic", "with-thumbnail.jpg", "tiff/rgb8-none.tif",
             "tiff/gray16-none.tif", "tiff-preview.tif"]
    with tempfile.TemporaryDirectory(prefix="rummage-photo-import-") as temporary:
        root = Path(temporary)
        source = root / "originals"
        source.mkdir()
        started = time.monotonic()
        for index in range(args.count):
            original = images / names[index % len(names)]
            folder = source / f"{index // 250:04d}"
            folder.mkdir(exist_ok=True)
            shutil.copy2(original, folder / f"photo-{index:06d}{original.suffix}")
        generated_seconds = time.monotonic() - started
        env = dict(os.environ)
        env.update({f"XDG_{name}_HOME": str(root / name.lower())
                    for name in ("CACHE", "DATA", "STATE", "CONFIG")})
        completed = subprocess.run([str(args.program.resolve()),
                                    str(root / "catalog/catalog.sqlite3"), str(source)],
                                   env=env, text=True, capture_output=True, timeout=3600)
        args.output.with_suffix(".log").write_text(completed.stderr)
        if completed.returncode:
            raise SystemExit(completed.stderr)
        report = json.loads(completed.stdout)
        report.update({"system": platform.platform(), "fixture_count": args.count,
                       "build_mode": args.build_mode,
                       "program_sha256": hashlib.sha256(args.program.read_bytes()).hexdigest(),
                       "fixture_sources": names, "fixture_generation_seconds": generated_seconds,
                       "cache": "fresh thumbnail/catalog cache; source pages warm from copies",
                       "limitation": "Repeated small real decoder fixtures; not a diverse personal collection or UI latency/scroll measurement"})
        if report["records"] != args.count:
            raise RuntimeError(f"Expected {args.count} records, received {report['records']}")
        if report["metadata_items"] != args.count or report["repeated_metadata_items"] != 0:
            raise RuntimeError("Every image must be processed once; unchanged rescan must skip metadata")
        for job in report["jobs"]:
            if job["state"] != "completed" or job.get("failures"):
                raise RuntimeError(f"Import did not complete cleanly: {job}")
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
