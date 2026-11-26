#!/usr/bin/env python3
"""Build and run bounded sanitizer/fuzzer checks in the pinned instrumentation shell."""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
FUZZERS = ("tiff", "camera", "picture", "dng", "ljpeg", "text", "svg", "archive")
THREAD_SOURCES = "*event_loop_test.cpp,*worker_pool_test.cpp,*file_watcher_test.cpp"
SEEDS = {
    "tiff": ("tests/data/image/tiff",),
    "camera": ("tests/data/image/dng",),
    "picture": ("tests/data/image",),
    "dng": ("tests/data/image/dng",),
    "ljpeg": (),
    "text": (),
    "svg": ("tests/fuzz/svg",),
    "archive": ("tests/data/archive",),
}


def configure(profile, build):
    return ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug", "-DRUMMAGE_WERROR=OFF",
            "-DRUMMAGE_BUILD_TESTS=" + ("OFF" if profile == "fuzz" else "ON"),
            "-DRUMMAGE_MEDIA=" + ("ON" if profile == "asan" else "OFF"),
            "-DRUMMAGE_PDF=" + ("ON" if profile == "asan" else "OFF"),
            "-DRUMMAGE_MODEL=" + ("ON" if profile == "asan" else "OFF"),
            "-DRUMMAGE_TERMINAL=" + ("ON" if profile == "asan" else "OFF"),
            "-DRUMMAGE_FONT=" + ("ON" if profile == "asan" else "OFF"),
            *( ["-DRUMMAGE_FUZZ=ON"] if profile == "fuzz" else
               ["-DRUMMAGE_SANITIZER=" + ("address,undefined" if profile == "asan" else "thread")])]


def fuzz_command(target, build, output, seconds):
    return [str(build / "tests/fuzz" / f"rummage-fuzz-{target}"),
            str(output / "corpus" / target), f"-max_total_time={seconds}",
            "-timeout=10", "-rss_limit_mb=2048", "-max_len=1048576", "-seed=1",
            "-artifact_prefix=" + str(output / "failures" / target) + "/"]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=("asan", "tsan", "fuzz"))
    parser.add_argument("output", type=Path, help="new directory outside the checkout")
    parser.add_argument("--fuzz-seconds", type=int, default=15)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args(argv)
    if not 1 <= args.fuzz_seconds <= 60 or not 1 <= args.jobs <= 16:
        parser.error("fuzz-seconds must be 1..60 and jobs must be 1..16")
    output = args.output.resolve()
    if output == ROOT or ROOT in output.parents or output.exists():
        parser.error("output must be a new directory outside the checkout")
    output.mkdir(parents=True)
    build = output / "build"
    report = {"profile": args.profile, "commands": [], "status": "running"}
    env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
           "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
           "TSAN_OPTIONS": "halt_on_error=1"}

    def save():
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(name, command, timeout):
        record = {"name": name, "argv": command, "timeout_seconds": timeout}
        report["commands"].append(record)
        save()
        print("+ " + " ".join(command), flush=True)
        with (output / (name + ".log")).open("w") as log:
            with subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                  stderr=subprocess.STDOUT, start_new_session=True) as proc:
                try:
                    result = proc.wait(timeout=timeout)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    record["timed_out"] = True
                    result = 124
        record["returncode"] = result
        save()
        if result:
            raise RuntimeError(f"{name} failed ({result}); see {output / (name + '.log')}")

    save()
    try:
        report["commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        report["dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True).strip())
        run("configure", configure(args.profile, build), 300)
        targets = ["rummage-fuzz-" + name for name in FUZZERS] if args.profile == "fuzz" else []
        if args.profile == "tsan":
            targets = ["rummage-tests"]
        run("build", ["cmake", "--build", str(build), "--parallel", str(args.jobs),
                      *(["--target", *targets] if targets else [])], 1800)
        if args.profile == "asan":
            listing = subprocess.check_output(
                ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True)
            names = {x["name"] for x in json.loads(listing)["tests"]}
            required = {"rummage-tests", "i18n-runtime", "media-tests", "pdf-tests", "font-tests", "model-tests",
                        "file-picker-portal", "action-notifications"}
            if not required <= names:
                raise RuntimeError(f"missing required tests: {sorted(required - names)}")
            report["excluded"] = {"memory-tests": "allocator accounting is changed by ASan"}
            run("tests", ["ctest", "--test-dir", str(build), "--output-on-failure",
                          "--no-tests=error", "--timeout", "240", "-E", "^memory-tests$"], 1200)
        elif args.profile == "tsan":
            report["scope"] = THREAD_SOURCES
            run("tests", [str(build / "tests/rummage-tests"), "--source-file=" + THREAD_SOURCES,
                          "--reporters=xml", "--out=" + str(output / "tests.xml")], 240)
            counts = ET.parse(output / "tests.xml").find(".//OverallResultsTestCases")
            if counts is None or int(counts.get("successes", "0")) == 0:
                raise RuntimeError("TSan selected no successful test cases")
        else:
            for target in FUZZERS:
                corpus = output / "corpus" / target
                corpus.mkdir(parents=True)
                (output / "failures" / target).mkdir(parents=True)
                (corpus / "empty").write_bytes(b"")
                (corpus / "text").write_bytes(b"hello\n\xef\xbb\xbfworld\x00\xff")
                for directory in SEEDS[target]:
                    for path in sorted((ROOT / directory).rglob("*")):
                        if path.is_file() and path.stat().st_size <= 1048576:
                            name = str(path.relative_to(ROOT)).replace("/", "_")
                            shutil.copyfile(path, corpus / name)
                run("fuzz-" + target, fuzz_command(target, build, output, args.fuzz_seconds),
                    args.fuzz_seconds + 45)
        report["status"] = "passed"
        return 0
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, ET.ParseError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        print(error, file=sys.stderr)
        return 1
    finally:
        save()


if __name__ == "__main__":
    sys.exit(main())
