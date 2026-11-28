#!/usr/bin/env python3
"""Run pinned sanitizer/fuzzer checks locally, retaining the toolchain and incremental builds."""
import argparse
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parents[2]


def paths(cache, root=ROOT):
    checkout = hashlib.sha256(os.fsencode(root.resolve())).hexdigest()[:12]
    return cache / "toolchain", cache / checkout


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?", choices=("all", "asan", "tsan", "fuzz"), default="all")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--fuzz-seconds", type=int, default=15)
    parser.add_argument("--cache-dir", type=Path,
                        default=Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "rummage-validation")
    args = parser.parse_args(argv)
    if not 1 <= args.jobs <= 16 or not 1 <= args.fuzz_seconds <= 60:
        parser.error("jobs must be 1..16 and fuzz-seconds must be 1..60")
    cache = args.cache_dir.expanduser().resolve()
    toolchain, checkout = paths(cache)
    cache.mkdir(parents=True, exist_ok=True)
    profiles = ("asan", "tsan", "fuzz") if args.profile == "all" else (args.profile,)
    for profile in profiles:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
        output = checkout / "reports" / f"{stamp}-{profile}"
        command = ["nix", "develop", ".#instrumentation", "--profile", str(toolchain), "--command",
                   "python3", str(ROOT / "scripts/ci/instrumentation.py"), profile, str(output),
                   "--build-dir", str(checkout / profile), "--jobs", str(args.jobs),
                   "--fuzz-seconds", str(args.fuzz_seconds)]
        print(f"{profile}: build {checkout / profile}\nReport: {output}", flush=True)
        result = subprocess.run(command, cwd=ROOT, check=False)
        if result.returncode:
            return result.returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
