#!/usr/bin/env python3
"""Run focused real-filesystem tests in a caller-provided disposable mount.

The caller provisions and identifies the backend; this tool never mounts devices,
changes servers, or infers one protocol's behavior from another. Logs and leftover
fixture names are preserved for review, including on failed tests.
"""

import argparse
import datetime
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--backend", required=True, help="Protocol/server/filesystem versions"
    )
    parser.add_argument(
        "--expected-fstype",
        required=True,
        help="Refuse tests if the target mount disappeared or is the wrong backend",
    )
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()

    def filesystem():
        return subprocess.run(
            [
                "findmnt",
                "--noheadings",
                "--output",
                "FSTYPE",
                "--target",
                str(args.target.resolve()),
            ],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()

    mounted = filesystem()
    if mounted != args.expected_fstype:
        parser.error(
            f"expected {args.expected_fstype}, found {mounted}; refusing misleading backend run"
        )
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="rummage-matrix-", dir=args.target.resolve()))
    environment = os.environ | {
        "TMPDIR": str(root),
        "LIBGL_ALWAYS_SOFTWARE": "1",
        "EGL_PLATFORM": "surfaceless",
    }
    groups = {
        "copy-move-replace-merge-cancel": "transfer:*",
        "rename-create": "operations:*",
        "extract": "extract:*",
        "archive-create": "compress:*",
        "undo-redo-history": "undo:*",
    }
    report = {
        "backend": args.backend,
        "kernel": platform.release(),
        "filesystem": mounted,
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "target": str(args.target.resolve()),
        "fixture": str(root),
        "groups": {},
    }
    for name, pattern in groups.items():
        if filesystem() != mounted:
            report["groups"][name] = {"status": "mount-lost", "leftovers": None}
            break
        command = [
            str(args.binary.resolve()),
            f"--test-case={pattern}",
            "--test-case-exclude=compress: more than 65535*",
        ]
        with (args.output / f"{name}.log").open("wb") as log:
            try:
                result = subprocess.run(
                    command,
                    env=environment,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=args.timeout,
                    check=False,
                )
                status = result.returncode
            except subprocess.TimeoutExpired:
                status = "timeout"
        if filesystem() != mounted:
            report["groups"][name] = {"status": "mount-lost", "leftovers": None}
            break
        summary = re.search(
            r"\[doctest\] test cases:\s*(\d+)\s*\|\s*(\d+) passed\s*\|\s*(\d+) failed",
            (args.output / f"{name}.log").read_text(errors="replace"),
        )
        counts = (
            dict(zip(("cases", "passed", "failed"), map(int, summary.groups())))
            if summary
            else None
        )
        if status == 0 and (not counts or counts["cases"] == 0):
            status = "no-test-evidence"
        report["groups"][name] = {
            "status": status,
            "counts": counts,
            "leftovers": sorted(p.name for p in root.iterdir()),
        }
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    report["leftovers_after_exit"] = (
        sorted(p.name for p in root.iterdir()) if filesystem() == mounted else None
    )
    if report["leftovers_after_exit"] == []:
        root.rmdir()
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return int(any(row["status"] != 0 for row in report["groups"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
